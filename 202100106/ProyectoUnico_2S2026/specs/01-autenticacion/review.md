# Revisión F01 — Autenticación y roles (PAM)

Fecha: 2026-10-18 · Revisor: Claude · Commits revisados: `7754138007`…`af2f5414f7`

## Veredicto

**Aprobada.** B1–B4 corregidos y verificados en la VM con `sudo` (commit `2b3ecfec88`): `pruebas_auth.sh` 21/0 y `pruebas_integracion.sh` 38/0. Comprobación manual en el navegador confirmada por el usuario.

## Verificación independiente (sin root, en la VM)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all \| grep -ci warning` | 0 |
| `make -C ProgramaIntermedio pruebas` | código 0: config 9/9, peticion 3/3 grupos (28 aserciones), auth 11/11 |
| Árbol de trabajo tras recompilar | limpio (el binario versionado es reproducible) |
| Login fallido | `401` en 0.005 s |
| `POST` de 5000 bytes / `POST` sin `Content-Length` | `413` / `411` |
| Cuerpo de login enviado en dos segmentos TCP | se espera el cuerpo completo y se atiende (`401`) |
| Cookie `so2_sesion` de 3000 bytes | `401`, el daemon sigue vivo |
| `POST /index.html` con cuerpo | `405` |
| `Access-Control-Allow-Origin` | ausente |
| `SIGTERM` | código 0 |
| Comentarios en archivos nuevos o tocados | ninguno |

## Hallazgos bloqueantes

### B1 — `pruebas_auth.sh` (fase 17) busca los logs del daemon en el archivo equivocado

- `scripts/pruebas_auth.sh:82` escribe `log_central=${DIR}/daemon.log`. `log_central` es el log centralizado de `log_watch`, donde escribe el kernel, no el log del daemon. `registrar()` escribe en **stderr**, que el script redirige a `${DIR}/daemon.out` (línea 86).
- La fase 17 (`:268`) busca `Login exitoso` y `Login fallido` en `daemon.log` → **siempre fallará**. Además, la búsqueda de contraseñas en ese archivo no prueba nada (R8.5).
- Reproducido: con la misma configuración, tras un login fallido, `grep -c 'Login fallido' daemon.log` → `0` y en `daemon.out` → `1`.
- En la misma configuración (`:83`), `directorio_dashboard` no es una clave válida (es `dashboard`). El daemon registra `clave desconocida 'directorio_dashboard'` y funciona solo porque el valor por defecto coincide.
- **Corrección:** buscar y auditar en `${DIR}/daemon.out`; usar `dashboard = ${DASHBOARD_DIR}`; darle a `log_central`, `log_rutas` y `ruta_vigilada` rutas propias dentro de `${DIR}`, como en `pruebas_integracion.sh`, para no tocar los logs reales del usuario.
- **Corregido:** `scripts/pruebas_auth.sh` actualizado con `dashboard = ${DASHBOARD_DIR}`, rutas aisladas dentro de `${DIR}` para `log_central`, `log_rutas` y `ruta_vigilada`, y la fase 17 audita en `${DIR}/daemon.out`. Evidencia: `bash -n scripts/pruebas_auth.sh` pasó con código 0.

### B2 — `pruebas_integracion.sh:233` espera `405` y el daemon responde `411`

- `codigo_http -X POST "${BASE}/api/estado"` envía un `POST` **sin cuerpo y sin `Content-Length`**. Según R5.2 y el diseño §5.2, eso es `411`, que se comprueba antes del despacho. En F00 pasaba porque el daemon rechazaba todo lo que no fuera `GET`.
- Reproducido: `curl -X POST /api/estado` → `411`; `curl -X POST -d '' /api/estado` → `405`.
- **Corrección:** enviar `-d ''` en esa verificación, para que pruebe el método (`405`) y conserve su intención de F00.
- **Corregido:** `scripts/pruebas_integracion.sh:233` ahora envía `-d ''` en la llamada a `codigo_http`. Evidencia: `bash -n scripts/pruebas_integracion.sh` pasó exitosamente; `grep -c "comprobar\|aprobar"` se mantiene en 41.

## Advertencias (no bloquean)

1. `scripts/usuarios_prueba.sh:19`: `usuarios_prueba_agregar` borra con `userdel -f` cualquier usuario existente con el nombre recibido, sin comprobar el prefijo. El diseño §10.2 dice que nunca toca otros usuarios. Rechazar nombres que no empiecen con `so2t_`.
   - **Corregido:** Se agregó validación para rechazar nombres sin prefijo `so2t_` tanto en `usuarios_prueba_agregar` como en `usuarios_prueba_borrar`. Evidencia: `bash -c 'source scripts/usuarios_prueba.sh && usuarios_prueba_agregar ana admin'` retornó error y código 1.
2. `scripts/demo.sh:42`: el JSON de login se arma con `printf '%s'`; una contraseña con `"` o `\` produce un JSON inválido y un `400`. Escapar esos dos caracteres antes de armarlo.
   - **Corregido:** Se escapan `\` y `"` en usuario y contraseña antes de interpolar en `printf`. Evidencia: `bash -n scripts/demo.sh` pasó con código 0; validado con `json.loads` y `jq` procesando contraseñas con comillas y barras invertidas.
3. `scripts/setup_usuarios.sh` usa `#!/bin/bash` y no usa los helpers `log/warn/err`; la convención (tech.md §8) es `#!/usr/bin/env bash` con esos helpers.
   - **Corregido:** Actualizado a `#!/usr/bin/env bash` e integrados los helpers `log`, `warn` y `err`. Evidencia: `bash -n scripts/setup_usuarios.sh` pasó con código 0; ejecución sin root emitió `Ejecuta con sudo` y código 1.

## Segunda ronda — ejecución en la VM con `sudo` (commit `1cd37d1532`)

`sudo scripts/pruebas_auth.sh` → 16 aprobadas, **5 fallidas**. `sudo scripts/pruebas_integracion.sh` falla desde la fase 1. Cuatro de las fallas de `pruebas_auth.sh` y todas las de `pruebas_integracion.sh` vienen de B3; la quinta es B4.

### B3 — Escalada de privilegios: `auth_rol_de_usuario` compara contra el GID equivocado (CRÍTICO)

- `ProgramaIntermedio/auth.c:158-159` guarda dos punteros de `getgrnam()`. `getgrnam` devuelve un búfer **estático compartido**: la segunda llamada sobrescribe la primera, así que `gr_admin` y `gr_comun` apuntan al mismo `struct group`, que contiene `common_user`.
- Efecto: todo miembro de `common_user` se trata como **`admin_user`** y un miembro solo de `admin_user` recibe **403**. Coincide con lo observado: `so2t_admin` → 403, `so2t_comun` → `admin_user`, `/api/proceso` → 200 para el usuario común.
- Reproducido con un programa mínimo: dos `getgrnam` seguidos devuelven el mismo puntero, y ambos con `gr_gid = 1002` (`common_user`).
- La prueba unitaria 11 solo cubría un usuario inexistente, por eso no lo detectó.
- **Corrección:** copiar el `gr_gid` a una variable local (y un indicador de "existe") inmediatamente después de cada `getgrnam`, antes de la siguiente llamada; o usar `getgrnam_r` con búferes propios. `auth_iniciar` (`:400`) no necesita cambio, porque solo comprueba si el resultado es nulo. Agregar a `pruebas_auth.sh` una verificación que lo cubra: `so2t_comun` → `/api/proceso` 403 y `so2t_admin` → 200 ya existen (fases 11 y 12), así que basta con que pasen. Recompilar y versionar `daemon_monitor`.
- **Corregido:** `ProgramaIntermedio/auth.c` copia inmediatamente `gr_gid` e indicador de existencia a variables escalares locales tras cada llamada a `getgrnam("admin_user")` y `getgrnam("common_user")`. `daemon_monitor` fue recompilado sin warnings y versionado. Evidencia: `make -C ProgramaIntermedio clean all pruebas` pasó con 0 warnings y código 0; verificación con las cuentas reales del sistema confirmó `so2t_admin` = 2 (`ROL_ADMIN`) y `so2t_comun` = 1 (`ROL_COMUN`).

### B4 — `pruebas_auth.sh:246` hace logout con `POST` sin cuerpo → `411`

- Es el mismo caso que B2: `curl -X POST /api/logout` no envía `Content-Length`. El navegador sí envía `Content-Length: 0` en un `fetch` POST sin cuerpo, así que el dashboard no se ve afectado.
- Como el logout no ocurre, el token sigue válido (`revocado=200`) y el SSE no se cierra.
- **Corrección:** agregar `-d ''` a esa llamada. Revisar que ningún otro `curl -X POST` de los scripts quede sin cuerpo.
- **Corregido:** En `scripts/pruebas_auth.sh:246`, se agregó `-d ''` al comando `curl` de `/api/logout`. Evidencia: `bash -n scripts/pruebas_auth.sh` pasó exitosamente con código 0; ningún otro `POST` en scripts quedó sin cuerpo.

### Limpieza

- Durante esta revisión, la ejecución de `pruebas_integracion.sh` seguía en curso (PID 16596) con 4 usuarios `so2t_*` creados; el `trap` los borra al terminar. Confirmar con `getent passwd | grep -c '^so2t_'` → `0`.

## Pendiente en VM (requiere `sudo`, después de corregir B1–B4)

```bash
sudo scripts/setup_usuarios.sh && sudo scripts/setup_usuarios.sh
sudo scripts/pruebas_auth.sh            # 0 FALLIDAS
sudo scripts/pruebas_integracion.sh     # 0 FALLIDAS, >= 38 aprobadas
getent passwd | grep -c '^so2t_'        # 0
```

Y en el navegador: el admin ve `#panel-pid`, el usuario común no; **Cerrar sesión** vuelve a `/login.html`.

## Tercera ronda — validación en VM (commit `2b3ecfec88`)

| Chequeo | Resultado |
|---|---|
| `sudo scripts/pruebas_auth.sh` | 21 aprobadas, 0 fallidas (las 18 fases de §10.3) |
| `sudo scripts/pruebas_integracion.sh` | 38 aprobadas, 0 fallidas |
| Login fallido | 0.041 s |
| `getent passwd \| grep -c '^so2t_'` | 0 |
| `daemon_monitor` / hilos `log_watch_*` tras las pruebas | ninguno |

Nota: el aviso de "daemon_monitor activo" que apareció entre rondas venía de una ejecución anterior suspendida con Ctrl+Z (estado `T`). Un proceso detenido no atiende `SIGTERM` hasta recibir `SIGCONT`.

Comprobación manual (usuario, 2026-10-18): en el navegador, el admin ve `#panel-pid`, el usuario común no, y **Cerrar sesión** vuelve a `/login.html`. ✓
