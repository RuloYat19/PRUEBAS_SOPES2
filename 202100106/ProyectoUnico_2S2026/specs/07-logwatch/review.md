# Revisión F07 — Logwatch e IPC

Fecha: 2026-10-22 · Revisor: Claude · Commits revisados: `679cf3b0bd`…`7ddd72074d`

## Veredicto

**Aprobada** (primera ronda), sin bloqueantes. Las 6 advertencias pasan a F08 y se corrigen allí antes de la entrega.

- La regla de rutas, el registro, los textos exactos, los permisos y el panel cumplen el spec.
- La suite completa pasó en la VM: 9/9 suites, F07 con 39 comprobaciones.
- La comprobación en el navegador está hecha (T6, `625060d998`).

## Verificación independiente (sin root, en la VM)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all \| grep -ci warning` | 0 |
| `make -C ProgramaIntermedio pruebas` | código 0: `test_logwatch` casos 1–9 ✓, `test_analisis` caso 16 ✓ (sigue en verde tras mover `abrir_sin_enlaces`) |
| Árbol tras recompilar | limpio (binario reproducible, `test_logwatch` ignorado) |
| Comentarios en `logwatch.c`, `rutas.c`, `test_logwatch.c`, `logwatch.js` | ninguno |
| `bash -n scripts/pruebas_logwatch.sh` | OK |
| `rutas.c` | `openat2` con `RESOLVE_NO_SYMLINKS \| RESOLVE_NO_MAGICLINKS`; `mode` solo con `O_CREAT`; `analisis.c` usa el helper compartido en sus 4 sitios ✓ |
| `logwatch.c` | Regla de ruta de R2.3: `ruta_en_directorio` + `.`/`..` + `\|`/control + `openat2 O_PATH` + `S_ISREG`; la central acepta `ENOENT`. Orden y textos de R2.2.1–R2.2.9 exactos; ciclo en las dos direcciones. `lock_logwatch` como hoja, con las syscalls fuera del lock. Central creada con `O_EXCL` y `fchown` al dueño de la base ✓ |
| `main.c` | base = `dirname(realpath(log_central))`; la configuración no pasa por la validación; `watch_ids[]` eliminado; `logwatch_finalizar_todos()` antes de `detener_hilo_ipc` ✓ |
| `http.c` | `extraer_id` con `strtol`, verificación del final y rango; 200/400/404/409/500 como en R2–R4; líneas de log de R2.5/R3.5 fuera del lock ✓ |
| `auth.c` / `test_auth.c` | 3 filas nuevas (24 en total) ✓ |
| Dashboard | sin `innerHTML`; confirmación exacta (`logwatch.js:144`); `central` prellenada con `base + '/'`; recarga al abrir el SSE y al iniciar sesión ✓ |
| `pruebas_logwatch.sh` | Aborta antes de crear usuarios si existe `DIR_LOGS` o hay otro `daemon_monitor`; `rm -rf` solo de `DIR_LOGS` con `PREPARADO=1`. Logins por stdin. Reposo del hilo `ipc` sin tolerancia (`V1 == V2` en 5 s). Kthreads comparados antes y después del `SIGTERM`; `/etc/x` no creado ✓ |

## Advertencias (pasan a F08)

- **A1 — Claves sin distinguir mayúsculas.** `logwatch_validar` usa `cJSON_GetObjectItem`, que no distingue mayúsculas. `{"PALABRA":…,"Central":…,"Rutas":[…]}` se acepta, aunque R2.1 pide exactamente `palabra`, `central` y `rutas`. Usa `cJSON_GetObjectItemCaseSensitive` y agrega ese caso a `test_logwatch` (caso 1).
- **A2 — Falta el caso de base cambiada por un enlace.** El diseño §6.1.1 lo pide, pero `test_logwatch` no prueba una base reemplazada por un enlace (`rename` + `symlink`). La implementación sí lo rechaza (`ELOOP` → inválida); falta la prueba que lo fije.
- **A3 — `trap` después de crear los usuarios.** En `pruebas_logwatch.sh`, `usuarios_prueba_crear` (`:45`) corre antes del `trap limpiar EXIT` (`:74`). Si la creación falla a medias, o hay un Ctrl-C en ese tramo, quedan usuarios `so2t_*`. Mueve las variables, `limpiar` y el `trap` antes de crear usuarios: `PREPARADO=0` ya protege el directorio.
- **A4 — Claves inexistentes en la configuración de prueba.** La configuración de la prueba usa `dashboard_dir`, `log_daemon` y `blacklist_ruta`. Ninguna existe, así que el daemon registra 3 `clave desconocida` y usa los valores por defecto. Usa `dashboard` y `blacklist`, y quita `log_daemon`.
- **A5 — La fase 9 aprueba sin comprobar.** `aprobar "15 logwatch de prueba finalizados correctamente"` no mira el código de cada `DELETE`. La fase 10 lo cubre de forma indirecta (lista vacía), pero comprueba que cada `DELETE` devuelva `200`.
- **A6 — "Detenido" aunque `stop` falle.** `logwatch_finalizar_todos` registra `logwatch <id> detenido` sin mirar el resultado de `stop`. El `main.c` anterior registraba `sys_stop_log_watch(<id>) fallo: <strerror>`, así que es una regresión en el log de apagado. Registra el éxito o el error según el resultado.

## Lo que no se pide

- Si `start` falla después de crear la central, el archivo vacío queda en la base. Es inofensivo (dueño = dueño de la base) y el usuario puede borrarlo.
- `extraer_id` acepta `id=+5` (`strtol`). Es aceptable.
- `GET /api/logwatch` no revisa `resp.fallo`, igual que otros listados chicos.

## Siguiente paso

F08 (documentación y entrega) incluye:
- A1–A6;
- las limitaciones conocidas de P3 en el Manual Técnico: el kernel sigue enlaces al reabrir las rutas cada 3 s, y las syscalls 550/551 no verifican permisos.
