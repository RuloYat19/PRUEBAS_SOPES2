# F04 — Procesos (top 20, PID, kill) · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo` o la VM queda `- [ ]` con `Pendiente en VM:` y los comandos exactos.

---

- [x] **T1 — Correcciones pendientes de F03** · R6 · diseño §5
  - `metricas_tomar` vuelve a comprobar `metricas_activas()` bajo `lock_metricas`; fase 13 de `pruebas_metricas.sh` con un daemon de `intervalo = 5`.
  - Marcar las advertencias 1 y 2 como `Corregido:` en `specs/03-metricas/review.md`.
  - Verificación: `make -C ProgramaIntermedio pruebas` → `0`; `bash -n scripts/pruebas_metricas.sh`.
  - Evidencia:
    ```
    make -C ProgramaIntermedio pruebas → Todas las pruebas pasaron exitosamente (código 0)
    bash -n scripts/pruebas_metricas.sh → sintaxis correcta (código 0)
    ```

- [x] **T2 — `procesos.c`: escaneo, top 20 y guardas, con pruebas** · R1, R4.2–R4.5 · diseño §2, §3.1–3.3, §8.1
  - Mover el escaneo de `metricas.c` a `procesos.c` con `raiz` configurable; `rss_kb`; `procesos_top`, `procesos_porcentaje`, `procesos_validar_kill`.
  - `pruebas/test_procesos.c` con los casos 1–4 de §8.1 (árbol falso con `mkdtemp`, borrado al final).
  - Verificación: `make -C ProgramaIntermedio pruebas; echo $?` → `0`.
  - Evidencia:
    ```
    make -C ProgramaIntermedio pruebas; echo $?
    ✓ Caso 1: procesos_escanear lee nombre, estado y RSS; omite el 700; PID 2 tiene RSS 0
    ✓ Caso 2: procesos_top devuelve 20 ordenados por RSS desc y desempate por PID asc
    ✓ Caso 3: procesos_porcentaje calcula con precision y maneja mem_total 0
    ✓ Caso 4: guardas de kill validan los 7 casos con orden y mensajes esperados
    Todas las pruebas de procesos (Casos 1-4) pasaron exitosamente.
    0
    ```

- [x] **T3 — Toma, instantánea, estado activo y bucle** · R1.5, R3.3–R3.6 · diseño §3.4, §4
  - `procesos_tomar`, `procesos_instantanea`, `procesos_activos`, `procesos_cambiar_estado`; bucle con la espera sin límite cuando ambos están desactivados.
  - Caso 5 de §8.1.
  - Verificación: `make pruebas` → `0`; daemon sin root durante 5 s sin errores de `/proc` en su salida.
  - Evidencia:
    ```
    make -C ProgramaIntermedio pruebas → código 0 (todas las pruebas pasan, incluyendo Caso 5)
    daemon ejecutado sin root por 5 s:
    [2026-10-18 17:22:44] daemon_monitor PID 52104, puerto 8097, dashboard en .../Dashboard
    [2026-10-18 17:22:49] senal de paro recibida, cerrando
    [2026-10-18 17:22:49] daemon finalizado
    grep -i "proc" daemon.out → Sin errores de proc
    ```

- [x] **T4 — JSON y endpoints** · R2, R3.1, R3.2, R4 · diseño §6
  - `json_procesos`, `json_monitoreo` ampliado, filas de permiso, `atender_procesos`, `atender_monitoreo_procesos`, `atender_kill`, `difundir_procesos`.
  - Caso 6 de §8.1; `test_auth.c` con las 3 filas nuevas.
  - Verificación en host (sin root): `GET /api/procesos` y `POST /api/proceso/kill` sin cookie → `401`; `make pruebas` → `0`.
  - Evidencia:
    ```
    curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8097/api/procesos → 401
    curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" -d '{"pid":1234}' http://127.0.0.1:8097/api/proceso/kill → 401
    make -C ProgramaIntermedio pruebas → código 0 (test_auth con 13 filas de permiso y test_procesos con Caso 6)
    ```

- [x] **T5 — Dashboard** · R5 · diseño §7
  - Verificación en host: `grep -rn "innerHTML" Dashboard/js` → sin resultados; `grep -n "api/estado" Dashboard/js/*.js` → sin resultados; `grep -c "addEventListener('procesos'" Dashboard/js/app.js` → `1`; `grep -n "60000" Dashboard/js/app.js` → presente; `grep -n "Esta acción puede afectar la estabilidad del sistema" Dashboard/js/procesos.js` → presente.
  - Evidencia:
    ```
    grep -rn "innerHTML" Dashboard/js → sin resultados (código de salida 1)
    grep -n "api/estado" Dashboard/js/*.js → sin resultados (código de salida 1)
    grep -c "addEventListener('procesos'" Dashboard/js/app.js → 1
    grep -n "60000" Dashboard/js/app.js → 110: setInterval(() => api.sesion(), 60000);
    grep -n "Esta acción puede afectar la estabilidad del sistema" Dashboard/js/procesos.js → presente
    ```

- [x] **T6 — `pruebas_procesos.sh`** · R7.2 · diseño §8.2
  - Las 16 verificaciones, con `trap` que detiene el daemon, mata los procesos de prueba y borra usuarios.
  - Verificación en host: `bash -n`; sin root → código 1 y `Ejecuta con sudo`; `grep -n 'curl.*-d "\$(' scripts/pruebas_procesos.sh` → sin resultados.
  - Evidencia:
    ```
    bash -n scripts/pruebas_procesos.sh → código 0
    ./scripts/pruebas_procesos.sh (sin root) → código 1 y "Ejecuta con sudo"
    grep -n 'curl.*-d "\$(' scripts/pruebas_procesos.sh → sin resultados (código 1)
    ```

- [x] **T7 — Binario y README** · R7.4
  - README: sección "Procesos" (top 20, `/api/procesos`, `event: procesos`, `/api/monitoreo/procesos`, `/api/proceso/kill` y sus guardas, `sudo scripts/pruebas_procesos.sh`).
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`; `daemon_monitor` versionado.
  - Evidencia:
    ```
    make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning → 0
    make -C ProgramaIntermedio pruebas → código 0 (todos los tests pasaron)
    git ls-files -s ProgramaIntermedio/daemon_monitor → 100755 ... ProgramaIntermedio/daemon_monitor (versionado)
    README.md actualizado con sección 7 Procesos del sistema (Top 20 y Kill), endpoints y scripts
    ```

- [ ] **T8 — Validación en VM** · R1–R7
  - Pendiente en VM (requiere `sudo`):
    1. `make -C ProgramaIntermedio clean all pruebas` → código 0.
    2. `sudo scripts/pruebas_procesos.sh` → `0 FALLIDAS`.
    3. `sudo scripts/pruebas_metricas.sh` → `0 FALLIDAS` (con la fase 13 nueva).
    4. `sudo scripts/pruebas_alertas.sh`, `sudo scripts/pruebas_auth.sh` → `0 FALLIDAS`.
    5. `sudo scripts/pruebas_integracion.sh` → `0 FALLIDAS`, 38 aprobadas.
    6. Navegador:
       - el top 20 se actualiza solo y las tarjetas cambian;
       - como admin, **Terminar** pide confirmación y, al aceptar, el proceso desaparece y aparece la alerta ámbar;
       - terminar el PID 1 muestra el error;
       - el interruptor de procesos muestra el aviso;
       - como común no se ven botones ni interruptor;
       - con la pestaña abierta más de 30 minutos, la sesión sigue activa.
    7. `getent passwd | grep -c '^so2t_'` → `0`.
