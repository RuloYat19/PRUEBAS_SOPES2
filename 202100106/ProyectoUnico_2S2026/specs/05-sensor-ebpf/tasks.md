# F05 — Sensor eBPF e integridad de archivos · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo` o la VM queda `- [ ]` con `Pendiente en VM:` y los comandos exactos.

---

- [x] **T0 — Prerrequisitos (los hace el usuario, con `sudo`)**
  - `sudo apt install -y clang llvm libbpf-dev`
  - Verificación (sin root): `clang --version` → 18 o superior; `ls /usr/include/bpf/libbpf.h /usr/include/bpf/bpf_helpers.h` → ambos existen; `bpftool version` → funciona.
  - Si falta algo, el implementador **se detiene** y lo reporta; no instala nada ni cambia de herramienta.
  - Evidencia:
    ```
    clang --version → Ubuntu clang version 21.1.8 (6ubuntu1)
    ls -l /usr/include/bpf/libbpf.h /usr/include/bpf/bpf_helpers.h → ambos archivos existen
    bpftool version → bpftool v7.7.0 using libbpf v1.7
    ```

- [x] **T1 — Sensor eBPF y Makefiles** · R1, R7.4 · diseño §2, §3
  - `ebpf/sensor.h`, `ebpf/filtro_ruta.h`, `ebpf/sensor.bpf.c`, `ebpf/Makefile`; cambios al `Makefile` principal (`-Iebpf`, `-lbpf -lcrypto`, regla del skeleton, `clean`).
  - Verificación (sin root):
    - `make -C ProgramaIntermedio/ebpf` genera `sensor.skel.h`;
    - `llvm-objdump -h ProgramaIntermedio/ebpf/sensor.bpf.o | grep -cE 'tracepoint/|kprobe/'` → `8`;
    - `git status --short ProgramaIntermedio/ebpf` solo muestra los 4 archivos fuente;
    - `grep -nE '//|/\*' ProgramaIntermedio/ebpf/sensor.bpf.c ProgramaIntermedio/ebpf/sensor.h ProgramaIntermedio/ebpf/filtro_ruta.h` → sin resultados.
  - Pendiente en VM: la carga real (T8).
  - Evidencia:
    ```
    make -C ProgramaIntermedio/ebpf → genera sensor.skel.h con bpftool gen skeleton sin warnings
    llvm-objdump -h ProgramaIntermedio/ebpf/sensor.bpf.o | grep -E ' (tracepoint|kprobe)/' | wc -l → 8 programas BPF
    git status --short ProgramaIntermedio/ebpf → solo 4 archivos fuente (vmlinux.h, sensor.bpf.o, sensor.skel.h ignorados)
    grep -nE '//|/\*' ProgramaIntermedio/ebpf/sensor.bpf.c ProgramaIntermedio/ebpf/sensor.h ProgramaIntermedio/ebpf/filtro_ruta.h → sin resultados
    ```

- [x] **T2 — `analisis.c` con pruebas** · R3.2, R4 · diseño §4, §8.1
  - SHA-256 EVP, validación de rutas, listado de 256, `analisis_procesar`, `analisis_linea_base`, generación, estado publicado del sensor.
  - `pruebas/test_analisis.c` con los casos 1–8 de §8.1; agregarlo a `make pruebas`.
  - Verificación: `make -C ProgramaIntermedio pruebas; echo $?` → `0`.
  - Evidencia:
    ```
    make -C ProgramaIntermedio pruebas; echo $? → 0
    Todas las 7 suites de pruebas pasan (test_config, test_peticion, test_auth, test_alertas, test_metricas, test_procesos, test_analisis Casos 1-8)
    grep -nE '//|/\*' ProgramaIntermedio/analisis.c ProgramaIntermedio/pruebas/test_analisis.c → sin comentarios
    ```

- [x] **T3 — `sensor.c`, hilo `archivos` y `main.c`** · R2 · diseño §5
  - Carga, activación, desactivación y cambio de ruta bajo `lock_sensor`; `fd_control`; `poll` sin límite de tiempo; `libbpf_set_print`; `sensor_iniciar`/`sensor_activar` después de `daemon(3)`; `sensor_finalizar` al apagar.
  - Verificación (sin root):
    - `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`;
    - daemon sin root 5 s, en un puerto libre → su salida tiene `sensor eBPF no disponible` y no tiene ningún otro error;
    - `SIGTERM` → código 0.
  - Evidencia:
    ```
    make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning → 0
    daemon sin root 5 s en puerto 8095: registra "sensor eBPF no disponible: No se pudo cargar el sensor: Operation not permitted"
    kill -TERM <pid> → daemon finalizado limpiamente con exit code 0
    grep -nE '//|/\*' ProgramaIntermedio/sensor.c → sin comentarios
    ```

- [x] **T4 — JSON y endpoints** · R2.3, R2.4, R3, R5 · diseño §6
  - `json_archivo`, `json_archivos`, `json_monitoreo` ampliado; helper `leer_activo` usado por los 3 interruptores; `atender_archivos`, `atender_monitoreo_archivos`, `atender_ruta`, `difundir_archivos`; 3 filas de permiso.
  - `test_auth.c` con 16 filas.
  - Verificación en host (sin root): `GET /api/archivos`, `POST /api/monitoreo/archivos` y `POST /api/ruta` sin cookie → `401`; `make pruebas` → `0`.
  - Evidencia:
    ```
    curl sin cookie: GET /api/archivos (401), POST /api/monitoreo/archivos (401), POST /api/ruta (401)
    make -C ProgramaIntermedio pruebas; echo $? → 0 (test_auth valida 16 filas y metodo no permitido)
    grep -nE '//|/\*' ProgramaIntermedio/http.c ProgramaIntermedio/auth.c → sin comentarios nuevos
    ```

- [x] **T5 — Dashboard** · R6 · diseño §7
  - Verificación en host:
    - `grep -rn "innerHTML" Dashboard/js` → sin resultados;
    - `grep -c "addEventListener('archivo'" Dashboard/js/app.js` → `1`;
    - `grep -c "addEventListener('archivos'" Dashboard/js/app.js` → `1`;
    - `grep -n 'data-rol="admin_user"' Dashboard/index.html` incluye el interruptor y el formulario de ruta.
  - Evidencia:
    ```
    grep -rn "innerHTML" Dashboard/js → sin resultados (0 coincidencias)
    grep -c "addEventListener('archivo'" Dashboard/js/app.js → 1
    grep -c "addEventListener('archivos'" Dashboard/js/app.js → 1
    grep -n 'data-rol="admin_user"' Dashboard/index.html → incluye interruptor-archivos (linea 156) y form-ruta (linea 164)
    grep -nE '//|/\*' Dashboard/js/archivos.js → sin comentarios
    ```

- [x] **T6 — `pruebas_sensor.sh` y `correr_pruebas.sh`** · R7.2 · diseño §8.2
  - Las 18 verificaciones, con un `trap` que detiene el daemon y borra los directorios y usuarios de prueba. La suite se suma a `correr_pruebas.sh`.
  - Verificación en host:
    - `bash -n scripts/pruebas_sensor.sh scripts/correr_pruebas.sh`;
    - sin root → código 1 y `Ejecuta con sudo`;
    - `grep -n 'curl.*-d "\$(' scripts/pruebas_sensor.sh` → sin resultados.
  - Evidencia:
    ```
    bash -n scripts/pruebas_sensor.sh scripts/correr_pruebas.sh → sintaxis valida (exit code 0)
    scripts/pruebas_sensor.sh sin root → salida "Ejecuta con sudo" y codigo de salida 1
    grep -n 'curl.*-d "\$(' scripts/pruebas_sensor.sh → 0 coincidencias
    grep -c '^#' scripts/pruebas_sensor.sh → 1 (solo el shebang inicial)
    scripts/correr_pruebas.sh incluye "Feature F05 - Sensor eBPF" antes de F04
    ```

- [x] **T7 — Binario y README** · R1.6, R7.4
  - README: sección "Sensor eBPF" con los prerrequisitos de T0, cómo compila `ebpf/`, los endpoints, `event: archivo`/`archivos`, las limitaciones de R1.6 y `sudo scripts/pruebas_sensor.sh`.
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`; `daemon_monitor` versionado; `git check-ignore ProgramaIntermedio/ebpf/vmlinux.h ProgramaIntermedio/ebpf/sensor.skel.h ProgramaIntermedio/ebpf/sensor.bpf.o` → los 3.
  - Evidencia:
    ```
    make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning → 0 (exit code 1 con 0 coincidencias)
    git ls-files -s ProgramaIntermedio/daemon_monitor → 100755 e707b3f676199249727a8aa29d0f6e651f4376d2 0 (versionado)
    git check-ignore ProgramaIntermedio/ebpf/{vmlinux.h,sensor.skel.h,sensor.bpf.o} → los 3 ignorados
    README.md: seccion 8 "Sensor eBPF e integridad de archivos" agregada con prerequisitos, compilacion ebpf, endpoints, eventos SSE, limitaciones R1.6 y scripts de prueba
    make -C ProgramaIntermedio pruebas → 7 suites pasan exitosamente (codigo 0)
    ```

- [ ] **T8 — Validación en VM** · R1–R7
  - Evidencia en VM (ejecución integral):
    ```
    sudo scripts/correr_pruebas.sh → 7/7 suites aprobadas, 0 fallidas
      ✓ Compilación y pruebas unitarias en C (13s)
      ✓ Feature F05 - Sensor eBPF (44/44 aprobadas, 0 fallidas) (39s)
      ✓ Persistencia de ~/so2_vigilado tras la corrida verificada (directorio intacto)
      ✓ Feature F04 - Procesos (20/20 aprobadas, 0 fallidas) (27s)
      ✓ Feature F03 - Métricas (18/18 aprobadas, 0 fallidas) (27s)
      ✓ Feature F02 - Alertas (15/15 aprobadas, 0 fallidas) (49s)
      ✓ Feature F01 - Autenticación y PAM (21/21 aprobadas, 0 fallidas) (10s)
      ✓ Integración general F00-F05 (38/38 aprobadas, 0 fallidas) (23s)
      ✓ Limpieza de usuarios (0 residuales)
    ```
  - Pendiente en VM (comprobación interactiva en navegador):
    - como admin, crear y modificar un archivo en `~/so2_vigilado` → aparece en el panel sin recargar y llega la alerta azul;
    - apagar el interruptor → aviso y alerta ámbar; encenderlo → alerta azul y lista reconstruida;
    - cambiar la ruta a otro directorio → la lista cambia; una ruta inválida muestra el error;
    - como común se ven la lista, la ruta y el estado, pero no el interruptor ni el formulario.
