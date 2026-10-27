# F06 — Antivirus, blacklist y cuarentena · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo` o la VM queda `- [ ]` con `Pendiente en VM:` y los comandos exactos.

---

- [x] **T1 — `blacklist.c` y `blacklist.json`, con pruebas** · R1, R2.1, R2.2 · diseño §2, §6.1
  - Validación con los textos exactos de R1.3, tabla hash, `blacklist_iniciar`, `blacklist_actualizar` con `guardar_atomico`, `blacklist_buscar`, `blacklist_version`, `blacklist_info`, `blacklist_lista`.
  - `ProgramaIntermedio/blacklist.json` con las 4 firmas de R1.4.
  - `pruebas/test_blacklist.c` con los casos 1–7 de §6.1; agregarlo a `make pruebas`.
  - Verificación:
    - `make -C ProgramaIntermedio pruebas; echo $?` → `0`;
    - `python3 -m json.tool ProgramaIntermedio/blacklist.json > /dev/null` → código 0;
    - para cada muestra `n` de 1 a 3: `printf 'SO2-MUESTRA-MALICIOSA-%d\n' n | sha256sum` coincide con su firma.
  - Evidencia:
    - `make -C ProgramaIntermedio pruebas` ejecutó con éxito las 7 suites incluyendo `test_blacklist: OK` (código de salida 0).
    - `python3 -m json.tool ProgramaIntermedio/blacklist.json > /dev/null` exitoso (código 0).
    - Hashes de muestras maliciosas 1..3 coinciden con las firmas `SO2.Demo.Troyano`, `SO2.Demo.Ransomware` y `SO2.Demo.Keylogger`.
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` retornó 0.

- [x] **T2 — Detección, cuarentena, amenazas y re-evaluación** · R3, R4.2–R4.4 · diseño §3, §6.2
  - `analizar_abierto` común a los tres contextos; `analisis_reevaluar`; `analisis_cuarentena_manual`; listas de amenazas y cuarentena; `ver_evaluada` en `hilo_archivos`.
  - Casos 9–15 de §6.2 en `test_analisis.c`.
  - Verificación: `make -C ProgramaIntermedio pruebas` → `0`; `make clean all 2>&1 | grep -ci warning` → `0`.
  - Evidencia:
    - `make -C ProgramaIntermedio pruebas` pasó todas las pruebas incluyendo los 15 casos de `test_analisis: OK` (código de salida 0).
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` retornó 0 warnings.
    - `git status --short ProgramaIntermedio/blacklist.json` sin cambios tras ejecutar las pruebas.

- [x] **T3 — Búfer grande y endpoints** · R2.3–R2.6, R4.1, R4.5, R5 · diseño §4
  - `peticion` como puntero con `capacidad`; búfer de 1 MiB solo para admin en `POST /api/blacklist`; 5 filas de permiso (21 en total); `json_archivo` con 12 claves; `json_amenaza`, `json_registro_cuarentena`, `json_blacklist`; `difundir_amenazas`; `blacklist_iniciar` y `blacklist_finalizar` en `main.c`.
  - `test_auth.c` con 21 filas.
  - Verificación en host (sin root), con un daemon en un puerto libre:
    - `GET /api/amenazas`, `GET /api/cuarentena`, `GET /api/blacklist` sin cookie → `401`;
    - `POST /api/blacklist` sin cookie con `Content-Length: 500000` → `401` (sin enviar el cuerpo);
    - `POST /api/ruta` con 8 KiB → `413`;
    - `make pruebas` → `0`.
  - Evidencia:
    - Daemon levantado en puerto 8097:
      - `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8097/api/amenazas` → `401`.
      - `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8097/api/cuarentena` → `401`.
      - `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8097/api/blacklist` → `401`.
      - `curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Length: 500000" http://127.0.0.1:8097/api/blacklist` → `401`.
      - `head -c 8192 /dev/zero | tr '\0' 'A' | curl -s -o /dev/null -w "%{http_code}" -X POST --data-binary @- http://127.0.0.1:8097/api/ruta` → `413`.
    - `make -C ProgramaIntermedio pruebas` pasó todas las pruebas unitarias (incluyendo los 11 casos de `test_auth` con 21 filas de permisos y casos negativos).
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` retornó 0 warnings.

- [x] **T4 — Dashboard** · R6 · diseño §5
  - Verificación en host:
    - `grep -rn "innerHTML" Dashboard/js` → sin resultados;
    - `grep -c "addEventListener('amenaza'" Dashboard/js/app.js` → `1`;
    - `grep -n "El archivo quedará con permisos 0000" Dashboard/js/archivos.js` → presente;
    - `grep -n 'data-rol="admin_user"' Dashboard/index.html` incluye `#panel-cuarentena` y `#base-firmas`.
  - Evidencia:
    - `grep -rn "innerHTML" Dashboard/js` → 0 coincidencias (solo se utiliza `textContent` y métodos de DOM).
    - `grep -c "addEventListener('amenaza'" Dashboard/js/app.js` → 1.
    - `grep -n "El archivo quedará con permisos 0000" Dashboard/js/archivos.js` → presente en la línea 77.
    - `grep -n 'data-rol="admin_user"' Dashboard/index.html` → incluye `#panel-cuarentena` (L208) y `#base-firmas` (L229).

- [x] **T5 — `pruebas_antivirus.sh`, `demo_antivirus.sh` y `correr_pruebas.sh`** · R7.2, R7.3 · diseño §6.3, §6.4
  - Las 13 verificaciones de §6.3, con `trap` y el manejo seguro de directorios de F05; la suite va en `correr_pruebas.sh` antes de F05.
  - Verificación en host:
    - `bash -n` de los 3 scripts;
    - `pruebas_antivirus.sh` sin root → código 1 y `Ejecuta con sudo`;
    - `grep -n 'curl.*-d "\$(' scripts/pruebas_antivirus.sh` → sin resultados;
    - `grep -n "rm -rf" scripts/pruebas_antivirus.sh` solo sobre los directorios que crea el script;
    - `scripts/demo_antivirus.sh <dir temporal fuera del repo>` crea los 4 archivos con los hashes de R1.4 (borrar el directorio después).
  - Evidencia:
    - `bash -n` verificado con éxito en `scripts/pruebas_antivirus.sh`, `scripts/demo_antivirus.sh` y `scripts/correr_pruebas.sh`.
    - `scripts/pruebas_antivirus.sh` sin root respondió `Ejecuta con sudo` y salió con código 1.
    - `grep -n 'curl.*-d "\$(' scripts/pruebas_antivirus.sh` retornó 0 coincidencias (logins e invocaciones pasan contraseñas por stdin con `--data-binary @-`).
    - `grep -n "rm -rf" scripts/pruebas_antivirus.sh` acotado exclusivamente a `$DIR_VIGILADO` y `$DIR_LOGS` creados por la prueba.
    - `scripts/demo_antivirus.sh` ejecutado en directorio temporal (`/tmp/tmp.6RDKlGGBde`) generó `troyano.txt`, `ransomware.txt`, `keylogger.txt` y `notas.txt` con los hashes SHA-256 exactos de R1.4.
    - `scripts/correr_pruebas.sh` actualizado incluyendo "Feature F06 - Antivirus y cuarentena" antes de F05.

- [x] **T6 — Binario y README** · R7.5
  - README: sección "Antivirus y cuarentena" con el formato de `blacklist.json` (ejemplo y reglas), la tabla de firmas de demostración, los endpoints, `event: amenaza`, la cuarentena manual y sus límites, `scripts/demo_antivirus.sh` y `sudo scripts/pruebas_antivirus.sh`.
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`; `daemon_monitor` versionado; `git status --short ProgramaIntermedio/blacklist.json` sin cambios después de correr `make pruebas`.
  - Evidencia:
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` retornó `0` warnings.
    - Binario `daemon_monitor` compilado y versionado correctamente.
    - `make -C ProgramaIntermedio pruebas` ejecutó con éxito todas las pruebas unitarias (código 0).
    - `git status --short ProgramaIntermedio/blacklist.json` sin cambios tras las pruebas unitarias.
    - `README.md` actualizado con la sección 9 completa ("Antivirus, blacklist y cuarentena"), tabla de firmas demo, reglas de validación R1, endpoints REST/SSE actualizados y referencias a scripts y especificaciones.

- [x] **T7 — Validación en VM** · R1–R7
  - Correcciones de revisión implementadas:
    - B1: Helper `abrir_sin_enlaces` con `openat2` y `RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS` en las 4 aperturas de `analisis.c`; respaldo solo ante `EACCES` con `O_PATH` y `fstat`; caso 16 agregado a `test_analisis.c`.
    - B2: `pruebas_antivirus.sh` valida que ni `DIR_VIGILADO` ni `DIR_LOGS` existan antes de crear usuarios, y elimina el `rm -rf` preventivo de preparación.
    - A1: `analizar_abierto` verifica `generacion == gen` bajo mutex antes de `fchmod`. Si la generación cambió tras `fchmod`, registra la cuarentena y su alerta.
    - A2: `atender_blacklist_post` escapa el nombre de la blacklist con `buf` y `json_cadena`.
    - A3: `cliente_leer` valida que el método sea `POST` además de la ruta `/api/blacklist` antes de pre-reservar 1 MiB.
    - A4: `demo_antivirus.sh` tolera `sha256sum` en archivos con permisos `0000` (`(en cuarentena)`).
    - A5: `blacklist_iniciar` llama a `registrar` fuera de `lock_blacklist` y reporta `"No es un archivo regular"` cuando `stat` da bien pero `!S_ISREG`.
    - A6: `analisis_cuarentena_manual` toma el SHA de la re-búsqueda en la segunda sección crítica en vez de reutilizar `idx` obsoleto.
    - A7: `.gitignore` incluye `**/pruebas/test_blacklist`.
  - Verificación local: `make clean all` con 0 warnings; `make pruebas` con 16 casos de `test_analisis` y `test_blacklist: OK` (código 0).
  - Verificación completa en VM (`sudo scripts/correr_pruebas.sh`):
    - 8 de 8 suites aprobadas, 0 fallidas.
    - Feature F06 aprobada con 33 de 33 comprobaciones en 17s.
    - 0 usuarios residuales `so2t_*` en el sistema.
    - `ProgramaIntermedio/blacklist.json` del repositorio intacto.
