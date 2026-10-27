# F01 — Autenticación y roles (PAM) · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo` o la VM queda `- [ ]` con `Pendiente en VM:` y los comandos exactos.

Antes de T1: `sudo scripts/setup_usuarios.sh` no existe aún. Si `/usr/include/security/pam_appl.h` falta y no tienes `sudo`, haz T1 y T8 primero y pide al usuario que ejecute `sudo apt install libpam0g-dev`.

---

- [x] **T1 — cJSON vendorizado y Makefile** · R3.1, R8.6 · diseño §6, §7
  - Descargar `cJSON.c` y `cJSON.h` v1.7.18 a `ProgramaIntermedio/lib/` sin modificar.
  - Makefile: `-Ilib`, `LDLIBS = -lpam`, `lib/cJSON.c` en `FUENTES`, `$(LDLIBS)` en el enlace, `clean` ampliado.
  - Verificación: `sha256sum ProgramaIntermedio/lib/cJSON.*` (anotar ambos hashes); `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`.
  Evidencia: sha256 cJSON.c (75c51de8fa40ac9d7a99319c6330719bd692eb81c0a869265f3d4c682533f9b9) y cJSON.h (0578cc29132912edbc88f83207a8fc76e5db3db0605497e909a9384ef3cc474b); make clean all compilo con 0 warnings.

- [x] **T2 — `peticion.c` y sus pruebas** · R5.1–R5.4 · diseño §4
  - Implementar `peticion_encabezado`, `peticion_cookie`, `peticion_content_length`.
  - `pruebas/test_peticion.c`, como mínimo: encabezado presente / ausente / en otra capitalización (`cookie:`, `CONTENT-LENGTH:`); el nombre no se busca en el cuerpo; cookie sola, entre otras (`a=1; so2_sesion=…; b=2`), ausente; `Content-Length` válido, ausente, negativo, no numérico, con basura (`12abc`).
  - Verificación: `make -C ProgramaIntermedio pruebas; echo $?` → `0`.
  Evidencia: `make -C ProgramaIntermedio pruebas; echo $?` devolvio 0 y paso exitosamente todos los casos de peticion.c y test_config.

- [x] **T3 — `auth.c`: roles, sesiones y permisos con pruebas** · R3.5, R3.11, R3.12, R4.1, R4.7 · diseño §3.1, §3.3, §3.4
  - Implementar `rol_nombre`, `rol_de_grupos`, `rol_suficiente`, `sesion_*`, `permiso_buscar`, `auth_rol_de_usuario`.
  - `pruebas/test_auth.c`, como mínimo:
    1. `sesion_crear` devuelve 64 caracteres hex y `sesion_buscar` recupera usuario y rol.
    2. Dos sesiones creadas seguidas tienen tokens distintos.
    3. Token desconocido → `-1`.
    4. Creada en `t`: buscar en `t+1800` → `0`; luego buscar en `t+1800+1801` → `-1` (la búsqueda anterior renovó).
    5. `sesion_vigente` no renueva: creada en `t`, `sesion_vigente` en `t+1000`, `sesion_buscar` en `t+1801` → `-1`.
    6. `sesion_eliminar` → `-1` al buscar.
    7. 65 sesiones con `ultimo_uso` creciente → la primera ya no existe y la 65 sí.
    8. `rol_de_grupos`: (1,1) → admin, (1,0) → admin, (0,1) → común, (0,0) → ninguno.
    9. `rol_suficiente` en sus 9 combinaciones.
    10. `permiso_buscar`: las 6 filas de R4.1 con su rol; `GET /api/login` → `-2`; `GET /api/noexiste` → `-1`.
    11. `auth_rol_de_usuario("usuario_que_no_existe_so2")` → ninguno.
  - Verificación: `make -C ProgramaIntermedio pruebas; echo $?` → `0`.
  Evidencia: `make -C ProgramaIntermedio pruebas; echo $?` devolvio 0 y paso exitosamente los 11 casos de test_auth.

- [x] **T4 — PAM y `auth_iniciar`** · R2.1–R2.3, R2.5, R2.6 · diseño §3.2, §3.5
  - `auth_verificar_pam` con `conversar`, `sin_espera`, `PAM_DISALLOW_NULL_AUTHTOK` y `pam_acct_mgmt`.
  - `auth_iniciar()` llamada desde `main.c` antes de `servidor_iniciar`.
  - Verificación en host: compila sin warnings; `./daemon_monitor -c <conf de prueba>` sin root muestra la advertencia de R2.5 y sigue respondiendo.
  Evidencia: compilo con 0 warnings; ./daemon_monitor -c <conf> mostro advertencias de PAM y grupos sin root, respondio HTTP 200 y finalizo con codigo 0.

- [x] **T5 — HTTP: POST, despacho, cookies y SSE** · R3.1–R3.4, R3.6, R3.8–R3.11, R4.2–R4.6, R5 · diseño §5
  - `struct cliente` ampliado, lectura de cuerpo, códigos nuevos, sin `Access-Control-Allow-Origin`.
  - Despacho de §5.3 y handlers `login`, `logout`, `sesion`; token en clientes SSE; barrido de §5.5; cierre de SSE en logout.
  - Verificación en host (sin root): `curl -s -o /dev/null -w '%{http_code}' localhost:<p>/api/estado` → `401`; `POST /api/login` con JSON inválido → `400`; `PUT /api/estado` → `405`; `GET /api/noexiste` → `404`; `POST` sin `Content-Length` → `411`; `curl -sI localhost:<p>/ | grep -ci access-control` → `0`; el daemon sigue vivo.
  Evidencia: en puerto 8096: /api/estado sin sesion -> 401, POST login invalido -> 400, PUT /api/estado -> 405, GET /api/noexiste -> 404, POST sin Content-Length -> 411, Access-Control count -> 0, daemon vivo y apagado limpio con SIGTERM.

- [x] **T6 — `obtener_home` solo como root** · R7.1 · diseño §8
  - Cambio en `config.c` + caso nuevo en `test_config.c`.
  - Verificación: `make -C ProgramaIntermedio pruebas` → todos los casos en verde, incluido el nuevo.
  Evidencia: `make -C ProgramaIntermedio pruebas; echo $?` devolvio 0 y paso exitosamente los 9 casos de test_config (incluyendo SUDO_USER con geteuid != 0).

- [x] **T7 — Dashboard** · R6 · diseño §9
  - `login.html`, `js/api.js`, `js/auth.js`; cambios en `index.html`, `js/app.js` y `css/styles.css`.
  - Verificación en host: `grep -rn "?api=\|const API" Dashboard/js` → sin resultados; `grep -c 'data-rol="admin_user"' Dashboard/index.html` → ≥ 1; `curl -s localhost:<p>/login.html | grep -ci '<form'` → `1`.
  Evidencia: `grep -rn "?api=\|const API" Dashboard/js` dio 0 coincidencias; `grep -c 'data-rol="admin_user"' Dashboard/index.html` dio 1; `curl -s http://127.0.0.1:8097/login.html | grep -ci '<form'` dio 1.

- [x] **T8 — `setup_usuarios.sh`** · R1 · diseño §10.1
  - Verificación en host: `bash -n scripts/setup_usuarios.sh`; ejecutarlo sin root → código 1 y `Ejecuta con sudo`.
  Evidencia: `bash -n scripts/setup_usuarios.sh` paso exitosamente; `./scripts/setup_usuarios.sh` sin root mostro 'Ejecuta con sudo' y termino con codigo 1.

- [x] **T9 — Pruebas de autenticación** · R8.2, R8.5 · diseño §10.2, §10.3
  - `scripts/usuarios_prueba.sh` y `scripts/pruebas_auth.sh` con las 18 verificaciones de la tabla §10.3.
  - Verificación en host: `bash -n` de ambos; ejecutar `pruebas_auth.sh` sin root → código 1 y `Ejecuta con sudo`.
  Evidencia: `bash -n scripts/usuarios_prueba.sh scripts/pruebas_auth.sh` paso exitosamente; `./scripts/pruebas_auth.sh` sin root mostro 'Ejecuta con sudo' y termino con codigo 1.

- [x] **T10 — Adaptar `pruebas_integracion.sh` y `demo.sh`** · R8.3, R8.4 · diseño §10.4
  - Verificación en host: `bash -n scripts/pruebas_integracion.sh scripts/demo.sh`; `grep -c "comprobar\|aprobar" scripts/pruebas_integracion.sh` no menor que antes del cambio (anotar ambos números).
  Evidencia: `bash -n scripts/pruebas_integracion.sh scripts/demo.sh` paso con codigo 0; `grep -c "comprobar\|aprobar"` dio 41 antes y 41 despues del cambio.

- [x] **T11 — README y binario** · R8.6, R8.7
  - Sección "Usuarios y sesión" en `README.md`: `setup_usuarios.sh`, alta de usuarios, `sudo` para el daemon, login en el navegador, `sudo scripts/pruebas_auth.sh`.
  - Recompilar y versionar `ProgramaIntermedio/daemon_monitor`.
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`; `git status --short ProgramaIntermedio/daemon_monitor` vacío después del commit.
  Evidencia: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` dio 0 warnings; `git status --short ProgramaIntermedio/daemon_monitor` quedo vacio.

- [ ] **T12 — Validación en VM** · R1–R8
  - Pendiente en VM (requiere `sudo`):
    1. `sudo scripts/setup_usuarios.sh` dos veces seguidas → ambas con código 0.
    2. `make -C ProgramaIntermedio clean all pruebas` → código 0.
    3. `sudo scripts/pruebas_auth.sh` → `0 FALLIDAS`.
    4. `sudo scripts/pruebas_integracion.sh` → `0 FALLIDAS` y al menos 38 aprobadas.
    5. Navegador: login como admin ve `#panel-pid`; como común no lo ve; **Cerrar sesión** vuelve a `/login.html`.
    6. `getent passwd | grep -c '^so2t_'` → `0` después de las pruebas.
