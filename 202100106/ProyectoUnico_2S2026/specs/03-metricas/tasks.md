# F03 — Métricas del sistema · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo` o la VM queda `- [ ]` con `Pendiente en VM:` y los comandos exactos.

---

- [x] **T1 — Lectura, cálculo e historial con pruebas** · R1, R2.1 · diseño §2, §3.1–3.3, §7.1
  - `struct muestra` y prototipos; `metricas_leer_meminfo`, `metricas_leer_vmstat`, `metricas_calcular`, `metricas_tomar`, `metricas_desde`, `metricas_historial`, `metricas_reiniciar`.
  - `pruebas/test_metricas.c` con los casos 1–8 y 10 de §7.1; objetivo `pruebas` y `.gitignore` del binario de prueba.
  - Verificación: `make -C ProgramaIntermedio pruebas; echo $?` → `0`.
  - Evidencia:
    ```
    make -C ProgramaIntermedio pruebas; echo $?
    ...
    ./pruebas/test_metricas
    ✓ Caso 1: meminfo con 9 claves y extras
    ✓ Caso 2: meminfo sin Active devuelve -1
    ✓ Caso 3: vmstat exacto y sin distractor
    ✓ Caso 4: metricas_calcular primera muestra
    ✓ Caso 5: metricas_calcular deltas con anterior
    ✓ Caso 6: contador menor que anterior produce delta 0
    ✓ Caso 7: derivados suman mem_total y saturacion usada == 0
    ✓ Caso 8: metricas_tomar dos veces con deltas correctos
    ✓ Caso 10: historial de 155 muestras devuelve 150 (ids 6..155) y metricas_desde
    Todas las pruebas de metricas pasaron exitosamente.
    make: se sale del directorio '/home/sudockentucky/Documentos/mapa/ProyectoUnico_2S2026/ProgramaIntermedio'
    0
    ```

- [x] **T2 — Estado activo y bucle del hilo** · R3.5, R3.6, R3.9, R3.10 · diseño §3.4
  - `metricas_activas`, `metricas_cambiar_estado`, `pendiente_muestra`; `recolectar()` separado en `metricas_tomar` + `recolectar_procesos`.
  - Caso 9 de §7.1.
  - Verificación: `make pruebas` → `0`; en host, arrancar el daemon sin root durante 5 s y comprobar que su salida no tiene errores de `/proc/meminfo` ni de `/proc/vmstat`.
  - Evidencia:
    ```
    make -C ProgramaIntermedio pruebas; echo $?
    ...
    ✓ Caso 9: metricas_cambiar_estado transiciones y primera == 1 al reactivar
    Todas las pruebas de metricas pasaron exitosamente.
    0

    timeout -s SIGTERM 5s ./ProgramaIntermedio/daemon_monitor -c "$TMPDIR/test.conf" 2>&1
    [2026-10-18 16:21:07] La autenticacion PAM requiere ejecutar el daemon como root
    [2026-10-18 16:21:07] vigilando 'ERROR' en 2 archivos, watch_id 29
    [2026-10-18 16:21:07] vigilando 'FAIL' en 2 archivos, watch_id 30
    [2026-10-18 16:21:07] daemon_monitor PID 41727, puerto 8097, dashboard en .../Dashboard
    [2026-10-18 16:21:12] senal de paro recibida, cerrando
    [2026-10-18 16:21:12] watch_id 29 detenido
    [2026-10-18 16:21:12] watch_id 30 detenido
    [2026-10-18 16:21:12] daemon finalizado
    Sin errores de /proc/meminfo ni /proc/vmstat en la salida.
    ```

- [x] **T3 — `json_muestra` y endpoints** · R2.2–R2.4, R3.1–R3.4, R3.7, R3.8 · diseño §4, §5
  - `json_muestra`, `json_monitoreo`, filas de permiso, `atender_metricas`, `atender_monitoreo`, `atender_monitoreo_metricas`, `difundir_metricas`; firma de `servidor_bucle` con el intervalo.
  - Caso 11 de §7.1; caso nuevo en `test_auth.c` para las 3 filas de permiso.
  - Verificación en host (sin root): `GET /api/metricas` y `GET /api/monitoreo` sin cookie → `401`; `POST /api/monitoreo/metricas` sin cookie → `401`; `make pruebas` → `0`.
  - Evidencia:
    ```
    GET /api/metricas: 401
    GET /api/monitoreo: 401
    POST /api/monitoreo/metricas: 401
    make -C ProgramaIntermedio pruebas; echo $?
    ...
    ✓ Caso 10: permiso_buscar valida 10 filas, metodo no permitido (-2) y ruta no encontrada (-1)
    ✓ Caso 11: json_muestra parsea con cJSON y primera es booleano
    Todas las pruebas de metricas pasaron exitosamente.
    0
    ```

- [x] **T4 — Dashboard: 4 gráficas e interruptor** · R4 · diseño §6
  - Verificación en host: `grep -n "innerHTML" Dashboard/js/metricas.js` → sin resultados; `grep -c "addEventListener('metricas'" Dashboard/js/app.js` → `1`; `grep -c "addEventListener('monitoreo'" Dashboard/js/app.js` → `1`; `grep -c 'id="interruptor-metricas"' Dashboard/index.html` → `1` y está dentro de un elemento con `data-rol="admin_user"`.
  - Evidencia:
    ```
    grep -n "innerHTML" Dashboard/js/metricas.js → sin resultados (código de retorno 1)
    grep -c "addEventListener('metricas'" Dashboard/js/app.js → 1
    grep -c "addEventListener('monitoreo'" Dashboard/js/app.js → 1
    grep -c 'id="interruptor-metricas"' Dashboard/index.html → 1
    43: <label data-rol="admin_user"><input type="checkbox" id="interruptor-metricas" checked> Métricas del sistema</label>
    ```

- [x] **T5 — `pruebas_metricas.sh`** · R5.2 · diseño §7.2
  - Las 15 verificaciones, con `trap` que detiene el daemon, mata el `sleep 30` y borra usuarios.
  - Verificación en host: `bash -n`; sin root → código 1 y `Ejecuta con sudo`; `grep -n 'curl.*-d "\$(' scripts/pruebas_metricas.sh` → sin resultados (logins por stdin).
  - Evidencia:
    ```
    bash -n scripts/pruebas_metricas.sh && echo "bash -n: OK"
    bash -n: OK
    ./scripts/pruebas_metricas.sh; echo "Codigo: $?"
    Ejecuta con sudo
    Codigo: 1
    grep -n 'curl.*-d "\$' scripts/pruebas_metricas.sh → sin resultados
    ```

- [x] **T6 — Binario y README** · R5.4
  - README: sección "Métricas del sistema" (claves leídas, deltas, `/api/metricas`, `event: metricas`, `/api/monitoreo`, `sudo scripts/pruebas_metricas.sh`).
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`; `daemon_monitor` versionado.
  - Evidencia:
    ```
    make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning → 0
    git ls-files -s ProgramaIntermedio/daemon_monitor → 100755 9d98f77cf28c65f3c86685a5c22eade482679300 0 ProgramaIntermedio/daemon_monitor
    make -C ProgramaIntermedio pruebas → Todas las pruebas pasaron exitosamente (test_config, test_peticion, test_auth, test_alertas, test_metricas)
    ```

- [ ] **T7 — Validación en VM** · R1–R5
  - Pendiente en VM (requiere `sudo`):
    1. `make -C ProgramaIntermedio clean all pruebas` → código 0.
    2. `sudo scripts/pruebas_metricas.sh` → `0 FALLIDAS`.
    3. `sudo scripts/pruebas_alertas.sh`, `sudo scripts/pruebas_auth.sh` → `0 FALLIDAS`.
    4. `sudo scripts/pruebas_integracion.sh` → `0 FALLIDAS`, 38 aprobadas (incluye 0 tics del hilo `ipc`).
    5. Navegador: las 4 gráficas se mueven cada intervalo; como admin, apagar el interruptor muestra el aviso en las 4 y una alerta ámbar; encenderlo genera una alerta azul y las gráficas siguen; como común no se ve el interruptor y sí el estado en texto.
    6. `getent passwd | grep -c '^so2t_'` → `0`.
