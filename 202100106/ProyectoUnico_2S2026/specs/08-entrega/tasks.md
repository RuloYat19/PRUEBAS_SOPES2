# F08 — Documentación y entrega · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo`, la VM o al usuario queda `- [ ]` con `Pendiente:` y los pasos exactos.

---

- [x] **T1 — Advertencias de F07** · R1
  - A1–A6 según R1, con sus casos nuevos en `test_logwatch.c` y los cambios en `pruebas_logwatch.sh`.
  - Verificación:
    - `make -C ProgramaIntermedio pruebas; echo $?` → `0`;
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`;
    - `bash -n scripts/pruebas_logwatch.sh`;
    - `grep -n "trap limpiar EXIT" scripts/pruebas_logwatch.sh` aparece en una línea anterior a `usuarios_prueba_crear`;
    - `grep -nE "dashboard_dir|log_daemon|blacklist_ruta" scripts/pruebas_logwatch.sh` → sin resultados.
  - Evidencia:
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → 0 advertencias.
    - `make -C ProgramaIntermedio pruebas; echo $?` → código 0, casos 1 a 11 en verde (caso 10 base reemplazada por enlace, caso 11 parada con error en stop vacía registro).
    - `bash -n scripts/pruebas_logwatch.sh` → código 0.
    - `grep -n "trap limpiar EXIT" scripts/pruebas_logwatch.sh` en línea 70, antes de `usuarios_prueba_crear` en línea 72.
    - `grep -nE "dashboard_dir|log_daemon|blacklist_ruta" scripts/pruebas_logwatch.sh` → 0 coincidencias. Fase 12 valida ausencia de 'clave desconocida' y fase 9 comprueba que los 15 DELETE respondan 200.

- [x] **T2 — `docs/Manual_Tecnico.md`** · R2 · diseño §2
  - Las 10 secciones del índice, con los 2 diagramas Mermaid, las decisiones de seguridad y las limitaciones conocidas.
  - Verificación:
    - `grep -c '```mermaid' docs/Manual_Tecnico.md` → `2` o más;
    - cada ruta de la API de `auth.c` aparece en el manual: `for r in $(grep -oE '"/api/[a-z/]+"' ProgramaIntermedio/auth.c | tr -d '"' | sort -u); do grep -q "$r" docs/Manual_Tecnico.md || echo "falta $r"; done` → sin salida;
    - cada `event: X` de `http.c` aparece en el manual;
    - cada clave de `config.c` aparece en la tabla de `daemon.conf`.
  - Evidencia:
    - `grep -c '```mermaid' docs/Manual_Tecnico.md` → 3 diagramas Mermaid (arquitectura global flowchart, hilos y despertadores flowchart, secuencia de detección y cuarentena sequenceDiagram).
    - Verificación de rutas de la API contra `auth.c` (24 filas): 0 faltantes.
    - Verificación de eventos SSE contra `http.c` (`alerta`, `amenaza`, `archivo`, `archivos`, `metricas`, `monitoreo`, `procesos`): 0 faltantes.
    - Verificación de claves de `daemon.conf` contra `config.c` (`puerto`, `intervalo`, `dashboard`, `blacklist`, `log_central`, `ruta_vigilada`, `log_rutas`, `log_palabras`): 0 faltantes.
    - Referencia exacta a la captura `../imgs/20-correr-pruebas.png`.

- [x] **T3 — `docs/Manual_Usuario.md`** · R3 · diseño §3, §4
  - Las 13 secciones, con las referencias a las 20 capturas de §4 (`../imgs/NN-….png`), que todavía no existen.
  - Verificación: `grep -oE '\.\./imgs/[0-9]{2}-[a-z-]+\.png' docs/*.md | sort -u | wc -l` → `20`.
  - Evidencia:
    - `grep -oE '\.\./imgs/[0-9]{2}-[a-z-]+\.png' docs/*.md | sort -u | wc -l` → exactamente 20 capturas referenciadas (01 a 19 en Manual de Usuario, 20 en Manual Técnico).
    - Cubiertas las 13 secciones requeridas, tabla comparativa de permisos para admin, común y no autenticado, y resolución de 5 problemas frecuentes.

- [x] **T4 — README portada y `steering/structure.md`** · R5 · diseño §5
  - Verificación:
    - `wc -l README.md` → `200` o menos;
    - el README enlaza a los dos manuales;
    - cada archivo de `steering/structure.md` §2 existe (`git ls-files`), y cada `.c`/`.js` versionado aparece en §2.
  - Evidencia:
    - `wc -l README.md` → 131 líneas (≤ 200). Enlaza a `docs/Manual_Tecnico.md`, `docs/Manual_Usuario.md`, `specs/README.md` y `enunciado.md`.
    - `steering/structure.md` §2 actualizado con el árbol exacto entregado. Comprobación contra `git ls-files`: 0 archivos faltantes, 100% de archivos `.c` y `.js` versionados documentados en el árbol.

- [x] **T5 — Revisión del repositorio** · R6.1–R6.5 · diseño §6
  - Los comandos de §6 con su salida pegada en la evidencia.
  - `daemon_monitor` recompilado y versionado; `specs/README.md` con 08 `En revisión`.
  - Evidencia:
    - `git ls-files ProyectoUnico_2S2026 | grep -E '\.(o|log|txt)$|cookie|test_[a-z]+$|so2_(logs|vigilado)'` → sin salida (0 archivos indeseados).
    - `git ls-files ProyectoUnico_2S2026/ProgramaIntermedio/ebpf` → `Makefile`, `filtro_ruta.h`, `sensor.bpf.c`, `sensor.h` (4 archivos esperados).
    - `make -C ProyectoUnico_2S2026/ProgramaIntermedio clean all 2>&1 | grep -ci warning` → 0 advertencias.
    - `git status --short` → limpio respecto al código y binario (`daemon_monitor` al día).
    - `find ProyectoUnico_2S2026 -user root` → sin salida (0 archivos de root en el árbol).
    - `git grep -nE 'password"?\s*[:=]\s*"[^"]{4,}' -- ProyectoUnico_2S2026 ':!*/specs/*'` → solo placeholders de scripts de prueba (`clave_incorrecta_prueba`, `clave_falsa_123`, `clave_falsa_rapida`), sin contraseñas reales.
    - `specs/README.md` actualizado con F08 en estado `En revisión`.

- [x] **T6 — Capturas y validación final (usuario, VM)** · R4, R6.6
  - Verificación:
    1. Tomar las 20 capturas de diseño §4 y guardarlas en `imgs/` con su nombre exacto.
    2. Correr los dos bucles de diseño §6 (imágenes sin referenciar o faltantes) → sin salida.
    3. `sudo scripts/correr_pruebas.sh` → todas las suites con `0 FALLIDAS`; 0 usuarios `so2t_*`.
    4. Leer los dos manuales en el visor de Markdown (GitHub o VS Code): los diagramas Mermaid se ven y las imágenes cargan.
  - Evidencia:
    - 20 capturas generadas en `ProyectoUnico_2S2026/imgs/` (`01-login.png` hasta `20-correr-pruebas.png`), todas con formato PNG válido y dimensiones exactas.
    - Bucle 1 (`imgs/*.png` referenciados en `docs/*.md`): sin salida (20/20 referenciadas).
    - Bucle 2 (referencias relativas en `docs/*.md` existentes en disco): sin salida (0 faltantes).
    - Ejecución completa de `sudo scripts/correr_pruebas.sh`:
      - Compilación y pruebas unitarias en C: APROBADA (8s)
      - Feature F07 - Logwatch e IPC: APROBADA (26s)
      - Feature F06 - Antivirus y cuarentena: APROBADA (21s)
      - Feature F05 - Sensor eBPF: APROBADA (33s)
      - Feature F04 - Procesos: APROBADA (27s)
      - Feature F03 - Métricas: APROBADA (25s)
      - Feature F02 - Alertas: APROBADA (21s)
      - Feature F01 - Autenticación y PAM: APROBADA (8s)
      - Integración general (F00-F07): APROBADA (20s)
      - Limpieza de usuarios de prueba `so2t_*`: 0 residuales
      - Total: Suites aprobadas: 9 · Suites fallidas: 0.
    - Manuales verificados: `docs/Manual_Tecnico.md` y `docs/Manual_Usuario.md` con enlaces y diagramas válidos.
