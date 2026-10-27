# F00 — Fundación desde P4 · Requisitos

## Introducción

Llevar la base funcional de la Práctica 4 (overlay del kernel, daemon, dashboard y scripts) a la estructura de entrega del Proyecto Único y adaptarla a lo que exige el enunciado y que P4 no tenía: kernel con soporte eBPF/BTF, daemon configurado por archivo y esquema de tres hilos de trabajo (monitoreo, archivos, IPC).

Esta feature **no agrega funcionalidad nueva de producto** (login, alertas, eBPF, antivirus): deja el terreno listo y conserva todo lo que P4 ya hacía.

## Glosario

- **P4:** `Practica_4_2S2026/`, de solo lectura.
- **Overlay:** archivos del kernel modificados, con su ruta original dentro del árbol `linux-6.12.69`.
- **Árbol de compilación:** fuentes completas de Linux 6.12.69 donde se aplica el overlay y se compila.
- **Host:** máquina sin el kernel del curso (syscalls 548–553 devuelven `ENOSYS`).
- **VM:** máquina arrancada en `6.12.69-so2-202100106`.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Estructura del proyecto

**Historia:** Como estudiante, quiero la estructura de carpetas que fija el enunciado, para que la entrega sea válida desde el primer commit.

1. EL repositorio DEBERÁ contener dentro de `ProyectoUnico_2S2026/` las carpetas `Kernel/`, `ProgramaIntermedio/`, `ProgramaIntermedio/ebpf/`, `ProgramaIntermedio/pruebas/`, `Dashboard/`, `scripts/`, `docs/` e `imgs/`, según `steering/structure.md`.
2. Las carpetas aún sin contenido (`ProgramaIntermedio/ebpf/`, `docs/`, `imgs/`) DEBERÁN versionarse con un `.gitkeep`.
3. EL proyecto DEBERÁ incluir un `README.md` con: descripción breve, requisitos, pasos para compilar e instalar el kernel, compilar y ejecutar el daemon, abrir el dashboard, y enlaces a `steering/`, `specs/` y `AGENTS.md`.
4. EL `.gitignore` de la raíz DEBERÁ ignorar `**/ebpf/vmlinux.h` y `**/ebpf/*.skel.h`.

## R2 — Overlay del kernel

**Historia:** Como evaluador, quiero ver solo los archivos del kernel modificados con su ruta original, para revisar la integración de las syscalls.

1. `Kernel/` DEBERÁ contener exactamente los 7 archivos del overlay de P4, con la misma ruta relativa: `arch/x86/entry/syscalls/syscall_64.tbl`, `include/linux/syscalls.h`, `include/linux/ipc_channel.h`, `kernel/Makefile`, `kernel/mi_syscall.c`, `kernel/ipc_channel.c`, `kernel/log_watch.c`.
2. `diff -r Practica_4_2S2026/linux-6.12.69 ProyectoUnico_2S2026/Kernel` NO DEBERÁ mostrar diferencias.
3. `Kernel/` NO DEBERÁ contener archivos del kernel sin modificar.

## R3 — Kernel con soporte eBPF

**Historia:** Como desarrollador, quiero que el script de compilación deje el kernel listo para eBPF con BTF, para que el sensor de la F05 cargue sin recompilar otra vez.

1. CUANDO se ejecute `scripts/preparar_kernel.sh`, EL script DEBERÁ aplicar el overlay desde `Kernel/` (no desde P4).
2. CUANDO se ejecute `scripts/preparar_kernel.sh`, exista o no un `.config` previo, EL script DEBERÁ dejar en `.config` con valor `=y`: `CONFIG_BPF`, `CONFIG_BPF_SYSCALL`, `CONFIG_BPF_EVENTS`, `CONFIG_DEBUG_INFO_BTF` y `CONFIG_FTRACE_SYSCALLS`.
3. SI después de `make olddefconfig` alguna de esas opciones no quedó en `=y`, EL script DEBERÁ abortar antes de compilar, indicando la opción faltante.
4. SI `pahole` no está instalado o su versión es menor a 1.16, EL script DEBERÁ abortar antes de compilar con un mensaje que indique instalar `dwarves`.
5. EL script DEBERÁ reutilizar el árbol de compilación existente. La ruta DEBERÁ poder cambiarse con la variable de entorno `SO2_WORKSPACE`; por defecto será `Practica_3_2S2026/so2_build_workspace` (la misma de P4).
6. CUANDO se ejecute `scripts/instalar_kernel.sh`, EL script DEBERÁ instalar los módulos sin símbolos de depuración (`INSTALL_MOD_STRIP=1`) y usar el mismo árbol que `preparar_kernel.sh`.
7. EL proyecto DEBERÁ incluir `scripts/verificar_kernel.sh`, que en la VM compruebe: `uname -r` igual a `6.12.69-so2-202100106`, existencia de `/sys/kernel/btf/vmlinux`, las 5 opciones del punto 2 en `/boot/config-$(uname -r)` y las 6 syscalls mediante `scripts/test_syscalls`. DEBERÁ terminar con código 0 solo si todo pasa y mostrar qué falló en caso contrario.
8. `scripts/test_syscalls.c` DEBERÁ provenir de `Practica_4_2S2026/test_fase1.c` y conservar sus validaciones.

## R4 — Archivo de configuración del daemon

**Historia:** Como administrador, quiero configurar el daemon con un archivo, porque el enunciado pide que `log_watch` arranque con los parámetros de su archivo de configuración.

1. EL daemon DEBERÁ aceptar `-c <archivo>`, `-d` (segundo plano) y `-h` (ayuda). Las demás opciones de línea de comandos de P4 se eliminan.
2. SI no se pasa `-c`, EL daemon DEBERÁ buscar `daemon.conf` en la carpeta del ejecutable; SI tampoco existe, DEBERÁ usar los valores por defecto y registrar una advertencia.
3. SI se pasa `-c` con un archivo inexistente o ilegible, EL daemon DEBERÁ terminar con código 1 y un mensaje claro, **antes** de daemonizarse.
4. EL archivo DEBERÁ tener formato `clave = valor`, admitir líneas vacías y comentarios con `#`, y reconocer estas claves:

   | Clave | Tipo | Por defecto |
   |---|---|---|
   | `puerto` | entero 1–65535 | `8080` |
   | `intervalo` | segundos, entero 1–60 | `2` |
   | `dashboard` | ruta a la carpeta del frontend | `../Dashboard` relativo al ejecutable |
   | `log_central` | ruta | `~/so2_logs/log_centralizado.txt` |
   | `log_rutas` | lista separada por comas (máx. 8) | `~/so2_logs/auth.log,~/so2_logs/app.log` |
   | `log_palabras` | lista separada por comas (máx. 8) | `ERROR,FAIL` |
   | `ruta_vigilada` | ruta de directorio | `~/so2_vigilado` |
   | `blacklist` | ruta a JSON | `blacklist.json` relativo al ejecutable |

5. SI una clave es desconocida o un valor es inválido, EL daemon DEBERÁ registrar una advertencia con el número de línea y usar el valor por defecto de esa clave, sin terminar.
6. EL daemon DEBERÁ expandir `~` y convertir a rutas absolutas todas las rutas antes de usarlas.
7. CUANDO arranque, EL daemon DEBERÁ iniciar un `sys_start_log_watch` por cada palabra de `log_palabras` con las rutas de `log_rutas` y el `log_central`, igual que P4.
8. `ruta_vigilada` y `blacklist` se leen y guardan, pero no se usan hasta F05/F06.
9. EL proyecto DEBERÁ incluir `ProgramaIntermedio/daemon.conf` con todas las claves documentadas en comentarios.

## R5 — Esquema de hilos

**Historia:** Como evaluador, quiero ver los tres hilos obligatorios con responsabilidades separadas, para verificar el requisito de concurrencia.

1. EL daemon DEBERÁ crear, además del hilo principal, tres hilos llamados `monitoreo`, `archivos` e `ipc` (visibles en `/proc/<pid>/task/*/comm`).
2. EL hilo `monitoreo` DEBERÁ realizar el muestreo periódico de P4 (memoria y procesos) con el `intervalo` configurado.
3. EL hilo `ipc` DEBERÁ permanecer bloqueado en `sys_ipc_channel_receive` y procesar los eventos como en P4, sin polling.
4. MIENTRAS no exista el sensor (F05), EL hilo `archivos` DEBERÁ quedar bloqueado sin consumir CPU hasta el apagado.
5. EL servidor HTTP/SSE DEBERÁ seguir en el hilo principal.
6. SI la creación de un hilo falla, EL daemon DEBERÁ registrarlo y seguir con los demás.
7. MIENTRAS el daemon esté en reposo, su uso de CPU DEBERÁ ser `0.0 %` en `top` fuera de los instantes de muestreo, como en P4.

## R6 — Resiliencia y apagado limpio

**Historia:** Como administrador, quiero que el daemon no se caiga por errores y que se detenga sin dejar hilos de kernel huérfanos.

1. CUANDO reciba `SIGINT` o `SIGTERM`, EL daemon DEBERÁ detener todos los `log_watch` iniciados, despertar y esperar a los tres hilos, cerrar los sockets y terminar con código 0 en menos de 5 segundos.
2. EL daemon DEBERÁ ignorar `SIGPIPE`; la desconexión de un cliente SSE no lo termina.
3. SI una syscall devuelve error (incluido `ENOSYS` en el host), EL daemon DEBERÁ registrarlo y seguir atendiendo HTTP.

## R7 — Paridad con P4

**Historia:** Como estudiante, quiero que todo lo que funcionaba en P4 siga funcionando, para construir encima sin regresiones.

1. `Dashboard/` DEBERÁ contener el frontend de P4 y el daemon DEBERÁ servirlo en `/` desde la ruta `dashboard` de la configuración.
2. Los endpoints `GET /api/estado`, `GET /api/eventos` (SSE) y `GET /api/proceso?pid=N` DEBERÁN responder con el mismo contrato JSON de P4 (`Practica_4_2S2026/steering/ESPECIFICACION.md`, sección 5.4).
3. `scripts/pruebas_integracion.sh` y `scripts/demo.sh` DEBERÁN provenir de P4, adaptados a las rutas nuevas y a `-c`, y `pruebas_integracion.sh` DEBERÁ pasar en la VM.
4. `make -C ProgramaIntermedio clean all` DEBERÁ compilar sin warnings, y el ejecutable `ProgramaIntermedio/daemon_monitor` DEBERÁ versionarse.
5. `make -C ProgramaIntermedio pruebas` DEBERÁ compilar y correr las pruebas de `ProgramaIntermedio/pruebas/` en el host, terminando con código 0.
