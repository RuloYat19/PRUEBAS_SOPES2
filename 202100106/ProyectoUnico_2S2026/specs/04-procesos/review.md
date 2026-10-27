# Revisión F04 — Procesos (top 20, PID, kill)

Fecha: 2026-10-18 · Revisor: Claude · Commits revisados: `ac0a460f66`…`5340ba7890`

## Veredicto

**Aprobada** (tercera ronda). B1, B2, B3 y las dos advertencias están corregidos. `sudo scripts/correr_pruebas.sh` pasó completo y el usuario confirmó la comprobación en el navegador (T8.6). Ver "Tercera ronda" al final.

> Primera ronda: cambios requeridos (1 bloqueante). El código en C y el dashboard cumplían el spec; el bloqueante estaba en `pruebas_procesos.sh`.

## Verificación independiente (sin root, en la VM)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all \| grep -ci warning` | 0 |
| `make -C ProgramaIntermedio pruebas` | código 0 |
| Árbol de trabajo tras recompilar | limpio (binario reproducible) |
| `procesos_validar_kill` (`procesos.c:152`) | orden de guardas correcto: rango → PID 1 → daemon por `pid` o `Tgid` → kernel (`PF_KTHREAD` en el campo 9 o PID 2) → no líder → OK; `Tgid` ilegible cuenta como no líder (falla cerrado) ✓ |
| `atender_kill` (`http.c:356`) | mensajes y códigos de R4; alerta solo si `kill` tuvo éxito ✓ |
| `procesos_tomar` | vuelve a comprobar `procesos_activos()`; la lista anterior se libera fuera del mutex; `eventfd_write` sin locks ✓ |
| Bucle de `hilo_monitoreo` (`metricas.c:295`) | espera sin límite si ambos están apagados; `pendiente_muestra` cubre la reactivación; el apagado lo despierta ✓ |
| `metricas_tomar` | comprobación de `metricas_activas()` bajo `lock_metricas` (corrección F03) ✓ |
| Dashboard | sin `innerHTML`; sin sondeo de `/api/estado`; `api.sesion()` cada 60 s; el botón **Terminar** solo se crea si `auth.esAdmin()`; `confirm()` antes del `POST` ✓ |
| Comentarios en archivos nuevos o tocados | ninguno |

## Hallazgo bloqueante

### B1 — `pruebas_procesos.sh` lee `historial` en lugar de `alertas`

`/api/alertas` responde `{"alertas":[…]}` (F02 R3.1), pero el script busca la clave `"historial"` en cuatro lugares. `.get("historial", [])` siempre devuelve una lista vacía:

| Línea | Fase | Efecto |
|---|---|---|
| `:393` | 10 — alerta `Proceso matado` | no la encuentra → **falla** |
| `:437` | 11 — alerta de desactivación | **falla** |
| `:491`, `:494` | 13 — "sin alerta nueva" | compara `0 == 0` → **pasa sin probar nada** |
| `:542` | 14 — alerta de activación | **falla** |

**Corrección:** usar `"alertas"` en los cuatro lugares. En la fase 13, comparar el `id` máximo (`alertas[0]["id"]`) en lugar de la cantidad, como hace `pruebas_metricas.sh:406`: la cantidad se satura en 200 y entonces tampoco detectaría una alerta nueva.
- Corregido: en `scripts/pruebas_procesos.sh` se cambió `"historial"` por `"alertas"` en las fases 10, 11 y 14. En la fase 13 se compara el `id` máximo (`d["alertas"][0]["id"] if d.get("alertas") else 0`) antes y después de re-desactivar.
- Evidencia: `bash -n scripts/pruebas_procesos.sh` → código 0; `./scripts/pruebas_procesos.sh` sin root → código 1 y "Ejecuta con sudo".

## Advertencias (no bloquean)

1. **`procesos_validar_kill` falla abierto si no puede leer `stat`.** Si `<raiz>/<pid>/stat` no se abre o no se puede interpretar, se salta la guarda de hilos del kernel y la función puede devolver `KILL_OK`. Con `/proc` real es casi imposible (el `status` acaba de leerse), pero una guarda de seguridad debe fallar cerrada: si `stat` no se lee → `KILL_NO_EXISTE`; si no se interpreta → `KILL_KERNEL`. Agregar el caso al `/proc` falso de `test_procesos.c`.
   - Corregido: en `procesos_validar_kill` (`ProgramaIntermedio/procesos.c`), si `stat` no se abre retorna `KILL_NO_EXISTE`; si la lectura está vacía, no contiene `)` o no parsea 7 campos, retorna `KILL_KERNEL`. En `ProgramaIntermedio/pruebas/test_procesos.c` se añadieron los procesos 701 (sin `stat`) y 702 (`stat` sin formato) al `/proc` falso de prueba y se asertaron `KILL_NO_EXISTE` y `KILL_KERNEL`.
   - Evidencia: `make -C ProgramaIntermedio pruebas` → código 0.
2. **`http.c:377`: `(long)j_pid->valuedouble` con un número enorme (`1e300`) o fuera de rango es comportamiento indefinido en C.** En x86 hoy termina en 400, pero conviene comprobar `valuedouble >= 1 && valuedouble <= PID_MAXIMO` **antes** de convertir.
   - Corregido: en `atender_kill` (`ProgramaIntermedio/http.c`) se comprueba `j_pid->valuedouble >= 1.0 && j_pid->valuedouble <= (double)PID_MAXIMO` y que no tenga decimales antes de convertir a `long`.
   - Evidencia: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → 0; `make -C ProgramaIntermedio pruebas` → código 0.

## Nota

La fase 13 nueva de `pruebas_metricas.sh` (`intervalo = 5`, umbral 1 s) ya distingue la muestra inmediata. Queda una probabilidad de alrededor de 1/5 de que un tic normal caiga dentro de ese segundo; es aceptable para una prueba de integración.

## Segunda ronda — `sudo scripts/correr_pruebas.sh` (commits `4b7f93eeeb`, `ae60b30f00`)

Resultado: F01 21/0, F02 15/0, F03 18/0 (con la fase 13 nueva), integración 38/0, 0 usuarios residuales. **Fallan dos suites**, por dos errores en las pruebas, no en el daemon:

### B2 — Fase 10 de `pruebas_procesos.sh`: busca el motivo en `tipo` y además mata el proceso dos veces

- `:396` compara `a.get("tipo") == "Proceso matado"`. El texto del motivo está en `motivo`; `tipo` vale `"sistema"`. La alerta nunca se encuentra y la fase falla aunque el `kill` funcionó (la salida muestra `código: 200, muerto: 1`).
- `:376–377` hacen **dos** `POST /api/proceso/kill` seguidos: uno para el cuerpo y otro para el código. El segundo llega cuando el proceso ya es un zombi (el script todavía no lo recogió), `kill()` sobre un zombi devuelve 0 y el daemon emite una **segunda** alerta `Proceso matado`. La prueba debe hacer una sola petición (`-o archivo -w '%{http_code}'`).
- **Corrección:** comparar `a.get("motivo") == "Proceso matado"` y comprobar también que la descripción contenga el nombre y `(PID <pid>)`; una sola petición de kill; verificar que haya exactamente una alerta nueva con ese PID.
- Corregido: en `scripts/pruebas_procesos.sh` fase 10 se unificó la petición de kill a una sola invocación de `curl` guardando el cuerpo en un archivo temporal (`mktemp_rastreado`) y el código con `-w '%{http_code}'`. Se obtiene `id_max_previo` antes de la llamada y se verifica que exista exactamente una alerta nueva para ese PID con `motivo == "Proceso matado"`, `severidad == "MEDIUM"`, y descripción con `nombre`, `(PID <pid>)` y `so2t_admin`. En las fases 11 y 14 se añadió la comprobación de `motivo` (`"Monitoreo del sistema desactivado"` y `"Monitoreo del sistema activado"`).
- Evidencia: `bash -n scripts/pruebas_procesos.sh` → código 0.

### B3 — `test_config` caso 6 falla cuando las pruebas unitarias corren con `sudo`

- `correr_pruebas.sh:19` ejecuta `make … pruebas` como root. Con `sudo`, `SUDO_USER` está definido y `geteuid() == 0`, así que `obtener_home()` usa el home de `SUDO_USER` (F01 R7), no el `HOME` que fija la prueba. El caso 6 no limpia `SUDO_USER` y el caso 9 asume que el proceso no es root.
- Además, correr `make` como root deja `*.o`, `daemon_monitor` y los binarios de prueba **con dueño root** dentro del repositorio (comprobado: todos los `.o` y `daemon_monitor` son de root ahora).
- **Corrección:**
  1. `correr_pruebas.sh`: compilar y correr las pruebas unitarias como el usuario normal (`sudo -u "$SUDO_USER" make …`); solo las suites de integración necesitan root.
  2. `test_config.c`: el caso 6 hace `unsetenv("SUDO_USER")` antes de cargar; el caso 9 comprueba la rama que corresponde a `geteuid()` (como root, `SUDO_USER=root` debe dar `/root`; sin root, debe ignorarse). Así la prueba pasa en ambos modos.
  3. Recuperar la propiedad de los archivos con `sudo chown -R "$USER": ProgramaIntermedio` (paso manual del usuario, una vez).
- Corregido: en `scripts/correr_pruebas.sh` la suite de compilación y pruebas unitarias en C se ejecuta con `sudo -u "$SUDO_USER" make …` cuando `$SUDO_USER` está presente. En `ProgramaIntermedio/pruebas/test_config.c`: el caso 6 ejecuta `unsetenv("SUDO_USER")` antes de cargar la configuración; el caso 9 comprueba según `geteuid()` (`assert(strcmp(c.ruta_vigilada, "/root/x") == 0)` si es root; `assert(strcmp(c.ruta_vigilada, "/tmp/fake_home/x") == 0)` si no es root).
- Evidencia: `make -C ProgramaIntermedio clean all pruebas` → código 0 (como usuario normal, 0 warnings); `unshare -r env HOME=/root SUDO_USER=sudockentucky make -C ProgramaIntermedio pruebas` → código 0 (simulando ejecución con sudo); `bash -n scripts/correr_pruebas.sh` → código 0.

## Pendiente en VM (requiere `sudo`, después de corregir B2 y B3)

```bash
make -C ProgramaIntermedio clean all pruebas
sudo scripts/pruebas_procesos.sh         # 0 FALLIDAS
sudo scripts/pruebas_metricas.sh         # 0 FALLIDAS (fase 13 con intervalo 5)
sudo scripts/pruebas_alertas.sh          # 0 FALLIDAS
sudo scripts/pruebas_auth.sh             # 0 FALLIDAS
sudo scripts/pruebas_integracion.sh      # 0 FALLIDAS, 38 aprobadas
getent passwd | grep -c '^so2t_'         # 0
```

Y la comprobación en el navegador de T8.6.

## Tercera ronda — cierre (commits `c76d8c2ad7`, `5340ba7890`)

| Chequeo | Resultado |
|---|---|
| `c76d8c2ad7` B2 | una sola petición de kill; compara `motivo`, severidad, nombre, `(PID <pid>)` y usuario; exactamente una alerta nueva para el PID ✓ |
| `c76d8c2ad7` B3 | pruebas unitarias con `sudo -u "$SUDO_USER"`; `test_config` caso 6 limpia `SUDO_USER` y caso 9 depende de `geteuid()` ✓ |
| `5340ba7890` (fuera de ronda) | el aviso de procesos desactivados tapaba el interruptor: cabecera del panel con `z-index` y aviso desplazado; `metricas.js` solo alterna los avisos de sus 4 paneles ✓ |
| `sudo scripts/correr_pruebas.sh` (usuario, VM) | unitarias OK, F04 20/20, F03 18/18, F02 15/15, F01 21/21, integración 38/38, 0 usuarios `so2t_*` |
| Propiedad de artefactos | ningún archivo de `ProgramaIntermedio/` pertenece a root |
| Navegador (usuario, T8.6) | top 20 en vivo, **Terminar** con confirmación y alerta ámbar, error con PID 1, aviso del interruptor, vista de usuario común sin controles, sesión viva > 30 min ✓ |
