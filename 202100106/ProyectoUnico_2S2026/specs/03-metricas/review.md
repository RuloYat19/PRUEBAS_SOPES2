# Revisión F03 — Métricas del sistema

Fecha: 2026-10-18 · Revisor: Claude · Commits revisados: `5f1198ee27`…`ad35de8268`

## Veredicto

**Aprobada.** El código cumple el spec y toda la validación en la VM con `sudo` pasó. Quedan dos advertencias sobre la precisión de una prueba y una carrera menor. Comprobación en el navegador confirmada por el usuario. Las dos advertencias se corrigen en F04 (tarea T1 de `specs/04-procesos/tasks.md`).

## Validación en la VM (ejecutada por el usuario)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all pruebas` | 0 warnings; config 9, peticion 3, auth 11 (10 filas de permiso), alertas 10, métricas 11/11 |
| `sudo scripts/pruebas_metricas.sh` | 18 aprobadas, 0 fallidas (las 15 fases de §7.2) |
| `sudo scripts/pruebas_alertas.sh` | 15/0 |
| `sudo scripts/pruebas_auth.sh` | 21/0 |
| `sudo scripts/pruebas_integracion.sh` | 38/0; hilo `ipc` con 0 tics en reposo |
| `getent passwd \| grep -c '^so2t_'` | 0 |

## Revisión del código

| Punto | Resultado |
|---|---|
| `metricas_tomar` (`metricas.c:121`) | lee fuera del mutex; calcula y guarda bajo `lock_metricas`; `estado.lock` y `eventfd_write` fuera de él ✓ |
| `/api/estado` | conserva las definiciones de P4 (`Total − Available`, etc.) ✓ |
| `metricas_cambiar_estado` (`:245`) | `__atomic_exchange_n`: una sola alerta por cambio aunque lleguen dos peticiones a la vez; al activar reinicia `hay_anterior` y despierta el hilo con `pendiente_muestra` ✓ |
| Bucle del hilo (`:372`) | sale de la espera por `pendiente_muestra` o por `ETIMEDOUT`; los procesos se recolectan aunque las métricas estén apagadas ✓ |
| `event: monitoreo` (`http.c:265`) | trama completa con `\n\n`, enviada desde el hilo HTTP ✓ |
| Dashboard | sin `innerHTML` en ningún JS; interruptor dentro de `data-rol="admin_user"` ✓ |
| Comentarios en archivos nuevos o tocados | ninguno |

## Advertencias (no bloquean)

1. **La fase 13 de `pruebas_metricas.sh` no distingue la muestra inmediata.**
   - Corregido: La fase 13 de `scripts/pruebas_metricas.sh` ahora detiene el daemon, reconfigura `intervalo = 5`, desactiva métricas, espera 6 s y comprueba que `event: metricas` con `"primera":true` llega en menos de 1 s tras reactivar.
   - Evidencia: `bash -n scripts/pruebas_metricas.sh` → OK.
2. **Carrera menor al desactivar.**
   - Corregido: `metricas_tomar` en `metricas.c` re-verifica `metricas_activas()` bajo `lock_metricas` antes de persistir la muestra; si ya está inactivo, libera el mutex y descarta la muestra retornando 0.
   - Evidencia: `make -C ProgramaIntermedio pruebas` → 0 warnings, todas las pruebas pasaron exitosamente.

## Nota para F04

El dashboard sigue consultando `/api/estado` cada intervalo, y esa consulta es lo que renueva la sesión mientras la pestaña está abierta (F01 R3.11: el SSE no la renueva). Si F04 mueve los procesos al SSE, debe mantener una petición periódica autenticada o renovar la sesión de otra forma.

## Comprobación manual (usuario, 2026-10-18), en el navegador ✓

- las 4 gráficas se mueven cada intervalo;
- como admin, apagar el interruptor muestra el aviso en las 4 gráficas y una alerta ámbar;
- al encenderlo aparece una alerta azul y las gráficas siguen;
- como usuario común no se ve el interruptor, pero sí el estado en texto.
