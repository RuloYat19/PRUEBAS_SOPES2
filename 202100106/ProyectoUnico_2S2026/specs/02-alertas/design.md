# F02 — Motor de alertas y tiempo real · Diseño

Requisitos: [`requirements.md`](requirements.md). Base: F00 (hilos, SSE) y F01 (sesiones, tabla de permisos).

---

## 1. Visión general

```text
hilo HTTP (login) ─┐
hilo ipc (EVT|…) ──┼─► alerta_emitir(motivo, …) ──► historial (200, lock_alertas)
F03–F07 (después) ─┘          │                            │
                              └─► eventfd_write(fd_aviso)  │
                                                           ▼
hilo HTTP: poll() despierta ─► difundir_alertas() ─► "event: alerta" a cada cliente SSE
GET /api/alertas ────────────► alertas_recientes() ─► {"alertas":[…]} (id desc)
```

| Pieza | Archivo | Requisito |
|---|---|---|
| Catálogo, historial, emisión | `ProgramaIntermedio/alertas.c` (nuevo) | R1, R2 |
| Serialización de alertas | `ProgramaIntermedio/json.c` | R3.2 |
| `/api/alertas`, difusión SSE con nombre | `ProgramaIntermedio/http.c`, fila nueva en `auth.c` | R3 |
| Productores | `http.c` (login), `ipc.c` | R4 |
| Limpieza de `struct evento` | `comun.h`, `json.c`, `ipc.c`, `http.c` | R3.4, R3.5 |
| Panel de alertas | `Dashboard/index.html`, `js/alertas.js` (nuevo), `js/app.js`, `css/styles.css` | R5 |
| Pruebas | `pruebas/test_alertas.c`, `scripts/pruebas_alertas.sh`, `scripts/pruebas_integracion.sh` | R6 |

## 2. Tipos y prototipos (`comun.h`)

```c
#define MAX_ALERTAS		200
#define MAX_DESCRIPCION		256

enum severidad { SEV_LOW, SEV_MEDIUM, SEV_HIGH };
enum tipo_alerta { TIPO_ARCHIVOS, TIPO_SISTEMA, TIPO_SESION, TIPO_LOGS };

enum motivo_alerta {
	MOT_ARCHIVO_MODIFICADO,
	MOT_MONITOREO_ACTIVADO,
	MOT_SESION_EXITOSA,
	MOT_MONITOREO_DESACTIVADO,
	MOT_SESION_FALLIDA,
	MOT_PALABRA_EN_LOG,
	MOT_BLACKLIST_ACTUALIZADA,
	MOT_PROCESO_MATADO,
	MOT_HASH_MALICIOSO,
	MOT_ARCHIVO_CUARENTENA,
	MOT_TOTAL
};

struct alerta {
	unsigned long id;
	enum motivo_alerta motivo;
	enum tipo_alerta tipo;
	enum severidad severidad;
	char descripcion[MAX_DESCRIPCION];
	char timestamp[32];
	char archivo[IPC_MSG_MAX_LEN];
	char palabra[64];
	char contenido[IPC_MSG_MAX_LEN];
};

const char *alerta_motivo_texto(enum motivo_alerta m);
enum tipo_alerta alerta_tipo(enum motivo_alerta m);
enum severidad alerta_severidad(enum motivo_alerta m);
const char *alerta_tipo_texto(enum tipo_alerta t);
const char *alerta_severidad_texto(enum severidad s);

unsigned long alerta_emitir(enum motivo_alerta m, const char *formato, ...)
	__attribute__((format(printf, 2, 3)));
unsigned long alerta_emitir_log(const char *palabra, const char *archivo,
				const char *contenido, time_t epoca);

size_t alertas_desde(unsigned long ultimo_id, struct alerta *destino, size_t max);
size_t alertas_recientes(struct alerta *destino, size_t max);
void alertas_reiniciar(void);

void json_alerta(struct buf *b, const struct alerta *a);
```

`alertas_reiniciar` vacía el historial y reinicia el contador; solo la usan las pruebas.

## 3. `alertas.c`

### 3.1 Catálogo (R1)

```c
struct entrada_catalogo { const char *texto; enum tipo_alerta tipo; enum severidad severidad; };

static const struct entrada_catalogo catalogo[MOT_TOTAL] = {
	[MOT_ARCHIVO_MODIFICADO]    = { "Archivo modificado",                 TIPO_ARCHIVOS, SEV_LOW },
	[MOT_MONITOREO_ACTIVADO]    = { "Monitoreo del sistema activado",     TIPO_SISTEMA,  SEV_LOW },
	[MOT_SESION_EXITOSA]        = { "Inicio de sesión exitoso",           TIPO_SESION,   SEV_LOW },
	[MOT_MONITOREO_DESACTIVADO] = { "Monitoreo del sistema desactivado",  TIPO_SISTEMA,  SEV_MEDIUM },
	[MOT_SESION_FALLIDA]        = { "Inicio de sesión fallido",           TIPO_SESION,   SEV_MEDIUM },
	[MOT_PALABRA_EN_LOG]        = { "Palabra clave detectada en log",     TIPO_LOGS,     SEV_MEDIUM },
	[MOT_BLACKLIST_ACTUALIZADA] = { "Actualización base de datos local",  TIPO_ARCHIVOS, SEV_MEDIUM },
	[MOT_PROCESO_MATADO]        = { "Proceso matado",                     TIPO_SISTEMA,  SEV_MEDIUM },
	[MOT_HASH_MALICIOSO]        = { "Hash malicioso detectado",           TIPO_ARCHIVOS, SEV_HIGH },
	[MOT_ARCHIVO_CUARENTENA]    = { "Archivo puesto en cuarentena",       TIPO_ARCHIVOS, SEV_HIGH },
};
```

- Textos de tipo: `archivos`, `sistema`, `sesion`, `logs`. Severidad: `LOW`, `MEDIUM`, `HIGH`.
- `alerta_emitir` con `m >= MOT_TOTAL` (o negativo) → `registrar("motivo de alerta invalido: %d")` y devuelve `0` sin emitir (R1.3).
- El archivo fuente es UTF-8; `json_cadena` copia los bytes ≥ 0x80 tal cual, así que las tildes llegan bien al navegador.

### 3.2 Historial (R2)

```c
static struct alerta historial[MAX_ALERTAS];
static unsigned long total_alertas;
static pthread_mutex_t lock_alertas = PTHREAD_MUTEX_INITIALIZER;
```

- Ranura de la alerta `id`: `historial[(id - 1) % MAX_ALERTAS]`. Alertas disponibles: de `max(1, total - 199)` a `total`.
- `insertar(const struct alerta *a)`: toma el mutex, asigna `id = ++total_alertas`, copia, suelta el mutex; **después** `if (fd_aviso >= 0) eventfd_write(fd_aviso, 1)` y `registrar("ALERTA [%s] %s: %s", …)`. Devuelve el `id`.
- `alerta_emitir`: arma la descripción con `vsnprintf(a.descripcion, MAX_DESCRIPCION, …)` (trunca solo, R2.4), con la hora actual (`localtime_r` + `strftime`).
- `alerta_emitir_log`: descripción `'%s' en %s: %s`, timestamp desde `epoca`, y copia `palabra`, `archivo` y `contenido` con `snprintf`.
- `alertas_desde(ultimo_id, destino, max)`: bajo el mutex copia en orden **ascendente** las alertas con `id > ultimo_id` que sigan en el historial, hasta `max`. Si `ultimo_id` quedó fuera de la ventana, empieza en la más antigua disponible.
- `alertas_recientes(destino, max)`: bajo el mutex copia en orden **descendente** (`total`, `total-1`, …).
- Nadie llama a `registrar()`, `eventfd_write()` ni a una syscall con `lock_alertas` tomado.

## 4. `json.c`

```c
void json_alerta(struct buf *b, const struct alerta *a)
{
	"id", "tipo", "motivo", "descripcion", "severidad", "timestamp"
	si a->tipo == TIPO_LOGS: + "archivo_origen", "palabra_detectada", "contenido"
}
```

Todas las cadenas pasan por `json_cadena`. `json_estado` deja de escribir `eventos_seguridad`; se elimina `json_evento`.

## 5. `http.c`

- **Permiso:** `{ "GET", "/api/alertas", ROL_COMUN }` en la tabla de `auth.c`.
- **`atender_alertas(c)`:** `malloc(MAX_ALERTAS * sizeof(struct alerta))` (≈ 180 KB, no en la pila), `n = alertas_recientes(…)`, serializa `{"alertas":[…]}` y libera.
- **`difundir_alertas()`** (reemplaza `difundir_eventos`): usa la variable estática `ultimo_difundido`. Pide `alertas_desde(ultimo_difundido, lote, 64)` en un `malloc` y arma por cada alerta `id: %lu\nevent: alerta\ndata: <json>\n\n`. Actualiza `ultimo_difundido` con el último `id` del lote y repite hasta que el lote vuelva vacío. Luego llama a `enviar_a_suscriptores()` como hoy.
- **Productores en `atender_login`** (R4.1–R4.4): junto a cada `registrar("Login …")` de F01:
  - 200 → `alerta_emitir(MOT_SESION_EXITOSA, "Usuario %s inició sesión como %s", usuario, rol_nombre(rol))`
  - 401 → `alerta_emitir(MOT_SESION_FALLIDA, "Credenciales inválidas para el usuario %s", usuario)`
  - 403 → `alerta_emitir(MOT_SESION_FALLIDA, "El usuario %s no pertenece a admin_user ni common_user", usuario)`
- Un cliente SSE nuevo **no** recibe el historial por SSE: lo pide con `/api/alertas` (R5.4). Así el comportamiento queda como en P4.

## 6. `ipc.c` (R4.5)

`interpretar()` deja de llenar `struct evento`: devuelve `palabra`, `archivo`, `contenido` y `epoca` en variables locales. `publicar()` se reemplaza por `alerta_emitir_log(palabra, archivo, contenido, epoca)`. Se elimina el `registrar("evento: …")`, porque `insertar()` ya registra.

## 7. Limpieza (R3.4, R3.5)

Se eliminan `MAX_EVENTOS`, `struct evento`, `estado.eventos`, `estado.total_eventos` y `json_evento`. `estado.lock` queda solo para métricas y procesos.

## 8. `Makefile`

- `alertas.c` en `FUENTES`.
- `pruebas`: compila y ejecuta `pruebas/test_alertas.c` enlazado con `alertas.o` y `json.o`. La prueba define `registrar()` (vacío), `fd_aviso = -1` y un `struct estado estado` en cero para satisfacer a `json.o`.

## 9. Dashboard

| Archivo | Cambio |
|---|---|
| `index.html` | `#panel-eventos` → `#panel-alertas`: título **Alertas**, `<span id="alertas-resumen">` en la cabecera, tabla con `<thead>` Fecha y hora · Severidad · Tipo · Motivo · Descripción y `<tbody id="alertas-cuerpo">`, `<p id="alertas-vacio">`. Carga `js/alertas.js` antes de `app.js`. |
| `js/alertas.js` | Nuevo. `const alertas = { cargarHistorial(), agregar(a, nueva), limpiar() }`. Inserta por `id` descendente, descarta duplicados con un `Set`, recorta a 200 filas y actualiza `#alertas-resumen`. La severidad va en `<span class="severidad sev-low|sev-medium|sev-high">LOW</span>`. En tipo `logs`, la descripción usa `resaltar()` sobre `contenido` con `palabra_detectada` (mover `resaltar` y `sinMarcaDeTiempo` de `app.js` aquí). Solo `textContent`/`createElement`. |
| `js/app.js` | Quitar `agregarEvento`, `idsEventos`, `historialPendiente` y el uso de `eventos_seguridad`. En `conectarEventos()`: `onopen` → `alertas.limpiar(); alertas.cargarHistorial()`; `flujo.addEventListener('alerta', (m) => alertas.agregar(JSON.parse(m.data), true))`. **`onmessage` no recibe eventos con nombre**: hay que usar `addEventListener`. |
| `css/styles.css` | `--sev-low: var(--serie-usada)`, `--sev-medium: var(--aviso)`, `--sev-high: var(--critico)`; clases `.severidad`, `.sev-*` (fondo tenue del color + texto del color) y un borde izquierdo de fila `tr.sev-high` para que las críticas destaquen. |

## 10. Pruebas

### 10.1 `pruebas/test_alertas.c` (sin root)

1. Catálogo: para los 10 motivos, `alerta_motivo_texto`, `alerta_tipo_texto(alerta_tipo())` y `alerta_severidad_texto(alerta_severidad())` coinciden con la tabla de R1.1.
2. Motivo inválido (`MOT_TOTAL`) → `alerta_emitir` devuelve 0 y `alertas_recientes` sigue vacío.
3. Tras `alertas_reiniciar`, tres emisiones devuelven los ids 1, 2 y 3.
4. `alertas_recientes` devuelve 3, 2, 1; `alertas_desde(1, …)` devuelve 2, 3.
5. Capacidad: 205 emisiones → `alertas_recientes` devuelve 200, el primero con id 205 y el último con id 6; `alertas_desde(0, …)` empieza en 6.
6. Descripción de 1000 caracteres → se guardan exactamente 255 bytes más el `\0`.
7. `alerta_emitir_log("ERROR", "/x/a.log", "linea \"con\" comillas", 0)`: tipo `logs`, `MEDIUM`, descripción `'ERROR' en /x/a.log: linea "con" comillas`.
8. `json_alerta` de esa alerta se puede parsear con cJSON, trae los 9 campos y conserva las comillas escapadas. Una alerta de sesión **no** trae `archivo_origen`.
9. Concurrencia: 4 hilos × 100 `alerta_emitir` → `total` = 400 y los 200 ids de `alertas_recientes` son únicos y consecutivos (400…201).

### 10.2 `scripts/pruebas_alertas.sh` (sudo, VM)

Reutiliza `usuarios_prueba.sh`. Usa el puerto `8097`, una carpeta propia en `~/so2_logs/pruebas_alertas` y una palabra clave propia en `log_palabras`.

| # | Verificación | Esperado |
|---|---|---|
| 1 | `GET /api/alertas` sin cookie | 401 |
| 2 | Login `so2t_comun` | `/api/alertas` 200 con `{"alertas":[…]}` |
| 3 | SSE abierto con la cookie del común; login de `so2t_admin` | llega `event: alerta` con `Inicio de sesión exitoso`, `LOW`, `sesion`, en < 1 s |
| 4 | Login con clave incorrecta | alerta `Inicio de sesión fallido` `MEDIUM` con `Credenciales inválidas para el usuario so2t_admin` |
| 5 | Login `so2t_nadie` | alerta `Inicio de sesión fallido` con `no pertenece a admin_user ni common_user` |
| 6 | Login con JSON inválido | **no** aparece una alerta nueva (el `id` máximo no cambia) |
| 7 | Línea con la palabra clave en el log vigilado | alerta `Palabra clave detectada en log`, `logs`, `MEDIUM`, con `archivo_origen`, `palabra_detectada` y `contenido` correctos; llega por SSE |
| 8 | `/api/alertas` | ids estrictamente descendentes |
| 9 | 210 logins fallidos | `/api/alertas` devuelve 200 alertas; el primer id es el máximo |
| 10 | `/api/estado` | ya no contiene `eventos_seguridad` |
| 11 | La salida del daemon contiene `ALERTA [MEDIUM]` y ninguna contraseña de prueba | ✓ |
| 12 | `SIGTERM` | código 0 |

### 10.3 `pruebas_integracion.sh`

- `eventos_sse()` → `grep -c '^data: .*"tipo":"logs"'`; las demás lecturas de `sse.txt` usan el mismo filtro.
- En la fase 2, quitar `eventos_seguridad` de `campos`.
- "`/api/estado` incluye el evento" → "`/api/alertas` incluye la alerta" (mismo `grep` de `palabra_detectada`).
- Mismo número de verificaciones que antes (38).

## 11. Decisiones

| Decisión | Alternativa descartada | Motivo |
|---|---|---|
| SSE con nombre (`event: alerta`) | Tramas sin nombre | F03 agregará `event: metricas` y F05/F06 `event: archivo` por el mismo canal, sin mezclar formatos. |
| Severidad solo desde el catálogo | El productor pasa la severidad | Una sola fuente de verdad para la tabla del enunciado; imposible equivocarse en otra feature. |
| Historial de 200 en memoria | Persistir en disco | El enunciado no pide persistencia; 200 cubre la demo y acota la memoria. |
| Login fallido genera alerta aunque venga de un anónimo | Limitar alertas por origen | Lo pide el enunciado. Limitación conocida: muchos intentos pueden desplazar alertas viejas del historial de 200; queda anotada para el Manual Técnico. |
