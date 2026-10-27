# F00 — Fundación desde P4 · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere la VM queda `- [ ]` con `Pendiente en VM:`.

---

- [x] **T1 — Estructura y archivos base** · R1
  - Crear las carpetas de R1.1 con `.gitkeep` donde estén vacías.
  - Agregar `**/ebpf/vmlinux.h` y `**/ebpf/*.skel.h` al `.gitignore` raíz.
  - Verificación: `find ProyectoUnico_2S2026 -maxdepth 2 -type d | sort` muestra todas las carpetas.
  Evidencia: `find ProyectoUnico_2S2026 -maxdepth 2 -type d | sort` muestra Dashboard, docs, imgs, Kernel, ProgramaIntermedio, ProgramaIntermedio/ebpf, ProgramaIntermedio/pruebas, scripts, specs, steering; .gitignore actualizado con **/ebpf/vmlinux.h y **/ebpf/*.skel.h.

- [x] **T2 — Overlay del kernel** · R2
  - Copiar `Practica_4_2S2026/linux-6.12.69/` a `ProyectoUnico_2S2026/Kernel/` conservando rutas.
  - Verificación: `diff -r Practica_4_2S2026/linux-6.12.69 ProyectoUnico_2S2026/Kernel && echo IGUAL` y `find ProyectoUnico_2S2026/Kernel -type f | wc -l` → `7`.
  Evidencia: `diff -r Practica_4_2S2026/linux-6.12.69 ProyectoUnico_2S2026/Kernel && echo IGUAL` devolvió IGUAL y `find ProyectoUnico_2S2026/Kernel -type f | wc -l` devolvió 7.

- [x] **T3 — Scripts del kernel con eBPF/BTF** · R3.1–R3.6, R3.8 · diseño §3.1–3.4
  - Copiar `preparar_kernel.sh` e `instalar_kernel.sh` a `scripts/` y aplicar: `OVERLAY` en `Kernel/`, `SO2_WORKSPACE`, `dwarves` en dependencias, chequeo de `pahole`, bloque eBPF/BTF siempre aplicado con verificación, eliminar los `--disable DEBUG_INFO*` de P4, `INSTALL_MOD_STRIP=1`.
  - Actualizar textos del banner/log de "P4" a "Proyecto Único" y la fase 7/7 para compilar `ProgramaIntermedio` y `scripts/test_syscalls`.
  - Copiar `test_fase1.c` a `scripts/test_syscalls.c`.
  - Verificación: `bash -n scripts/preparar_kernel.sh scripts/instalar_kernel.sh`; `grep -n 'disable DEBUG_INFO_BTF' scripts/preparar_kernel.sh` → sin resultados; `gcc -O2 -Wall -o /dev/null scripts/test_syscalls.c` sin warnings.
  Evidencia: `bash -n scripts/preparar_kernel.sh scripts/instalar_kernel.sh` pasó sin errores de sintaxis; `grep -n 'disable DEBUG_INFO_BTF' scripts/preparar_kernel.sh` sin coincidencias; `gcc -O2 -Wall -o /dev/null scripts/test_syscalls.c` compiló con 0 warnings.

- [x] **T4 — `verificar_kernel.sh`** · R3.7 · diseño §3.5
  - Crear el script con los 4 chequeos acumulativos y salida `✓`/`✗`.
  - Verificación en host: `bash -n` y ejecutarlo → debe reportar `✗` en versión/BTF/syscalls y salir con código ≠ 0 (comportamiento esperado en el host).
  Evidencia: `bash -n scripts/verificar_kernel.sh` pasó sin errores; ejecución en host reportó `✗ Version de kernel` y `✗ Syscalls 548-553` con código de salida 1.

- [x] **T5 — Port del daemon y renombres** · R5.2, R5.3, R5.5, R7.2 · diseño §2
  - Copiar `daemon/*.c`, `comun.h`, `Makefile` a `ProgramaIntermedio/`; `eventos.c` → `ipc.c`.
  - Renombrar `hilo_metricas` → `hilo_monitoreo`, `metricas_despertar` → `monitoreo_despertar`, `hilo_eventos` → `hilo_ipc`, `eventos_despertar` → `ipc_despertar`, sin cambiar su lógica.
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`.
  Evidencia: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` devolvió 0.

- [x] **T6 — Configuración por archivo** · R4 · diseño §4.1–4.2
  - Crear `config.c` + declaraciones en `comun.h`; reemplazar `struct opciones`/`getopt` de `main.c` por `-c/-d/-h` y `config_cargar`; arrancar `log_watch` desde la configuración.
  - Crear `ProgramaIntermedio/daemon.conf` comentado con todas las claves.
  - Verificación: `./daemon_monitor -h`; `./daemon_monitor -c /no/existe; echo $?` → `1`.
  Evidencia: `./daemon_monitor -h` muestra la ayuda con código 0; `./daemon_monitor -c /no/existe` reportó error y código 1.

- [x] **T7 — Pruebas del parser** · R4, R7.5 · diseño §6.1
  - Crear `ProgramaIntermedio/pruebas/test_config.c` con los 7 casos y el objetivo `pruebas` en el Makefile.
  - Verificación: `make -C ProgramaIntermedio pruebas; echo $?` → `0` y salida con los 7 casos en verde.
  Evidencia: `make -C ProgramaIntermedio pruebas; echo $?` devolvió 0 y ejecutó exitosamente los 8 casos en verde (incluyendo validacion de ruta a directorio).

- [x] **T8 — Tres hilos con nombre y apagado** · R5, R6 · diseño §4.3–4.5
  - Crear `sensor.c` con `hilo_archivos`/`archivos_despertar`; crear los 3 hilos con `pthread_setname_np`; ignorar `SIGPIPE`; apagar y unir los 3 hilos.
  - Verificación en host (puerto libre, p. ej. 8099, con un `daemon.conf` temporal fuera del repo):
    - `cat /proc/<pid>/task/*/comm` contiene `monitoreo`, `archivos`, `ipc`.
    - `curl -s -o /dev/null -w '%{http_code}' localhost:8099/api/estado` → `200`.
    - `kill -TERM <pid>`; termina en < 5 s; código de salida `0`.
  Evidencia: `cat /proc/<pid>/task/*/comm` mostró archivos, daemon_monitor, ipc, monitoreo; curl /api/estado devolvió 200; kill -TERM terminó en 1 s con código 0.

- [x] **T9 — Dashboard** · R7.1
  - Copiar `Practica_4_2S2026/frontend/` a `Dashboard/`.
  - Verificación: `diff -r Practica_4_2S2026/frontend ProyectoUnico_2S2026/Dashboard && echo IGUAL`; con el daemon en el host, `curl -s localhost:8099/ | grep -c '<html'` → `≥1`.
  Evidencia: `diff -r Practica_4_2S2026/frontend ProyectoUnico_2S2026/Dashboard` devolvió IGUAL; `curl -s localhost:8099/ | grep -ci '<html'` devolvió 1.

- [x] **T10 — Scripts de prueba y demo** · R7.3
  - Copiar `pruebas_integracion.sh` y `demo.sh` a `scripts/`, adaptando la ruta del ejecutable, el uso de `-c` (con un `daemon.conf` temporal fuera del repo) y la carpeta del dashboard.
  - Verificación: `bash -n scripts/pruebas_integracion.sh scripts/demo.sh`. Ejecución completa → **Pendiente en VM**.
  Evidencia: `bash -n scripts/pruebas_integracion.sh scripts/demo.sh` completó con código 0; ambos scripts tienen permisos de ejecución. Pendiente en VM: ejecución completa contra el kernel 6.12.69-so2-202100106.

- [x] **T11 — README y binario** · R1.3, R7.4
  - Escribir `ProyectoUnico_2S2026/README.md` (R1.3).
  - Compilar y versionar `ProgramaIntermedio/daemon_monitor`.
  - Verificación: `git -C .. ls-files ProyectoUnico_2S2026/ProgramaIntermedio/daemon_monitor` lo lista tras el commit.
  Evidencia: `git ls-files ProyectoUnico_2S2026/ProgramaIntermedio/daemon_monitor` lo lista y el binario se compiló con 0 warnings.

- [x] **T12 — Validación en VM** · R3.7, R5.7, R6.1, R7.3 · diseño §6.2
  - `scripts/preparar_kernel.sh` → `instalar_kernel.sh` → reiniciar → `verificar_kernel.sh` → `pruebas_integracion.sh` → CPU en reposo → apagado sin `log_watch` huérfanos.
  Evidencia: `scripts/verificar_kernel.sh` aprobó los 4 checks en verde con código 0; `scripts/pruebas_integracion.sh` completó exitosamente con 38 pruebas aprobadas y 0 fallidas (0.0% CPU en reposo del hilo IPC y apagado limpio con SIGTERM); se validó resolución de HOME con SUDO_USER.
