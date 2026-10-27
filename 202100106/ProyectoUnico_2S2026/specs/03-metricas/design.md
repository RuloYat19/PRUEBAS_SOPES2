# F03 — Métricas del sistema · Diseño

Requisitos: [`requirements.md`](requirements.md). Base: F00 (hilo `monitoreo`), F01 (permisos), F02 (alertas, SSE con nombre).

---

## 1. Visión general

```text
hilo monitoreo, cada `intervalo` s:
    si metricas activas: metricas_tomar("/proc/meminfo", "/proc/vmstat")
        └─► leer → calcular (deltas, derivados) → historial[150] (lock_metricas)
            └─► actualiza estado.memoria_* de P4 (estado.lock) → eventfd_write(fd_aviso)
    siempre: resumen de procesos + listado de P4 (sin cambios; F04 lo reemplaza)

hilo HTTP:
    fd_aviso ─► difundir_alertas() + difundir_metricas()  ──► "event: metricas"
    POST /api/monitoreo/metricas ─► metricas_cambiar_estado() ─► alerta + "event: monitoreo"
    GET  /api/metricas, GET /api/monitoreo
```

| Pieza | Archivo | Requisito |
|---|---|---|
| Lectura, cálculo, historial, estado activo | `ProgramaIntermedio/metricas.c` | R1, R2.1, R3.5, R3.6 |
| `json_muestra` | `ProgramaIntermedio/json.c` | R2.2, R2.3 |
| Endpoints y difusión | `ProgramaIntermedio/http.c`, filas en `auth.c` | R2, R3 |
| Gráficas e interruptor | `Dashboard/index.html`, `js/metricas.js` (nuevo), `js/app.js`, `css/styles.css` | R4 |
| Pruebas | `pruebas/test_metricas.c`, `scripts/pruebas_metricas.sh` | R5 |

## 2. Tipos y prototipos (`comun.h`)

```c
#define MAX_MUESTRAS		150

struct muestra {
	unsigned long id;
	char timestamp[32];
	int primera;
	unsigned long mem_total, mem_libre, mem_disponible, buffers, cached;
	unsigned long activas, inactivas, swap_total, swap_libre;
	unsigned long usada, cache, libre, swap_usada;
	unsigned long long pgfault, pgmajfault;
	unsigned long long pgfault_delta, pgmajfault_delta;
};

int metricas_leer_meminfo(const char *ruta, struct muestra *m);
int metricas_leer_vmstat(const char *ruta, struct muestra *m);
void metricas_calcular(struct muestra *m, const struct muestra *anterior);
unsigned long metricas_tomar(const char *ruta_meminfo, const char *ruta_vmstat);

size_t metricas_desde(unsigned long ultimo_id, struct muestra *destino, size_t max);
size_t metricas_historial(struct muestra *destino, size_t max);
void metricas_reiniciar(void);

int metricas_activas(void);
int metricas_cambiar_estado(int activo);

void json_muestra(struct buf *b, const struct muestra *m);
```

## 3. `metricas.c`

### 3.1 Lectura (R1.1, R1.2, R1.7)

- `metricas_leer_meminfo(ruta, m)`: misma técnica que P4 (`fgets` + `sscanf("%63[^:]: %lu")`), ahora con las 9 claves. Lleva una máscara de bits con las claves encontradas; si falta alguna → `-1`.
- `metricas_leer_vmstat(ruta, m)`: líneas `clave valor`; busca `pgfault` y `pgmajfault` (comparación exacta, para no tomar `pgfault_*` de otras versiones); si falta alguna → `-1`.
- Las dos funciones reciben la ruta para poder probarlas con archivos de ejemplo.

### 3.2 Cálculo (R1.3–R1.6)

```c
void metricas_calcular(struct muestra *m, const struct muestra *anterior)
{
	m->cache = m->buffers + m->cached;
	m->libre = m->mem_libre;
	m->usada = m->mem_total > m->libre + m->cache ? m->mem_total - m->libre - m->cache : 0;
	m->swap_usada = m->swap_total > m->swap_libre ? m->swap_total - m->swap_libre : 0;
	m->primera = anterior == NULL;
	m->pgfault_delta    = anterior && m->pgfault    >= anterior->pgfault    ? m->pgfault    - anterior->pgfault    : 0;
	m->pgmajfault_delta = anterior && m->pgmajfault >= anterior->pgmajfault ? m->pgmajfault - anterior->pgmajfault : 0;
}
```

Si `usada` llega al piso de 0, `libre + cache` puede pasar de `mem_total`. Ese caso no ocurre con un `/proc/meminfo` real; la prueba unitaria lo cubre igual.

### 3.3 Toma de muestra e historial (R2.1)

```c
static struct muestra historial[MAX_MUESTRAS];
static unsigned long total_muestras;
static struct muestra anterior;
static int hay_anterior;
static pthread_mutex_t lock_metricas = PTHREAD_MUTEX_INITIALIZER;
```

`metricas_tomar(ruta_meminfo, ruta_vmstat)`:
1. Leer ambos archivos; si falla → `registrar(...)` y `return 0` (la muestra se descarta, R1.7).
2. `timestamp` con `localtime_r` + `strftime`.
3. Bajo `lock_metricas`: `metricas_calcular(&m, hay_anterior ? &anterior : NULL)`; `m.id = ++total_muestras`; guardarla en `historial[(id-1) % MAX_MUESTRAS]`; `anterior = m; hay_anterior = 1`.
4. Fuera del mutex: actualizar `estado.memoria_usada/libre/cache` y `estado.swap_usada` con las definiciones de P4 (`MemTotal − MemAvailable`, `MemAvailable`, `Cached + Buffers`, `SwapTotal − SwapFree`) bajo `estado.lock`, para que `/api/estado` no cambie (R2.4).
5. `if (fd_aviso >= 0) eventfd_write(fd_aviso, 1)`; devolver el `id`.

`metricas_desde` y `metricas_historial` siguen el mismo patrón que `alertas_desde`, ambos en orden **ascendente** (R2.2).

### 3.4 Estado activo (R3.5, R3.6, R3.10)

```c
static int activas = 1;
static int pendiente_muestra;
```

- `metricas_activas()`: `__atomic_load_n(&activas, __ATOMIC_SEQ_CST)`.
- `metricas_cambiar_estado(activo)`: con `__atomic_exchange_n` obtiene el valor previo; si es igual al pedido → `0`. Si cambió:
  - al activar: bajo `lock_metricas`, `hay_anterior = 0` (período nuevo, R1.4); bajo `lock_espera`, `pendiente_muestra = 1` y `pthread_cond_signal(&cond_espera)`.
  - devuelve `1`.
- Bucle de `hilo_monitoreo`:

  ```c
  while (ejecutando) {
      pendiente_muestra = 0;
      unlock(lock_espera);
      if (metricas_activas())
          metricas_tomar("/proc/meminfo", "/proc/vmstat");
      recolectar_procesos();
      lock(lock_espera);
      limite = ahora + intervalo;
      while (ejecutando && !pendiente_muestra &&
             pthread_cond_timedwait(&cond_espera, &lock_espera, &limite) != ETIMEDOUT)
          ;
  }
  ```

  Hoy el bucle interno solo sale por tiempo o porque `ejecutando` pasó a 0: un `signal` vuelve a esperar. `pendiente_muestra` hace que la reactivación tome la muestra al instante (R3.6).
- `recolectar_procesos()` es el `recolectar()` de P4 sin la parte de memoria: `sys_get_processes_state` + escaneo de `/proc`. Se ejecuta aunque las métricas estén desactivadas (R3.9).

## 4. `json.c`

`json_muestra(b, m)`: objeto con los 19 campos de R2.2 en ese orden. `primera` como `true`/`false` y los `unsigned long long` con `%llu`.

## 5. `http.c` y `auth.c`

Filas nuevas en la tabla de permisos:

```c
{ "GET",  "/api/metricas",          ROL_COMUN },
{ "GET",  "/api/monitoreo",         ROL_COMUN },
{ "POST", "/api/monitoreo/metricas", ROL_ADMIN },
```

- **`atender_metricas`**: `malloc(MAX_MUESTRAS * sizeof(struct muestra))`, `metricas_historial`, responde `{"activo":…,"intervalo_s":…,"muestras":[…]}`. `intervalo_s` sale de la configuración: `servidor_bucle` recibe un `const struct configuracion *` o una copia del intervalo (cambiar la firma en `comun.h` y `main.c`).
- **`atender_monitoreo`**: `{"metricas":true|false}`, escrito por una función `json_monitoreo(struct buf *)` que F04/F05 ampliarán.
- **`atender_monitoreo_metricas(c, usuario)`**: `cJSON_ParseWithLength`; `cJSON_IsBool(activo)` o 400. `cambio = metricas_cambiar_estado(cJSON_IsTrue(activo))`. Si `cambio`: `alerta_emitir(MOT_MONITOREO_DESACTIVADO, "Métricas del sistema desactivadas por %s", usuario)` o `alerta_emitir(MOT_MONITOREO_ACTIVADO, "Métricas del sistema activadas por %s", usuario)`, y difundir `event: monitoreo` con `json_monitoreo` directamente con `enviar_a_suscriptores` (estamos en el hilo HTTP). Responde `{"metricas":…,"cambio":…}`.
- **`difundir_metricas()`**: igual a `difundir_alertas()` con su propio `ultimo_metrica_difundida`, lote de 32, trama `id: %lu\nevent: metricas\ndata: <json>\n\n`. Se llama al despertar por `fd_aviso`, junto a `difundir_alertas()`.
- Ojo con `id:` en SSE: alertas y métricas tienen contadores independientes. El navegador guarda el último `id` para `Last-Event-ID`, pero el daemon no lo usa, así que no hay conflicto. El dashboard deduplica por tipo de evento.

## 6. Dashboard

| Archivo | Cambio |
|---|---|
| `index.html` | Panel **Métricas del sistema** con cabecera: `<label data-rol="admin_user"><input type="checkbox" id="interruptor-metricas"> Métricas del sistema</label>` y `<span id="estado-metricas">`. Cuatro paneles: `#panel-paginas` (`#grafico-paginas`, leyenda), `#panel-evolucion` (existente), `#panel-desglose` (dona existente, ahora con libre/caché/utilizada), `#panel-fallos` (`#grafico-pgfault` y `#grafico-pgmajfault` apilados, leyendas). Un `<p class="aviso-desactivado">` por panel, oculto por defecto. Carga `js/metricas.js` antes de `app.js`. |
| `js/metricas.js` | Nuevo. `const metricas = { cargarHistorial(), agregar(m), aplicarEstado(activo), conectarInterruptor() }`. Guarda hasta 150 muestras por `id` (descarta repetidos). `agregar` actualiza las series y repinta con `Graficos.lineas` / `Graficos.dona` / `Graficos.leyenda`, que ya existen. `aplicarEstado` marca el interruptor, escribe `Métricas activas`/`Métricas desactivadas` y muestra u oculta los avisos. El interruptor llama a `api.post('/api/monitoreo/metricas', {activo})`; si falla, vuelve a su valor anterior. Solo `textContent`. |
| `js/app.js` | El sondeo de `/api/estado` deja de pintar memoria (solo tarjetas y procesos; F04 lo revisa). En `conectarEventos`: `onopen` → `metricas.cargarHistorial()` y `api.get('/api/monitoreo')` → `metricas.aplicarEstado`; `addEventListener('metricas', …)` y `addEventListener('monitoreo', …)`. |
| `css/styles.css` | Interruptor, `.aviso-desactivado` superpuesto con fondo semitransparente, y el apilado de las dos gráficas de fallos. Colores nuevos como variables (`--serie-activas`, `--serie-inactivas`, `--serie-pgfault`, `--serie-pgmajfault`). |

Unidades: kB → GB con `/ 1048576` y 2 decimales (R4.7).

## 7. Pruebas

### 7.1 `pruebas/test_metricas.c` (sin root)

Escribe archivos de ejemplo en un `mktemp -d` (`mkdtemp`) y los borra al final.

1. `meminfo` de ejemplo con las 9 claves y otras extra → valores correctos.
2. `meminfo` sin `Active` → `-1`.
3. `vmstat` de ejemplo con `pgfault`, `pgmajfault` y además `pgfault_file` (distractor) → toma solo los exactos; sin `pgmajfault` → `-1`.
4. `metricas_calcular(m, NULL)` → `primera = 1`, deltas 0.
5. Con anterior → deltas = diferencias.
6. Contador menor que el anterior → delta 0.
7. Derivados: `usada + cache + libre == mem_total`; caso con `MemFree + Buffers + Cached > MemTotal` → `usada == 0`.
8. `metricas_tomar` dos veces con los archivos de ejemplo (el segundo `vmstat` con contadores mayores) → ids 1 y 2, la segunda con delta correcto.
9. `metricas_cambiar_estado(0)` → 1; otra vez `(0)` → 0; `(1)` → 1; el siguiente `metricas_tomar` tiene `primera = 1`.
10. Historial: 155 tomas → `metricas_historial` devuelve 150, ids 6…155 ascendentes; `metricas_desde(150, …)` → 151…155.
11. `json_muestra` se parsea con cJSON y trae los 19 campos; `primera` es booleano.

La prueba define `registrar()`, `fd_aviso = -1` y `struct estado estado` con su mutex inicializado.

### 7.2 `scripts/pruebas_metricas.sh` (sudo, VM)

Reutiliza `usuarios_prueba.sh`; logins por stdin; puerto `8096`; carpeta `~/so2_logs/pruebas_metricas`; `intervalo = 1`.

| # | Verificación | Esperado |
|---|---|---|
| 1 | `GET /api/metricas` sin cookie | 401 |
| 2 | Común: `/api/metricas` | 200, `activo` true, `intervalo_s` 1, ≥ 1 muestra con los 19 campos |
| 3 | Última muestra contra `/proc/meminfo` leído en el script | `mem_total` y `swap_total` exactos; `mem_libre`, `cached`, `activas`, `inactivas` con tolerancia de 5 % o 20 MB |
| 4 | Todas las muestras | `usada + cache + libre == mem_total`; deltas ≥ 0 |
| 5 | SSE del común | 3 `event: metricas` con ids consecutivos en ≤ 4 s |
| 6 | `python3 -I -c` que reserva y toca 200 MB durante un intervalo | algún `pgfault_delta` posterior ≥ 10000 |
| 7 | Común: `POST /api/monitoreo/metricas` | 403 |
| 8 | Admin: `{"activo":"no"}` y JSON inválido | 400 y 400 |
| 9 | Admin: `{"activo":false}` | 200 `cambio` true; alerta `MEDIUM` `Monitoreo del sistema desactivado` con `Métricas del sistema desactivadas por so2t_admin`; llega `event: monitoreo` con `"metricas":false`; `GET /api/monitoreo` → false |
| 10 | Durante 3 s desactivado | ningún `event: metricas`; el último `id` de `/api/metricas` no cambia |
| 11 | Durante la desactivación se lanza `sleep 30 &` | su PID aparece en `/api/estado` → `procesos` en ≤ 3 s (R3.9) |
| 12 | Admin: `{"activo":false}` otra vez | `cambio` false; el `id` máximo de `/api/alertas` no cambia |
| 13 | Admin: `{"activo":true}` | alerta `LOW` `Monitoreo del sistema activado`; siguiente `event: metricas` en < 1 s con `"primera":true` |
| 14 | Salida del daemon | sin contraseñas de prueba |
| 15 | `SIGTERM` | código 0 |

## 8. Decisiones

| Decisión | Alternativa descartada | Motivo |
|---|---|---|
| Métricas por SSE (`event: metricas`) + historial por `/api/metricas` | Seguir sondeando `/api/estado` | Llegan en el instante de la muestra, sin sondeo, por el canal que F02 dejó preparado. |
| Mantener el sondeo de `/api/estado` para tarjetas y procesos | Mover todo a SSE ahora | F04 rehace los procesos; además, ese sondeo renueva la sesión mientras la pestaña está abierta (el SSE no la renueva, F01 R3.11). F04 deberá conservar alguna petición periódica o renovar la sesión de otra forma. |
| `usada = Total − Free − Buffers − Cached` para la dona | `Total − Available` de P4 | La dona debe sumar 100 %; `Available` incluye parte de la caché y se solaparía. `/api/estado` conserva la definición de P4. |
| Dos gráficas apiladas para los fallos | Un solo eje | `pgmajfault` suele ser 100–1000 veces menor y se vería como una línea plana. |
| Historial de 150 muestras | Ilimitado | 5 minutos a 2 s alcanzan para las gráficas y acotan la memoria. |
