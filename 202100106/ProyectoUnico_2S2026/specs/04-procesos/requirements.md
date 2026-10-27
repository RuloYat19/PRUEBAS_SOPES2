# F04 — Procesos (top 20, PID, kill) · Requisitos

## Introducción

Completar el módulo de procesos del enunciado (§2.2.3.2, §2.2.3.7 y §2.2.4.5):
- obtener periódicamente el estado de los procesos con `sys_get_processes_state` y armar en espacio de usuario el **top 20 por memoria** (PID, nombre y % de memoria);
- dejar que el administrador active o desactive ese escaneo;
- dejar que el administrador termine un proceso con `kill(2)` (`sys_kill`), con confirmación y con las guardas obligatorias: ni PID 1, ni el propio daemon, ni hilos del kernel.

Además corrige las dos advertencias de la revisión de F03 (R6).

Ya existe y no cambia: la consulta por PID con `sys_get_process_info` (`GET /api/proceso`, solo admin, F01).

## Glosario

- **RSS:** memoria residente del proceso, `VmRSS` de `/proc/<pid>/status`, en kB.
- **% de memoria:** `RSS / MemTotal × 100`, con 2 decimales.
- **Hilo del kernel:** tarea con el flag `PF_KTHREAD` (`0x00200000`) en el campo 9 de `/proc/<pid>/stat`, o el propio `kthreadd` (PID 2).

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Top 20 por memoria

1. En cada intervalo, EL hilo `monitoreo` DEBERÁ:
   - llamar a `sys_get_processes_state` para el resumen (total, running, sleeping, zombie);
   - leer de cada `/proc/<pid>` el nombre, el estado y `VmRSS`.
2. EL daemon DEBERÁ ordenar en espacio de usuario por RSS descendente (a igual RSS, por PID ascendente) y quedarse con los **20** primeros.
3. Cada entrada del top DEBERÁ incluir `pid`, `nombre`, `estado`, `rss_kb` y `porcentaje_memoria`. `MemTotal` se obtiene con `sysinfo(2)`, así que el porcentaje no depende de que las métricas de F03 estén activas.
4. Un proceso sin `VmRSS` (hilos del kernel) cuenta con RSS 0. Un proceso que desaparece durante la lectura se omite sin error.
5. `GET /api/estado` DEBERÁ conservar su contrato (`procesos` sigue siendo el listado completo de P4 con `nombre`, `pid` y `estado`), para no romper `pruebas_integracion.sh`.

## R2 — API y tiempo real

1. `GET /api/procesos` (rol `common_user`) DEBERÁ responder `200` con:

   ```json
   {"activo":true,"timestamp":"2026-10-18 14:00:00",
    "resumen":{"total":…,"running":…,"sleeping":…,"zombie":…},
    "top":[{"pid":…,"nombre":"…","estado":"S","rss_kb":…,"porcentaje_memoria":12.34}, …]}
   ```

2. CUANDO termine un escaneo, EL daemon DEBERÁ enviar ese mismo cuerpo a los clientes SSE como `event: procesos`.
3. `GET /api/monitoreo` DEBERÁ incluir `"procesos":true|false` además de `"metricas"`, y `event: monitoreo` DEBERÁ enviar el cuerpo completo.

## R3 — Activar y desactivar el escaneo de procesos

1. `POST /api/monitoreo/procesos` (rol `admin_user`) DEBERÁ funcionar igual que `/api/monitoreo/metricas` de F03: cuerpo `{"activo":bool}`, `400` con `{"error":"Solicitud invalida"}` si es inválido, y respuesta `{"procesos":<estado>,"cambio":<bool>}`.
2. CUANDO el estado cambie, EL daemon DEBERÁ emitir `Monitoreo del sistema desactivado` (`MEDIUM`) con `Escaneo de procesos desactivado por <usuario>`, o `Monitoreo del sistema activado` (`LOW`) con `Escaneo de procesos activado por <usuario>`, y `event: monitoreo`. SI no cambia, no hay alerta.
3. MIENTRAS esté desactivado, EL daemon NO DEBERÁ llamar a `sys_get_processes_state` ni recorrer `/proc`, ni emitir `event: procesos`. `/api/procesos` y `/api/estado` devuelven los últimos datos, y `/api/procesos` con `"activo":false`.
4. CUANDO se reactive, el siguiente escaneo DEBERÁ ocurrir en menos de 1 s.
5. Métricas (F03) y procesos se activan y desactivan de forma independiente.
6. MIENTRAS métricas y procesos estén desactivados a la vez, el hilo `monitoreo` NO DEBERÁ despertarse por tiempo; DEBERÁ esperar sin límite hasta una reactivación o el apagado.

## R4 — Terminar procesos

1. `POST /api/proceso/kill` (rol `admin_user`) DEBERÁ recibir `{"pid":<entero>}`.
2. SI el cuerpo es inválido o `pid` no es un entero entre 1 y 4194304, EL daemon DEBERÁ responder `400` con `{"error":"PID invalido"}`.
3. EL daemon DEBERÁ rechazar con `403`, sin enviar ninguna señal:
   - PID 1 → `{"error":"No se permite terminar el proceso con PID 1"}`;
   - el propio daemon o **cualquiera de sus hilos** (`Tgid` igual a `getpid()`) → `{"error":"No se permite terminar el propio daemon"}`;
   - un hilo del kernel → `{"error":"No se permite terminar hilos del kernel"}`.
4. SI el PID corresponde a un hilo que no es el líder de su grupo (`Tgid ≠ pid`), EL daemon DEBERÁ responder `400` con `{"error":"El PID no corresponde a un proceso"}`.
5. SI el proceso no existe, EL daemon DEBERÁ responder `404` con `{"error":"Proceso no encontrado"}`.
6. Si pasa las guardas, EL daemon DEBERÁ enviar `SIGKILL` con `kill(2)` y responder `200` con `{"pid":…,"nombre":"…","senal":"SIGKILL"}`. SI `kill` falla con `ESRCH`, `404`; con otro error, `500` con `{"error":"No se pudo terminar el proceso"}`.
7. CUANDO el `kill` tenga éxito, EL daemon DEBERÁ emitir `Proceso matado` (`MEDIUM`) con `Proceso <nombre> (PID <pid>) terminado por <usuario>`.
8. Las guardas DEBERÁN validarse en C, en el daemon; la confirmación del dashboard es solo presentación.

## R5 — Dashboard

1. La tabla de procesos DEBERÁ mostrar el top 20 de `event: procesos`, con columnas PID, Nombre, Estado, Memoria (MB, 1 decimal) y % de memoria (con una barra proporcional). Se cargan con `GET /api/procesos` al abrir o reabrir el SSE.
2. Las tarjetas de resumen (total, running, sleeping, zombie) DEBERÁN alimentarse del mismo evento.
3. EL dashboard DEBERÁ dejar de sondear `/api/estado`. Para mantener viva la sesión (F01 R3.11), DEBERÁ hacer `GET /api/sesion` cada **60 s**; un `401` redirige al login como siempre.
4. EL administrador DEBERÁ ver:
   - un interruptor **Escaneo de procesos** que refleja `event: monitoreo`;
   - en cada fila, un botón **Terminar**.
5. **Terminar** DEBERÁ pedir confirmación con el texto `¿Terminar el proceso <nombre> (PID <pid>)? Esta acción puede afectar la estabilidad del sistema.` y solo con confirmación llamar a `POST /api/proceso/kill`. El resultado o el `error` se muestran en un aviso junto a la tabla.
6. Al hacer clic en una fila, el administrador DEBERÁ ver su detalle en el panel **Consulta por PID**, como en P4.
7. EL usuario común NO DEBERÁ ver el interruptor ni los botones; DEBERÁ ver el estado `Procesos: activos/desactivados` en texto.
8. MIENTRAS el escaneo esté desactivado, la tabla DEBERÁ mostrar el aviso `Escaneo de procesos desactivado` y conservar los últimos datos.
9. Solo `textContent`; nada de `innerHTML`.

## R6 — Correcciones pendientes de F03

1. `metricas_tomar` DEBERÁ volver a comprobar `metricas_activas()` bajo `lock_metricas` antes de guardar, y descartar la muestra si el muestreo ya se desactivó (advertencia 2 de `specs/03-metricas/review.md`).
2. La fase 13 de `pruebas_metricas.sh` DEBERÁ ejecutarse contra un daemon con `intervalo = 5` y exigir la muestra en menos de 1 s, de modo que distinga la muestra inmediata de una muestra normal (advertencia 1).

## R7 — Pruebas y paridad

1. `make -C ProgramaIntermedio pruebas` DEBERÁ incluir `pruebas/test_procesos.c`, con un árbol `/proc` falso en un directorio temporal (lectura, top 20, porcentaje y cada guarda de R4), y terminar con código 0 sin root.
2. EL proyecto DEBERÁ incluir `scripts/pruebas_procesos.sh` (`sudo`, VM) que verifique R1–R4 contra el daemon real.
3. `pruebas_integracion.sh`, `pruebas_auth.sh`, `pruebas_alertas.sh` y `pruebas_metricas.sh` DEBERÁN seguir terminando con `0 FALLIDAS`.
4. `make -C ProgramaIntermedio clean all` sin warnings y `daemon_monitor` versionado.
