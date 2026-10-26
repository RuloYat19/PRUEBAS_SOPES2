# Integración de Kernel

Este directorio representa los archivos modificados que deben copiarse al árbol fuente del kernel 6.18.48. **No compiles `Kernel/` como un proyecto independiente**: los archivos deben integrarse dentro de `linux-6.18.48/kernel/`, junto con sus entradas de syscall y sus prototipos.

## Antes de usar

1. Haz una copia del kernel 6.18.48 que ya compila.
2. Compara `kernel/process_monitor.c`, `kernel/ipc_channel.c` y `kernel/log_watch.c` con los archivos reales que ya compilaste en tus prácticas. Mantén las implementaciones que ya verificaste si son más completas.
3. La estructura del usuario debe coincidir exactamente con las estructuras del kernel. El código de `daemon.c` de este proyecto asume `struct process_info_user` y `struct processes_state_user` definidos en `process_monitor.c`.
4. En `arch/x86/entry/syscalls/syscall_64.tbl`, confirma los números acordados en las prácticas: 470–475. No dupliques números que ya estén asignados en tu árbol.
5. Asegura los prototipos en `include/linux/syscalls.h` según las firmas de `SYSCALL_DEFINE*` usadas. Añade `obj-y += process_monitor.o`, `obj-y += ipc_channel.o` y `obj-y += log_watch.o` en `kernel/Makefile`.
6. Compila y arranca el kernel antes de probar el daemon. No ejecutes código de kernel no probado en un equipo de uso diario; usa tu VM.

## Advertencias de integración

- La API de `get_process_info` aquí usa `(pid, user_ptr)` y `get_processes_state` usa `(user_ptr)`. Si las firmas de tus prácticas son distintas, actualiza el daemon y los prototipos para que coincidan.
- El archivo `log_watch.c` es una implementación de referencia para comparar con tu versión de Práctica 3. Las APIs de acceso a archivos en kernel son delicadas; la versión ya compilada y probada en tu árbol debe tener prioridad y verificarse en 6.18.48.
- Las llamadas de `syscall_64.tbl` deben registrarse con nombres y wrappers correctos; copiar únicamente los `.c` no registra las syscalls.
