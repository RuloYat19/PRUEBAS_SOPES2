# F02 — Motor de alertas y tiempo real · Tareas

Requisitos: [`requirements.md`](requirements.md) · Diseño: [`design.md`](design.md) · Reglas: [`../../AGENTS.md`](../../AGENTS.md)

Marca `- [x]` solo con evidencia real debajo (`Evidencia:`). Lo que requiere `sudo` o la VM queda `- [ ]` con `Pendiente en VM:` y los comandos exactos.

---

- [x] **T1 — `alertas.c`: catálogo, historial y emisión, con pruebas** · R1, R2 · diseño §2, §3, §8, §10.1
  - Tipos y prototipos en `comun.h`; `alertas.c` en `FUENTES`; objetivo `pruebas` con `test_alertas`.
  - `pruebas/test_alertas.c` con los 9 casos de §10.1 (el caso 8 se completa en T2).
  - Verificación: `make -C ProgramaIntermedio pruebas; echo $?` → `0`.
  - Evidencia: `make -C ProgramaIntermedio pruebas; echo $?` → Casos 1 a 9 ejecutados sin fallos (caso 8 preparado para T2), código 0.

- [x] **T2 — `json_alerta` y limpieza de `struct evento`** · R3.2, R3.4, R3.5 · diseño §4, §7
  - `json_alerta`; quitar `eventos_seguridad` de `json_estado`; eliminar `json_evento`, `struct evento`, `MAX_EVENTOS`, `estado.eventos` y `estado.total_eventos`.
  - Verificación: `grep -rn "json_evento\|struct evento\|eventos_seguridad\|total_eventos" ProgramaIntermedio/*.c ProgramaIntermedio/*.h` → sin resultados; `make pruebas` → `0` (incluye el caso 8).
  - Evidencia: `grep -rn "json_evento\|struct evento\|eventos_seguridad\|total_eventos" ProgramaIntermedio/comun.h ProgramaIntermedio/json.c` → sin resultados; `make -C ProgramaIntermedio pruebas; echo $?` → Casos 1 a 9 aprobados (incluye caso 8 con cJSON), código 0.

- [x] **T3 — Productor IPC** · R4.5 · diseño §6
  - `ipc.c` emite con `alerta_emitir_log`.
  - Verificación: compila sin warnings.
  - Evidencia: `gcc -Wall -Wextra -pthread -O2 -D_GNU_SOURCE -Ilib -c -o ProgramaIntermedio/ipc.o ProgramaIntermedio/ipc.c; echo "exit: $?"` → 0 warnings, exit: 0.

- [x] **T4 — HTTP: `/api/alertas`, SSE con nombre y alertas de login** · R3.1–R3.3, R4.1–R4.4, R4.6 · diseño §5
  - Fila de permiso, `atender_alertas`, `difundir_alertas` y las tres emisiones en `atender_login`.
  - Verificación en host (sin root): `GET /api/alertas` sin cookie → `401`; un login con clave incorrecta deja `ALERTA [MEDIUM] Inicio de sesión fallido` en la salida del daemon; un login con JSON inválido no deja ninguna línea `ALERTA`.
  - Evidencia: `curl localhost:8099/api/alertas` → 401; login clave incorrecta → `ALERTA [MEDIUM] Inicio de sesión fallido: Credenciales inválidas para el usuario nadie_prueba`; login JSON inválido → sin línea ALERTA (conteo total: 1).

- [x] **T5 — Dashboard: panel de alertas** · R5 · diseño §9
  - `index.html`, `js/alertas.js`, `js/app.js`, `css/styles.css`.
  - Verificación en host: `grep -n "innerHTML" Dashboard/js/alertas.js` → sin resultados; `grep -c "addEventListener('alerta'" Dashboard/js/app.js` → `1`; `grep -rn "eventos_seguridad\|onmessage" Dashboard/js` → sin resultados.
  - Evidencia: `grep -n "innerHTML" Dashboard/js/alertas.js` → sin resultados; `grep -c "addEventListener('alerta'" Dashboard/js/app.js` → 1; `grep -rn "eventos_seguridad\|onmessage" Dashboard/js` → sin resultados.

- [x] **T6 — `pruebas_alertas.sh`** · R6.2 · diseño §10.2
  - Las 12 verificaciones de §10.2, reutilizando `usuarios_prueba.sh`, con `trap` que borra los usuarios y detiene el daemon.
  - Verificación en host: `bash -n scripts/pruebas_alertas.sh`; sin root → código 1 y `Ejecuta con sudo`.
  - Evidencia: `bash -n scripts/pruebas_alertas.sh` → código 0; `./scripts/pruebas_alertas.sh` sin root → código 1 y `Ejecuta con sudo`.

- [x] **T7 — Adaptar `pruebas_integracion.sh`** · R6.3 · diseño §10.3
  - Verificación en host: `bash -n`; `grep -c "comprobar\|aprobar" scripts/pruebas_integracion.sh` igual que antes (anotar ambos); `grep -n "eventos_seguridad" scripts/` → sin resultados.
  - Evidencia: `bash -n scripts/pruebas_integracion.sh` → código 0; `grep -c "comprobar\|aprobar"` → 41 (antes: 41, `comprobar`: 33, `aprobar`: 5 + 3 definiciones/cabecera); `grep -rn "eventos_seguridad" scripts/` → sin resultados (código 1).

- [x] **T8 — Binario y README** · R6.5
  - README: sección breve "Alertas" (catálogo de severidades, `/api/alertas`, evento SSE `alerta`, `sudo scripts/pruebas_alertas.sh`).
  - Verificación: `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → `0`; `daemon_monitor` versionado.
  - Evidencia: README.md actualizado con sección 5 "Motor de alertas y tiempo real", catálogo, endpoints, SSE nombrado y scripts; `make -C ProgramaIntermedio clean all 2>&1 | grep -ci warning` → 0; `git ls-files -s ProgramaIntermedio/daemon_monitor` → rastreado y actualizado.

- [ ] **T9 — Validación en VM** · R1–R6
  - Pendiente en VM (requiere `sudo`):
    1. `make -C ProgramaIntermedio clean all pruebas` → código 0.
    2. `sudo scripts/pruebas_alertas.sh` → `0 FALLIDAS`.
    3. `sudo scripts/pruebas_auth.sh` → `0 FALLIDAS`.
    4. `sudo scripts/pruebas_integracion.sh` → `0 FALLIDAS`, 38 aprobadas.
    5. Navegador: un login fallido en otra pestaña aparece arriba en ámbar sin recargar; una línea con la palabra clave en el log aparece en ámbar con la palabra resaltada; el resumen `LOW · MEDIUM · HIGH` se actualiza.
    6. `getent passwd | grep -c '^so2t_'` → `0`.
