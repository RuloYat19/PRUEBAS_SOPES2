# F07 — Logwatch e IPC · Requisitos

## Introducción

Completar el análisis de palabras clave del enunciado (§2.2.2.2.10, §2.2.4.6 y la tabla de permisos):
- el **hilo IPC** sigue bloqueado en `sys_ipc_channel_receive`, sin polling, y convierte cada notificación en la alerta `MEDIUM` `Palabra clave detectada en log` (ya existe desde F00/F02; aquí se verifica);
- el administrador **crea** logwatch nuevos desde el dashboard (ruta central, rutas a vigilar y palabra clave);
- el administrador ve la **tabla de logwatch activos**, cada uno con su ID, y **finaliza** cualquiera de ellos.

El `log_watch` del kernel (Práctica 3) corre como root:
- sigue enlaces;
- lee cualquier archivo;
- crea o agrega la ruta central en cualquier lugar;
- las syscalls 550/551 no verifican permisos.

Por decisión del usuario (2026-10-21):
- las rutas que acepta el dashboard se limitan al **directorio de logs** del daemon (R2.3);
- la falta de permisos en 550/551 **se documenta** como limitación conocida de P3 (F08) y no se corrige en el kernel.

## Glosario

- **Logwatch:** un monitoreo del kernel creado con `sys_start_log_watch(rutas, cantidad, ruta_central, palabra)`. Tiene un ID entero > 0 y un kthread `log_watch_<id>` que revisa las rutas cada 3 s.
- **Directorio de logs (base):** el directorio de la ruta central configurada, después de `realpath`. Con la configuración por defecto es `~/so2_logs`.
- **Registro:** la lista en memoria del daemon con los logwatch que él creó (los de `daemon.conf` y los del dashboard). El kernel no ofrece una syscall para listar.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Registro de logwatch

1. AL arrancar, EL daemon DEBERÁ registrar cada logwatch que crea desde `daemon.conf` (uno por palabra de `log_palabras`) con `origen` `configuracion` y `usuario` vacío.
2. Cada entrada del registro DEBERÁ guardar:
   - `id`, `palabra`, `central` y `rutas` (absolutas, de 1 a 8);
   - `origen` (`configuracion` o `dashboard`), `usuario` (quien lo creó; vacío si es de configuración) y `creado` (`AAAA-MM-DD HH:MM:SS`).
3. El registro DEBERÁ admitir como máximo **16** logwatch activos y estar protegido por su propio mutex. Ninguna syscall se llama con ese mutex tomado.
4. AL apagarse, EL daemon DEBERÁ finalizar con `sys_stop_log_watch` **todos** los logwatch del registro, los de configuración y los del dashboard. Esto reemplaza al arreglo `watch_ids` de `main.c`.
5. Los logwatch creados desde el dashboard NO se guardan en disco: al reiniciar, el daemon solo recrea los de `daemon.conf`.

## R2 — Crear logwatch (`POST /api/logwatch`, rol `admin_user`)

1. El cuerpo DEBERÁ ser `{"palabra":"<palabra>","central":"<ruta>","rutas":["<ruta>", …]}` (exactamente esas tres claves).
2. Validación, en este orden. Cada falla responde con el texto exacto indicado, y en ese caso no se llama a la syscall ni cambia el registro:
   1. el cuerpo no es un objeto JSON, le falta una clave, tiene otra clave o algún tipo no coincide → `400` `Cuerpo invalido`;
   2. `palabra` no tiene entre 1 y 63 bytes, o contiene `|` o caracteres de control (< 0x20 o 0x7f) → `400` `Palabra invalida`;
   3. `rutas` no tiene entre 1 y 8 elementos → `400` `Se requieren entre 1 y 8 rutas`;
   4. `central` no cumple R2.3 → `400` `Ruta central invalida`;
   5. una ruta vigilada no cumple R2.3, no existe o no es un archivo regular → `400` `Ruta invalida: <ruta>` (la primera que falla, tal como llegó);
   6. una ruta vigilada aparece dos veces → `400` `Ruta repetida: <ruta>`;
   7. `central` es una de sus propias rutas vigiladas → `400` `La ruta central no puede ser un archivo vigilado`;
   8. `central` es una ruta vigilada de un logwatch del registro, o una de las rutas vigiladas es la `central` de un logwatch del registro → `409` `Conflicto con el logwatch <id>: un log central no puede vigilarse` (el kernel escribiría sus propias coincidencias en un ciclo sin fin);
   9. ya hay 16 logwatch en el registro → `409` `Limite de 16 logwatch alcanzado`.
3. **Regla de ruta** (para `central` y cada ruta vigilada):
   - es absoluta, de 1 a 255 bytes, y está **directamente** dentro del directorio de logs: `<base>/<nombre>`, sin `/` en `<nombre>`;
   - `<nombre>` no es `.` ni `..`, y la ruta no contiene `|` ni caracteres de control;
   - ningún componente de la ruta es un enlace: se comprueba con `openat2` y `RESOLVE_NO_SYMLINKS`, la misma regla que F06;
   - para la ruta vigilada, el archivo existe y es regular;
   - para la ruta central, si existe es un archivo regular; si no existe, el daemon la crea con modo `0644` y dueño igual al del directorio de logs.
4. Si todo es válido, EL daemon DEBERÁ llamar a `sys_start_log_watch`:
   - si devuelve un ID → lo registra con `origen` `dashboard` y el usuario de la sesión, y responde `200` con la entrada (R4.2);
   - si falla → `500` `No se pudo crear el logwatch: <strerror>`, sin cambiar el registro.
5. Crear un logwatch NO genera alerta (el catálogo del enunciado no la tiene). EL daemon DEBERÁ escribir en su log `logwatch <id> creado por <usuario>: '<palabra>' en <n> archivos`.

## R3 — Finalizar logwatch (`DELETE /api/logwatch?id=<id>`, rol `admin_user`)

1. `id` falta, no es un entero en base 10 o es ≤ 0 → `400` `ID invalido`.
2. `id` no está en el registro → `404` `Logwatch no encontrado`, sin llamar a la syscall. El daemon solo finaliza logwatch que él mismo creó.
3. Si está, EL daemon DEBERÁ llamar a `sys_stop_log_watch(id)`:
   - devuelve 0 → quita la entrada y responde `200` `{"id":<id>,"finalizado":true}`;
   - falla con `ESRCH` (alguien lo detuvo fuera del daemon) → quita la entrada y responde `404` `Logwatch no encontrado`;
   - cualquier otro error → `500` `No se pudo finalizar el logwatch: <strerror>`, sin quitar la entrada.
4. Se puede finalizar cualquier logwatch del registro, incluidos los de configuración.
5. EL daemon DEBERÁ escribir en su log `logwatch <id> finalizado por <usuario>`.

## R4 — Listado (`GET /api/logwatch`, rol `admin_user`)

1. La respuesta DEBERÁ ser `{"base":"<directorio de logs>","logwatch":[…]}`, ordenada por `id` ascendente.
2. Cada elemento DEBERÁ tener `id`, `palabra`, `central`, `rutas` (arreglo), `origen`, `usuario` y `creado`.

## R5 — Hilo IPC

1. El hilo `ipc` DEBERÁ seguir bloqueado en `sys_ipc_channel_receive`. Sin actividad en los logs, su contador `voluntary_ctxt_switches` NO DEBERÁ aumentar.
2. CUANDO llegue una notificación `EVT|…`, el daemon DEBERÁ emitir `Palabra clave detectada en log` (`MEDIUM`, tipo `logs`) con el archivo y la línea (comportamiento de F02, sin cambios). Esto vale para los logwatch de configuración y para los del dashboard.
3. Los contratos de `/api/alertas` y `event: alerta` no cambian.

## R6 — Dashboard

1. EL administrador DEBERÁ ver un panel **Logwatch** con:
   - el texto `Las rutas deben estar dentro de <base>`;
   - un formulario con **Palabra clave**, **Ruta central** (prellenada con `<base>/`) y **Rutas a vigilar** (un área de texto, una ruta por línea, de 1 a 8);
   - el botón **Crear logwatch**.
2. Debajo, una tabla de logwatch activos con ID, Palabra, Ruta central, Rutas vigiladas, Origen, Creado por y Fecha, y en cada fila el botón **Finalizar**:
   - el botón pide confirmación con el texto exacto `¿Finalizar el logwatch <id> (<palabra>)?`;
   - la tabla se recarga con `GET /api/logwatch` al abrir o reabrir el SSE, y después de crear o finalizar.
3. El resultado o el error de crear y finalizar se muestran en un aviso con el texto `error` de la respuesta.
4. EL usuario común NO DEBERÁ ver el panel **Logwatch**. Las alertas `Palabra clave detectada en log` siguen visibles para todos en el panel de alertas.
5. Solo `textContent`; nada de `innerHTML`.

## R7 — Pruebas y paridad

1. `make -C ProgramaIntermedio pruebas` DEBERÁ incluir `pruebas/test_logwatch.c` (validación, registro, crear y finalizar con syscalls simuladas). Todo sin root y con código 0.
2. EL proyecto DEBERÁ incluir `scripts/pruebas_logwatch.sh` (`sudo`, VM, puerto **8092**) que verifique R1–R5 contra el daemon y el kernel reales, sumado a `correr_pruebas.sh`. Trabaja en `~/so2_logs/pruebas_logwatch/`: aborta si ya existe y solo borra lo que creó.
3. `README.md` DEBERÁ documentar:
   - los endpoints y el panel;
   - la regla de rutas de R2.3;
   - la limitación conocida: el kernel de P3 vuelve a abrir las rutas por nombre cada 3 s y sigue enlaces, así que el dueño del directorio de logs podría cambiar un archivo por un enlace después de crear el logwatch; y 550/551 no verifican permisos.
4. Las suites anteriores DEBERÁN seguir con `0 FALLIDAS`.
5. `make -C ProgramaIntermedio clean all` DEBERÁ terminar sin warnings, con `daemon_monitor` versionado.
