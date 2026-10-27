# F05 — Sensor eBPF e integridad de archivos · Requisitos

## Introducción

Implementar el monitoreo de archivos del enunciado (§2.2.2, §2.2.3.3, §2.2.4.3 y la tabla de permisos):
- un programa **eBPF** (Clang/LLVM) que intercepta la creación y modificación de archivos **directamente dentro** del directorio vigilado, ignora al propio daemon y envía `ruta`, `flags` y `PID` por un **Ring Buffer**;
- el daemon lo carga con **libbpf**, escribe la ruta en un mapa con `bpf_map_update_elem()` y consume los eventos en el hilo `archivos`;
- el daemon calcula el **SHA-256**, compara el hash y el timestamp con el último análisis, y genera `Archivo modificado` (`LOW`);
- el administrador activa y desactiva el escaneo (cargar y descargar el eBPF) y cambia la ruta vigilada desde el dashboard; todos ven el listado de archivos analizados.

Fuera de alcance (F06): blacklist, `Hash malicioso detectado`, cuarentena y estado `infectado`. En F05 todos los archivos quedan `benigno`; el modelo de datos ya reserva el estado `infectado`.

## Glosario

- **Directorio vigilado:** ruta absoluta y canónica (`realpath`) de un directorio; se guarda en el mapa del sensor con `/` al final.
- **Directamente dentro:** la ruta abierta empieza con `<directorio vigilado>/`, y el resto es un nombre no vacío y sin otra `/`.
- **Liberación:** el último cierre de un archivo abierto (el kernel libera el `struct file`). Ocurre después de todas las escrituras, aunque el descriptor se haya duplicado (`dup2`) o el proceso haya terminado sin `close`.
- **Intención de escritura:** `flags` de apertura con modo `O_WRONLY` u `O_RDWR`, o con `O_CREAT` u `O_TRUNC`.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Sensor eBPF (`ProgramaIntermedio/ebpf/`)

1. EL sensor DEBERÁ escribirse en C (`sensor.bpf.c`), compilarse con Clang `-target bpf` y empaquetarse en un skeleton generado con `bpftool gen skeleton`.
2. EL sensor DEBERÁ interceptar las aperturas con los tracepoints `syscalls/sys_enter_openat`, `sys_enter_open` y `sys_enter_creat`.
3. EN cada apertura, EL sensor DEBERÁ:
   1. descartar el evento si el TGID es el del daemon, leído del mapa `pid_daemon`;
   2. leer el directorio vigilado del mapa `config_sensor` con `bpf_map_lookup_elem()`;
   3. descartar el evento si la ruta pasada a la syscall no está directamente dentro del directorio vigilado, si el mapa está vacío o si la ruta no cabe completa en el búfer.
4. CUANDO una apertura pase el filtro, EL sensor DEBERÁ enviar el evento al Ring Buffer `eventos` en la **liberación** de ese archivo. El evento lleva el PID (TGID) y el nombre (`comm`) del proceso que lo abrió, los `flags` de apertura y la ruta.
5. Las únicas decisiones que puede tomar el sensor son el filtro de ruta y el descarte del daemon (R1.3). Nada de hashes, severidades ni clasificaciones: la intención de escritura (creado o modificado) la decide el daemon.
6. Limitaciones conocidas que DEBERÁN documentarse en el README y luego en el Manual Técnico: no se resuelven rutas relativas ni `openat` con un `dirfd` propio; tampoco se detectan `rename`/`mv` hacia el directorio, `openat2`, `io_uring` ni escrituras por `mmap` sin reapertura.

## R2 — Carga, activación y ciclo de vida (daemon)

1. EL daemon DEBERÁ cargar el sensor con libbpf, escribir su propio PID (`getpid()`, **después** de `daemon(3)`) en `pid_daemon` y la ruta en `config_sensor`, adjuntar los programas y crear el consumidor del Ring Buffer.
2. AL arrancar, EL daemon DEBERÁ intentar activar el sensor con la `ruta_vigilada` de `daemon.conf`. SI falla (sin root, sin BTF, ruta inexistente), DEBERÁ registrar el motivo, seguir funcionando con el escaneo desactivado y exponer el motivo en `sensor_error` (R5.3).
3. `POST /api/monitoreo/archivos` (rol `admin_user`), con cuerpo `{"activo":bool}`, DEBERÁ:
   - responder `400` con `{"error":"Solicitud invalida"}` si el cuerpo es inválido;
   - **activar:** cargar y adjuntar el sensor (R2.1) y hacer el análisis inicial (R4.6);
   - **desactivar:** liberar el consumidor y **destruir** el sensor (se desadjunta y se descarga del kernel);
   - responder `200` con `{"archivos":<estado>,"cambio":<bool>}`;
   - SI la activación falla, responder `500` con `{"error":"No se pudo cargar el sensor eBPF"}`, sin alerta y con el escaneo desactivado.
4. CUANDO el estado cambie, EL daemon DEBERÁ emitir `Monitoreo del sistema activado` (`LOW`) con `Escaneo de archivos activado por <usuario>`, o `Monitoreo del sistema desactivado` (`MEDIUM`) con `Escaneo de archivos desactivado por <usuario>`, y enviar `event: monitoreo`. SI no cambia, no hay alerta.
5. MIENTRAS el escaneo esté desactivado, no DEBERÁ quedar ningún programa ni mapa del sensor en el kernel (`bpftool prog show` no los lista), y `/api/archivos` DEBERÁ devolver la última lista con `"activo":false`.
6. EL hilo `archivos` DEBERÁ bloquearse en `poll()` sobre el descriptor epoll del Ring Buffer y un `eventfd` de control, sin tiempo de espera. Sin actividad de archivos, no DEBERÁ consumir CPU.
7. AL apagarse el daemon (`SIGTERM`/`SIGINT`), DEBERÁ destruir el sensor y terminar con código 0.
8. Los mensajes de libbpf de nivel *warning* DEBERÁN ir al log del daemon, no a una terminal.

## R3 — Ruta vigilada

1. `POST /api/ruta` (rol `admin_user`) DEBERÁ recibir `{"ruta":"<ruta absoluta>"}`.
2. EL daemon DEBERÁ validar la ruta y responder `400` así:
   - cuerpo inválido, `ruta` vacía o no absoluta, o ruta canónica `/` → `{"error":"Ruta invalida"}`;
   - no existe o no es un directorio → `{"error":"La ruta no es un directorio existente"}`;
   - ruta canónica de más de 200 caracteres → `{"error":"Ruta demasiado larga"}`.
3. Con una ruta válida, EL daemon DEBERÁ canonizarla con `realpath` y actualizar `config_sensor` con `bpf_map_update_elem()` si el sensor está cargado; si no, la usará en la próxima activación. Además DEBERÁ vaciar el listado, hacer el análisis inicial si el escaneo está activo y responder `200` con `{"ruta":"<canónica>","cambio":true}`. SI falla la escritura del mapa, DEBERÁ responder `500` con `{"error":"No se pudo actualizar el sensor"}`.
4. SI la ruta canónica es igual a la actual, DEBERÁ responder `{"ruta":"<canónica>","cambio":false}` sin vaciar nada.
5. CUANDO la ruta cambie, EL daemon DEBERÁ enviar `event: monitoreo` y registrar en su log quién la cambió.
6. La ruta vale solo mientras el daemon está en ejecución: `daemon.conf` no se reescribe.

## R4 — Análisis de integridad (daemon)

1. Por cada evento del Ring Buffer, EL hilo `archivos` DEBERÁ ignorar los que no tienen intención de escritura y los que no están directamente dentro del directorio vigilado **actual** (eventos pendientes tras un cambio de ruta).
2. EL daemon DEBERÁ abrir el archivo con `O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC`, comprobar con `fstat` que sea un archivo regular y calcular el SHA-256 con la API EVP de OpenSSL. Si es un enlace simbólico, no es regular o ya no existe, el evento se ignora.
3. SI el archivo no está en el listado, EL daemon DEBERÁ agregarlo con la acción `creado` y emitir `Archivo modificado` (`LOW`, tipo `archivos`) con `El archivo <ruta> fue creado por el PID <pid> (<proceso>)`.
4. SI el archivo está en el listado y su SHA-256 o su `mtime` cambió, EL daemon DEBERÁ actualizarlo con la acción `modificado` y emitir `Archivo modificado` (`LOW`) con `El archivo <ruta> fue modificado por el PID <pid> (<proceso>)`. SI no cambió ninguno de los dos, no hay alerta ni actualización.
5. Cada entrada del listado DEBERÁ guardar la ruta, el nombre, el SHA-256 (64 hex en minúsculas), el tamaño, el `mtime`, el estado (`benigno` en F05), la última acción (`inicial`, `creado` o `modificado`), el PID y el proceso del último evento (`0` y `""` en el análisis inicial) y el momento del análisis.
6. **Análisis inicial:** CUANDO se active el escaneo o cambie la ruta, EL daemon DEBERÁ vaciar el listado y analizar los archivos regulares directamente dentro del directorio (sin subdirectorios ni enlaces) con la acción `inicial`, **sin generar alertas**.
7. El listado DEBERÁ tener como máximo **256** entradas. Si se llena y llega un archivo nuevo, se descarta la entrada con el análisis más antiguo.
8. El cálculo del SHA-256 DEBERÁ hacerse fuera de todo mutex. El listado se protege con su propio mutex.
9. Las propias lecturas del daemon no DEBERÁN generar eventos (R1.3.1): un cambio produce **una** alerta, no un bucle.

## R5 — API y tiempo real

1. `GET /api/archivos` (rol `common_user`) DEBERÁ responder `200` con:

   ```json
   {"activo":true,"ruta":"/home/u/so2_vigilado",
    "archivos":[{"ruta":"/home/u/so2_vigilado/a.txt","nombre":"a.txt",
                 "sha256":"…","tamano":12,"mtime":"2026-10-18 14:00:00",
                 "estado":"benigno","accion":"modificado","pid":1234,"proceso":"bash",
                 "analizado":"2026-10-18 14:00:01"}, …]}
   ```

   con `archivos` ordenado del análisis más reciente al más antiguo.
2. CUANDO se agregue o actualice una entrada, EL daemon DEBERÁ enviar a los clientes SSE `event: archivo` con esa entrada. CUANDO el listado se vacíe (activación o cambio de ruta), DEBERÁ enviar `event: archivos` con el cuerpo completo de R5.1.
3. `GET /api/monitoreo` y `event: monitoreo` DEBERÁN incluir `"archivos":bool`, `"ruta_vigilada":"<canónica>"` y `"sensor_error":"<texto>"` (vacío si no hay error), además de `metricas` y `procesos`.
4. Los contratos de `/api/estado`, `/api/procesos`, `/api/metricas` y `/api/alertas` no cambian.

## R6 — Dashboard

1. Un panel **Archivos analizados** DEBERÁ mostrar el listado con las columnas Archivo, Estado (`benigno` en verde; `infectado` en rojo, reservado para F06), Última modificación (`mtime`), SHA-256 (12 primeros caracteres; el hash completo en `title`) y Último evento (acción, PID y proceso). La lista se carga con `GET /api/archivos` al abrir o reabrir el SSE y se actualiza con `event: archivo` y `event: archivos`.
2. Todos los usuarios DEBERÁN ver la ruta vigilada y el estado `Archivos: activo/desactivado`. Si `sensor_error` no está vacío, se muestra.
3. EL administrador DEBERÁ ver un interruptor **Escaneo de archivos** (refleja `event: monitoreo`) y un formulario **Ruta vigilada** con un campo y el botón **Cambiar ruta**. El resultado o el `error` se muestran en un aviso junto al panel.
4. EL usuario común NO DEBERÁ ver el interruptor ni el formulario.
5. MIENTRAS el escaneo esté desactivado, el panel DEBERÁ mostrar `Escaneo de archivos desactivado`, conservar los últimos datos y dejar visible el interruptor.
6. Solo `textContent`; nada de `innerHTML`.

## R7 — Pruebas y paridad

1. `make -C ProgramaIntermedio pruebas` DEBERÁ incluir `pruebas/test_analisis.c`: el filtro de ruta compartido con el eBPF, el SHA-256, la validación de rutas, el procesamiento de eventos, el análisis inicial, el límite de 256 entradas y el JSON. Todo sin root y con código 0.
2. EL proyecto DEBERÁ incluir `scripts/pruebas_sensor.sh` (`sudo`, VM), que verifique R1–R5 contra el daemon real y el kernel, y DEBERÁ sumarse a `scripts/correr_pruebas.sh`.
3. Las suites `pruebas_integracion.sh`, `pruebas_auth.sh`, `pruebas_alertas.sh`, `pruebas_metricas.sh` y `pruebas_procesos.sh` DEBERÁN seguir terminando con `0 FALLIDAS`.
4. `make -C ProgramaIntermedio clean all` DEBERÁ terminar sin warnings, con `daemon_monitor` versionado. `vmlinux.h`, `*.skel.h` y `*.bpf.o` no se versionan.
