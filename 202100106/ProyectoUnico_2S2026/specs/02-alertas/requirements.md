# F02 — Motor de alertas y tiempo real · Requisitos

## Introducción

Reemplazar los "eventos de seguridad" de P4 (solo `log_watch`) por un **motor de alertas** único en el daemon. Cada alerta tiene tipo, motivo, descripción, severidad y fecha/hora, como piden el enunciado (§2.2.3.8) y el panel de alertas del dashboard (§2.2.4.2). La severidad la decide el daemon en C según una tabla fija.

Esta feature deja el motor y el canal en tiempo real listos. Además, conecta los productores que ya existen:

| Productor | Motivo | Severidad |
|---|---|---|
| Login exitoso (F01) | Inicio de sesión exitoso | `LOW` |
| Login rechazado: credenciales o sin grupo (F01) | Inicio de sesión fallido | `MEDIUM` |
| `log_watch` por el canal IPC (F00) | Palabra clave detectada en log | `MEDIUM` |

Fuera de alcance: los demás motivos del catálogo (monitoreo activado/desactivado, blacklist, proceso matado, archivo modificado, hash malicioso, cuarentena). Los emiten F03–F07 llamando a la función de este motor; F02 solo los declara en el catálogo.

## Glosario

- **Alerta:** registro inmutable con `id` creciente, `tipo`, `motivo`, `descripcion`, `severidad` y `timestamp`.
- **Catálogo:** tabla en C que asocia cada motivo con su tipo y su severidad.
- **Historial:** las últimas 200 alertas en memoria.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Catálogo de alertas

**Historia:** Como evaluador, quiero que la severidad de cada evento sea exactamente la del enunciado y la decida el daemon.

1. EL daemon DEBERÁ declarar en **una sola tabla** en C estos 10 motivos, con estos textos exactos, tipos y severidades:

   | Motivo (texto exacto) | Tipo | Severidad |
   |---|---|---|
   | `Archivo modificado` | `archivos` | `LOW` |
   | `Monitoreo del sistema activado` | `sistema` | `LOW` |
   | `Inicio de sesión exitoso` | `sesion` | `LOW` |
   | `Monitoreo del sistema desactivado` | `sistema` | `MEDIUM` |
   | `Inicio de sesión fallido` | `sesion` | `MEDIUM` |
   | `Palabra clave detectada en log` | `logs` | `MEDIUM` |
   | `Actualización base de datos local` | `archivos` | `MEDIUM` |
   | `Proceso matado` | `sistema` | `MEDIUM` |
   | `Hash malicioso detectado` | `archivos` | `HIGH` |
   | `Archivo puesto en cuarentena` | `archivos` | `HIGH` |

2. Quien emite una alerta DEBERÁ indicar solo el motivo y la descripción; tipo y severidad DEBERÁN salir del catálogo.
3. Ningún productor DEBERÁ poder emitir una alerta con un motivo fuera del catálogo.

## R2 — Historial en memoria

**Historia:** Como usuario, quiero ver las alertas recientes aunque abra el dashboard después de que ocurrieron.

1. EL daemon DEBERÁ guardar las últimas **200** alertas; CUANDO llegue la 201, DEBERÁ descartar la más antigua.
2. Cada alerta DEBERÁ recibir un `id` entero que empieza en 1 y crece de uno en uno, sin repetirse mientras el daemon corre.
3. `timestamp` DEBERÁ tener formato `YYYY-MM-DD HH:MM:SS` en hora local. Para las alertas de `log_watch`, DEBERÁ usar la época que trae el mensaje IPC; para las demás, la hora de emisión.
4. `descripcion` DEBERÁ truncarse a 255 bytes si es más larga, sin desbordar ni cortar el daemon.
5. El historial DEBERÁ protegerse con su propio mutex. Emitir una alerta desde cualquier hilo (HTTP, `monitoreo`, `archivos`, `ipc`) DEBERÁ ser seguro.
6. EL daemon DEBERÁ registrar cada alerta con `registrar()`: `ALERTA [<severidad>] <motivo>: <descripcion>`.

## R3 — API

**Historia:** Como usuario, quiero consultar el historial y recibir las alertas nuevas sin recargar.

1. `GET /api/alertas` (rol `common_user`) DEBERÁ responder `200` con `{"alertas":[…]}`: hasta 200 alertas, **más recientes primero** (`id` descendente).
2. Cada alerta DEBERÁ serializarse así:

   ```json
   {"id":7,"tipo":"sesion","motivo":"Inicio de sesión exitoso",
    "descripcion":"Usuario ana inició sesión como admin_user",
    "severidad":"LOW","timestamp":"2026-10-18 14:00:00"}
   ```

   Las alertas de tipo `logs` DEBERÁN agregar `archivo_origen`, `palabra_detectada` y `contenido` (los mismos nombres de P4).
3. CUANDO se emita una alerta, EL daemon DEBERÁ enviarla a todos los clientes de `GET /api/eventos` como una trama SSE con nombre:

   ```text
   id: <id>
   event: alerta
   data: <json de la alerta>
   ```

   en menos de **1 s**, sin polling.
4. `GET /api/estado` DEBERÁ dejar de incluir `eventos_seguridad`; el resto de su contrato no cambia.
5. Se eliminan `struct evento`, `json_evento` y el arreglo de eventos de `struct estado`.

## R4 — Productores de esta feature

1. CUANDO un login responda `200`, EL daemon DEBERÁ emitir `Inicio de sesión exitoso` con la descripción `Usuario <usuario> inició sesión como <rol>`.
2. CUANDO un login responda `401`, EL daemon DEBERÁ emitir `Inicio de sesión fallido` con `Credenciales inválidas para el usuario <usuario>`.
3. CUANDO un login responda `403`, EL daemon DEBERÁ emitir `Inicio de sesión fallido` con `El usuario <usuario> no pertenece a admin_user ni common_user`.
4. Un login rechazado con `400` (petición mal formada) NO DEBERÁ generar alerta.
5. CUANDO el hilo `ipc` reciba un mensaje `EVT|…` válido, EL daemon DEBERÁ emitir `Palabra clave detectada en log` con la descripción `'<palabra>' en <archivo>: <linea>` y los campos `archivo_origen`, `palabra_detectada` y `contenido`.
6. Ninguna descripción DEBERÁ contener contraseñas.

## R5 — Dashboard

**Historia:** Como usuario, quiero un panel de alertas ordenado y con colores por severidad.

1. El panel **Eventos de seguridad** DEBERÁ reemplazarse por **Alertas**, con columnas: fecha y hora, severidad, tipo, motivo y descripción.
2. Las alertas DEBERÁN mostrarse más recientes primero.
3. La severidad DEBERÁ distinguirse por color con las variables de `styles.css`: `LOW` azul (informativo), `MEDIUM` ámbar (advertencia), `HIGH` rojo (crítico). El texto de la severidad también DEBERÁ estar visible, no solo el color.
4. CUANDO se abra o reabra el SSE, el dashboard DEBERÁ cargar el historial con `GET /api/alertas` y descartar los `id` repetidos.
5. Las alertas nuevas DEBERÁN aparecer arriba sin recargar la página, escuchando el evento SSE `alerta`.
6. En las alertas de tipo `logs`, la descripción DEBERÁ resaltar la palabra detectada, como en P4.
7. El encabezado del panel DEBERÁ mostrar el total de alertas por severidad (`LOW n · MEDIUM n · HIGH n`) de las que hay en pantalla.
8. El panel DEBERÁ mostrar como máximo 200 filas.
9. El dashboard NUNCA DEBERÁ insertar texto de una alerta como HTML (`textContent`, no `innerHTML`).

## R6 — Pruebas y paridad

1. `make -C ProgramaIntermedio pruebas` DEBERÁ incluir `pruebas/test_alertas.c` (catálogo, ids, capacidad, orden, truncado, JSON y emisión concurrente) y terminar con código 0, sin root.
2. EL proyecto DEBERÁ incluir `scripts/pruebas_alertas.sh` (`sudo`, VM) que verifique R3 y R4 contra el daemon real.
3. `scripts/pruebas_integracion.sh` DEBERÁ adaptarse al nuevo formato (contar solo las tramas de alertas `logs` y consultar `/api/alertas` en lugar de `eventos_seguridad`) y seguir terminando con `0 FALLIDAS`, sin perder verificaciones.
4. `scripts/pruebas_auth.sh` DEBERÁ seguir terminando con `0 FALLIDAS`.
5. `make -C ProgramaIntermedio clean all` sin warnings y `daemon_monitor` versionado.
