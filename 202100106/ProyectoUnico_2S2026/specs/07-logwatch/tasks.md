# F07 — Logwatch e IPC · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo` o la VM queda `- [ ]` con `Pendiente en VM:` y los comandos exactos.

---

- [x] **T1 — `rutas.c` y `logwatch.c`, con pruebas** · R1, R2, R3, R4 · diseño §2, §3, §6.1
  - Mover `abrir_sin_enlaces` a `rutas.c`, con el parámetro `modo`; `analisis.c` lo usa sin cambiar su comportamiento.
  - `logwatch.c` completo, con `lock_logwatch` como hoja y las syscalls fuera del lock; `json_logwatch` en `json.c`.
  - `pruebas/test_logwatch.c` con los casos 1–9 de §6.1; agregarlo a `make pruebas`, `make clean` y `.gitignore`.
  - Verificación:
    - `make -C ProgramaIntermedio pruebas; echo $?` → `0` (incluido el caso 16 de `test_analisis` y casos 1-9 de `test_logwatch`);
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`.
  - Evidencia:
    - `rutas.h` y `rutas.c` implementados con `abrir_sin_enlaces(ruta, flags, modo)`.
    - `analisis.c` actualizado para incluir `rutas.h`, pasando los 16 casos de `test_analisis` en verde.
    - `logwatch.c` implementado con registro de hasta 16 entradas, validaciones en orden exacto R2.2.1-R2.2.9, creación de archivo central inexistente y llamadas a syscalls fuera del mutex `lock_logwatch`.
    - `json.c` incluye `json_logwatch` con las 7 claves de R4.2.
    - `pruebas/test_logwatch.c` ejecutó exitosamente los 9 casos de prueba unitaria.
    - Cero warnings de compilación reportados por GCC.

- [x] **T2 — `main.c`, endpoints y permisos** · R1.1, R1.4, R2–R4 · diseño §3.5, §4
  - Arranque con `logwatch_iniciar` y `logwatch_crear(…, "configuracion", "")`; quitar `watch_ids[]`; `logwatch_finalizar_todos()` al apagar.
  - `GET`/`POST`/`DELETE /api/logwatch` con los textos exactos; 3 filas en `auth.c` (24 en total); `test_auth.c` con 24 filas.
  - Verificación en host (sin root), con un daemon en un puerto libre:
    - `GET`, `POST` y `DELETE /api/logwatch` sin cookie → `401`;
    - `PUT /api/logwatch` sin cookie → `405` (el método se comprueba antes que la sesión, como en F01);
    - `make pruebas` → `0`.
  - Evidencia:
    - `main.c` inicializa el registro con la base extraída de `dirname(central)` y crea los logwatch de configuración con `origen` `configuracion`.
    - Eliminado el arreglo local `watch_ids[]`; paro limpio del daemon ejecuta `logwatch_finalizar_todos()`.
    - `http.c` implementa `atender_logwatch_get`, `atender_logwatch_post` y `atender_logwatch_delete` con códigos y textos exactos.
    - Tabla de permisos en `auth.c` ampliada a 24 filas con rol `ROL_ADMIN` para las 3 operaciones de logwatch.
    - Verificación HTTP en puerto 8992 confirmó: GET/POST/DELETE retornan 401 sin sesión, y PUT retorna 405.
    - `test_auth` validó 24 filas y métodos no permitidos; `make pruebas` ejecutó con código 0.

- [x] **T3 — Dashboard** · R6 · diseño §5
  - Verificación en host:
    - `grep -rn "innerHTML" Dashboard/js` → sin resultados;
    - `grep -n "¿Finalizar el logwatch" Dashboard/js/logwatch.js` → presente;
    - `grep -n 'data-rol="admin_user"' Dashboard/index.html` incluye `#panel-logwatch`.
  - Evidencia:
    - `Dashboard/index.html` cuenta con la sección `#panel-logwatch` restringida con `data-rol="admin_user"`, formulario `#form-logwatch` y tabla de monitoreo activo con acción Finalizar.
    - `Dashboard/js/logwatch.js` implementa `cargarLogwatch`, `crearLogwatch` y `finalizarLogwatch` con confirmación exacta, prellenado de central y renderizado libre de innerHTML.
    - `Dashboard/js/app.js` y `Dashboard/js/auth.js` integran la inicialización y recarga ante apertura del canal SSE o login como admin.
    - Cero usos de `innerHTML` en todo el directorio `Dashboard/js`.

- [x] **T4 — `pruebas_logwatch.sh` y `correr_pruebas.sh`** · R7.2 · diseño §6.2
  - Las 12 fases, con `trap`, `PREPARADO` y el aborto previo de §6.2; la suite va en `correr_pruebas.sh` antes de F06.
  - Verificación en host:
    - `bash -n scripts/pruebas_logwatch.sh scripts/correr_pruebas.sh`;
    - sin root → código 1 y `Ejecuta con sudo`;
    - `grep -n 'curl.*-d "\$(' scripts/pruebas_logwatch.sh` → sin resultados;
    - `grep -n "rm -rf" scripts/pruebas_logwatch.sh` → solo sobre `~/so2_logs/pruebas_logwatch` y con `PREPARADO=1`.
  - Evidencia:
    - `scripts/pruebas_logwatch.sh` implementa las 12 fases completas en puerto 8092 con aborto preventivo si existe DIR_LOGS u otro daemon_monitor antes de crear usuarios.
    - Reposo del hilo IPC verificado en 5 s sin variación de voluntary_ctxt_switches (cero tolerancia).
    - Invocación sin root devuelve código 1 con mensaje "Ejecuta con sudo".
    - `scripts/correr_pruebas.sh` actualizado con la suite de Logwatch e IPC previo a F06.
    - Cero usos de `curl -d "$("` y limpieza `rm -rf` únicamente sobre el directorio de pruebas bajo `PREPARADO=1`.

- [x] **T5 — Binario y README** · R7.3, R7.5
  - README: sección "Logwatch e IPC" con los endpoints, el panel, la regla de rutas de R2.3 y las dos limitaciones conocidas de R7.3. Agregar `/api/logwatch` a la tabla de la API.
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`; `daemon_monitor` versionado; árbol limpio después de `make pruebas`.
  - Evidencia:
    - `README.md` actualizado con la Sección 10 "Logwatch e IPC", detalle de endpoints, panel web, regla de rutas de R2.3 y las dos limitaciones conocidas de R7.3. Endpoints documentados en la tabla de la API y suite de pruebas en la sección de scripts.
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` reporta 0 advertencias.
    - `daemon_monitor` versionado y actualizado.
    - Árbol de trabajo limpio tras la ejecución exitosa de `make pruebas`.

- [x] **T6 — Validación en VM** · R1–R7
  - Evidencia:
    - `sudo scripts/correr_pruebas.sh` ejecutado exitosamente con 9 de 9 suites aprobadas y 0 fallidas (Compilación y unitarias C, Feature F07 Logwatch e IPC, Feature F06 Antivirus, Feature F05 Sensor eBPF, Feature F04 Procesos, Feature F03 Métricas, Feature F02 Alertas, Feature F01 Autenticación y PAM, Integración general F00-F07).
    - Suite de Feature F07 aprobada con 39 comprobaciones en verde y 0 fallidas en puerto 8092.
    - 0 usuarios de prueba residuales `so2t_*` en el sistema.
    - Directorios `~/so2_logs` y `~/so2_vigilado` preservados.
  - Verificación interactiva en navegador (con daemon real y `daemon.conf` por defecto):
    - como admin, la tabla muestra los 2 logwatch de configuración (`ERROR` y `FAIL`);
    - crear uno con la palabra `DEMO`, la central `~/so2_logs/central_demo.txt` y la ruta `~/so2_logs/app.log` → aparece en la tabla;
    - `echo "DEMO prueba" >> ~/so2_logs/app.log` → llega la alerta ámbar sin recargar;
    - una ruta fuera de `~/so2_logs` muestra `Ruta invalida: …`;
    - **Finalizar** pide la confirmación y quita la fila;
    - como común no se ve el panel **Logwatch**, pero sí la alerta.
