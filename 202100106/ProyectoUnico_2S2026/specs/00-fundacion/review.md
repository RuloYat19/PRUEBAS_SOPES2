# Revisión F00 — Fundación desde P4

Fecha: 2026-10-18 · Revisor: Claude · Commit revisado: `ee783416c6`

## Veredicto

**Aprobada.** 12/12 tareas con evidencia reproducida de forma independiente en la VM.

## Verificación independiente

Ejecutado en la VM con el kernel `6.12.69-so2-202100106` en ejecución:

| Chequeo | Resultado |
|---|---|
| `./scripts/verificar_kernel.sh` | código 0, 4/4 checks en verde (versión, BTF, config eBPF/BTF, syscalls 548-553) |
| `make -C ProgramaIntermedio clean all \| grep -ci warning` | 0 |
| `make -C ProgramaIntermedio pruebas` | código 0, 8/8 casos (incluye ruta a directorio) |
| `./scripts/pruebas_integracion.sh` | código 0, 38 aprobadas, 0 fallidas |
| Procesos huérfanos tras las pruebas | ninguno (`daemon_monitor`, `log_watch`) |
| Árbol de trabajo tras recompilar | limpio: el binario versionado coincide con la compilación |
| Comentarios en C, Bash, Makefile, Dashboard | ninguno, aparte del shebang |
| `scripts/test_syscalls` (binario) | ignorado en `.gitignore`, no versionado |

## Hallazgos

Ninguno bloqueante.

### Advertencias (no bloquean)

1. `ProgramaIntermedio/config.c` (`obtener_home`) usa `SUDO_USER` aunque el proceso no corra como root. Un usuario sin privilegios puede definir `SUDO_USER=otro` y hacer que `~` apunte al home de otro usuario. El impacto es bajo porque es su propia configuración, pero lo correcto es usar `SUDO_USER` solo cuando `geteuid() == 0`. Se puede corregir en una feature posterior.
2. La validación con `SUDO_USER` (T12) no la reproduje: requiere `sudo` interactivo. Queda respaldada solo por la evidencia del implementador.

## Siguiente paso

F00 cerrada. Se puede escribir el spec de la siguiente feature.
