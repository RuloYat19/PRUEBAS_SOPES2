# Revisión F06 — Antivirus, blacklist y cuarentena

Fecha: 2026-10-21 · Revisor: Claude · Commits revisados: `4da51b3812`…`fbd997e993`

## Veredicto

**Aprobada** (segunda ronda). B1, B2 y A1–A7 están corregidos; ver «Segunda ronda» al final.

> Primera ronda: cambios requeridos, con 2 bloqueantes y 7 advertencias (detalle abajo).

La blacklist, la detección, las alertas, las listas, los endpoints y el dashboard cumplen el spec. La suite completa pasó en la VM: 8/8 suites, F06 con 13 fases. La comprobación en el navegador también está hecha.

El problema principal es de seguridad. Por un hueco del diseño (mío), el daemon, que corre como root, puede dejar con permisos `0000` archivos **fuera** del directorio vigilado. Para eso basta con cambiar ese directorio por un enlace simbólico. R4.2 promete justo lo contrario.

## Verificación independiente (sin root, en la VM)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all \| grep -ci warning` | 0 |
| `make -C ProgramaIntermedio pruebas` | código 0 (`test_blacklist: OK`, casos 9–15 de `test_analisis` ✓) |
| Árbol tras recompilar | `daemon_monitor` reproducible; queda sin ignorar `pruebas/test_blacklist` (A7) |
| `bash -n` de `pruebas_antivirus.sh` y `demo_antivirus.sh` | OK |
| Comentarios en `blacklist.c` y `analisis.c` | ninguno |
| `blacklist.c` | textos de R1.3 exactos; claves opcionales y obligatorias según R1.2; minúsculas; duplicados detectados en la tabla; la tabla nueva se arma fuera del mutex y el intercambio ocurre dentro; `guardar_atomico` con `mkstemp`, `fsync`, `fchown`/`fchmod` y `rename` ✓ |
| `analisis.c` | `analizar_abierto` común a los 3 contextos; `fchmod` sobre el mismo `fd` del hash; sin `LOW` para infectados; re-evaluación sin `Archivo modificado` y sin restaurar; amenazas sin duplicar con el mismo SHA; listas en anillo que no se vacían al cambiar la ruta ✓ |
| `sensor.c` | `ver_evaluada` tomada antes de la línea base; re-evalúa si la versión cambió ✓ |
| `http.c` | búfer de 1 MiB solo después de validar una sesión admin (401/403 sin leer el cuerpo); 413 por encima de 1 MiB; `archivos_despertar` tras la subida; `difundir_amenazas` con `*_desde` ✓ |
| `auth.c` | 5 filas nuevas (21 en total) ✓ |
| Dashboard | sin `innerHTML`; texto de confirmación exacto (`archivos.js:77`) ✓ |
| `pruebas_antivirus.sh` | trabaja sobre una copia de la blacklist; comprueba que el SHA del `blacklist.json` del repo no cambia; logins por stdin ✓ (ver B2) |

## Hallazgos bloqueantes

### B1 — La cuarentena puede salir del directorio vigilado cambiando el directorio por un enlace

Todas las aperturas que terminan en `fchmod` usan `open(ruta_completa, O_RDONLY|O_NOFOLLOW|…)`:
- `analisis_procesar`
- `analisis_linea_base`
- `analisis_reevaluar` (`analisis.c`, en el bucle de rutas copiadas)
- `analisis_cuarentena_manual`

`O_NOFOLLOW` solo protege el **último** componente de la ruta. El dueño de `~/so2_vigilado` controla su `$HOME`, así que puede reemplazar el directorio por un enlace.

**Escenario:**
1. Se crea `~/so2_vigilado/hostname`, que entra al listado.
2. `mv ~/so2_vigilado ~/x; ln -s /etc ~/so2_vigilado`.
3. Un admin pulsa **Cuarentena** sobre `…/so2_vigilado/hostname`. Pasa los cuatro controles:
   - está en el listado;
   - `ruta_en_directorio` sobre la cadena da bien;
   - `O_NOFOLLOW` no falla, porque `hostname` no es un enlace;
   - `S_ISREG` se cumple.
4. Resultado: `fchmod(/etc/hostname, 0000)` como root.

Lo mismo ocurre sin clic con la re-evaluación (`analisis_reevaluar` reabre **cada** ruta del listado al subir una blacklist) y con los eventos o la línea base. En esos casos el daemon además publica el SHA-256 de archivos que solo root puede leer en `/api/archivos`, que ve cualquier usuario común.

**Comprobado** (sin root, en el scratchpad): la misma secuencia `open(O_NOFOLLOW)` + `fchmod(fd, 0)` a través de un directorio cambiado por un enlace dejó en `0` el archivo de "sistema". Con `openat2` y `RESOLVE_NO_SYMLINKS`, la apertura falla con `ELOOP`.

El origen es mi diseño, que solo pedía `O_NOFOLLOW`. Ya lo corregí en `requirements.md` (R4.2) y en `design.md`.

**Corrección:**
- Un único helper en `analisis.c`, `static int abrir_sin_enlaces(const char *ruta, int flags)`:
  - `syscall(SYS_openat2, AT_FDCWD, ruta, &how, sizeof(how))`;
  - `how.flags = flags`;
  - `how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS`;
  - `<linux/openat2.h>`, kernel ≥ 5.6.
- Úsalo en las **cuatro** aperturas con `O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC`. La ruta vigilada ya es canónica (`realpath`), así que una ruta legítima no tiene enlaces.
- Cambia los respaldos de `analisis_reevaluar` y `analisis_cuarentena_manual`, que hoy usan `stat(ruta)` y siguen enlaces:
  - si la apertura falla con `EACCES`, ábrelo con `abrir_sin_enlaces(ruta, O_PATH|O_NOFOLLOW|O_CLOEXEC)` y usa `fstat` sobre ese `fd` para ver si está en `0000`;
  - con cualquier otro error (`ELOOP`, `ENOENT`, …), en la cuarentena manual responde `404`, y en la re-evaluación deja la entrada como está.
- `opendir` de `analisis_linea_base` puede quedarse como está: si el directorio es un enlace, cada `abrir_sin_enlaces` falla.
- **Prueba (sin root) en `test_analisis.c`, caso 16:**
  1. Directorio vigilado `D` con `f` en el listado, más un directorio externo `E/f` con modo `644` y contenido de muestra maliciosa.
  2. `rename(D, D_viejo)` y `symlink(E, D)`.
  3. Comprueba:
     - `analisis_cuarentena_manual(D/f)` devuelve `CUARENTENA_NO_LISTADO`;
     - `analisis_reevaluar` con una blacklist que contiene el hash de `E/f` no lo toca;
     - `E/f` sigue en `644`;
     - no hay alertas `HIGH` nuevas;
     - el `sha256` del listado no cambia.

### B2 — `pruebas_antivirus.sh` borra `~/so2_logs/pruebas_antivirus` sin haberlo creado

`:96` aborta si `DIR_VIGILADO` existe, pero no revisa `DIR_LOGS`. Después, `:108` hace `rm -rf "$DIR_VIGILADO" "$DIR_LOGS"` y el `trap` (`:68`) vuelve a borrar `DIR_LOGS`.

Es la misma regla de F05 B1/B3: un script no borra lo que no creó. Si la carpeta queda de una corrida abortada, o el usuario guardó ahí logs de una prueba anterior, se pierde sin aviso.

**Corrección:**
- En la comprobación de `:96`, revisa `DIR_VIGILADO` **o** `DIR_LOGS`, con el mismo mensaje y antes de `usuarios_prueba_crear`.
- Elimina el `rm -rf` de `:108`, que ya no hace falta porque se garantiza que ninguno existe.
- El `trap` puede seguir borrando ambos con `PREPARADO=1`.

## Advertencias (corregir en esta ronda; no bloquean por sí solas)

- **A1 — `fchmod` silencioso si cambia la generación.** `analizar_abierto` aplica `fchmod` **antes** de comprobar `generacion != gen`. Si la ruta cambió en medio, el archivo queda en `0000` sin alerta, sin registro de cuarentena y sin entrada en el listado. Comprueba la generación bajo `lock_archivos` antes del `fchmod`. Por ejemplo, haz una primera pasada que solo lea `generacion` y la entrada; luego aplica `fchmod` fuera del lock; luego vuelve a tomar el lock para registrar. Si la generación cambió entre la comprobación y el registro, igual registra la cuarentena y emite su alerta, porque el permiso ya se cambió.
- **A2 — JSON sin escapar en la respuesta de la subida.** `atender_blacklist_post` arma `{"firmas":%zu,"nombre":"%s"}` con `snprintf`. Un `nombre` válido según R1 como `Base "local"` produce un JSON inválido, y el dashboard muestra error aunque la subida funcionó. Usa `buf` + `json_cadena`, igual que en `atender_cuarentena_post`.
- **A3 — Búfer grande para cualquier método.** La pre-reserva de `cliente_leer` solo compara la ruta (`/api/blacklist`), no el método. Exige también `strcmp(met, "POST") == 0`.
- **A4 — `demo_antivirus.sh` se corta si la muestra ya está en cuarentena.** Con `set -e`, `sha256sum "$ruta"` falla con `Permission denied` cuando el daemon ya puso el archivo en `0000`. Pasa siempre al correr el script una segunda vez, y a veces en la primera, si el daemon gana la carrera. El script termina en la primera muestra. Usa `sha256sum "$ruta" 2>/dev/null || echo "(en cuarentena) ${ruta}"`.
- **A5 — `registrar` dentro de `lock_blacklist`.** `blacklist_iniciar` llama a `registrar(...)` con el mutex tomado (4 sitios), y el diseño dice que `lock_blacklist` es hoja. Copia el mensaje y regístralo después de soltar el lock. Además, cuando `stat` da bien pero `!S_ISREG`, `strerror(errno)` imprime un `errno` viejo; usa un texto fijo `No es un archivo regular`.
- **A6 — `idx` obsoleto en la cuarentena manual.** `analisis_cuarentena_manual` guarda `idx` en la primera sección crítica y lo usa en la segunda (`listado[idx].sha256`). Si el listado cambió en medio, el registro lleva el SHA de otro archivo. Toma el SHA del mismo recorrido que marca `cuarentena = 1` (o calcula el hash del `fd`, que está abierto de todos modos).
- **A7 — `pruebas/test_blacklist` no está en `.gitignore`.** Después de `make pruebas` aparece como `??`. Agrégalo junto a los otros binarios de prueba (`.gitignore:36`).

## Lo que no se pide

- El orden de R3.1: el `fchmod` ocurre antes de emitir la alerta de detección, pero las dos alertas salen en el orden correcto y sobre el mismo `fd`. Se acepta.
- `Firma <n>: falta sha256` cuando un elemento de `firmas` no es un objeto: R1.3 no tiene un texto para ese caso, y este es razonable.
- Pedir un JSON de más de 1 MiB sin cookie da `413` y no `401`. R2.3 y R2.5 se pisan en ese caso; cualquiera de los dos es válido.

## Siguiente paso

El implementador corrige B1, B2 y A1–A7 en commits `fix: F06 …`, sin push:
- B1 con su prueba (caso 16) y los textos actualizados de R4.2 y del diseño;
- después, `make pruebas`, 0 warnings, el binario recompilado y `tasks.md` actualizado.

El usuario vuelve a correr `sudo scripts/correr_pruebas.sh`. No hace falta repetir la comprobación en el navegador, salvo por A2 (subir una blacklist con comillas en `nombre`) y A4 (correr `scripts/demo_antivirus.sh` dos veces).

## Segunda ronda (2026-10-21)

Commits: `335525bd4f` (correcciones) y `fbd997e993` (T7).

| Hallazgo | Corrección | Comprobado |
|---|---|---|
| B1 | `abrir_sin_enlaces` (`openat2` con `RESOLVE_NO_SYMLINKS \| RESOLVE_NO_MAGICLINKS`) en las 4 aperturas de `analisis.c`. El respaldo actúa solo ante `EACCES`, con `O_PATH` + `fstat`. | El caso 16 pasa con el código nuevo. Con el `analisis.c` de `1ee887c2b7` **falla** (la cuarentena manual atraviesa el enlace), así que la prueba sí detecta la regresión. |
| B2 | Se aborta si existen `DIR_VIGILADO` o `DIR_LOGS`, antes de crear usuarios; se quitó el `rm -rf` de la preparación. | Lectura del script ✓ |
| A1 | La generación se comprueba bajo el lock antes del `fchmod`. Si cambia después, igual se registran la cuarentena y su alerta. | Lectura ✓ |
| A2 | Respuesta construida con `buf` + `json_cadena`. | Lectura ✓ |
| A3 | Se exige `POST` además de la ruta para el búfer grande. | Lectura ✓ |
| A4 | `sha256sum … \|\| echo "(en cuarentena) …"`. | Demo corrida dos veces con una muestra en `0000`: termina con código 0 y crea o lista las 4 ✓ |
| A5 | `registrar` fuera de `lock_blacklist` (6 sitios); texto `No es un archivo regular`. | Lectura ✓ |
| A6 | El SHA se toma del mismo recorrido que marca `cuarentena = 1`. | Lectura ✓ |
| A7 | `**/pruebas/test_blacklist` en `.gitignore`. | Árbol limpio tras `make pruebas` ✓ |

Verificación propia: `make clean all` con 0 warnings, `make pruebas` con código 0 (16 casos de `test_analisis`, `test_blacklist: OK`), árbol limpio y sin comentarios.

En la VM, según el reporte: `sudo scripts/correr_pruebas.sh` 8/8 suites (F06 33/33), 0 fallidas, 0 usuarios residuales y el `blacklist.json` del repositorio intacto.

