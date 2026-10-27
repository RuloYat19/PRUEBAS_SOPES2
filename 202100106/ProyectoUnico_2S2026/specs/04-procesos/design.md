# F04 — Procesos (top 20, PID, kill) · Diseño

Requisitos: [`requirements.md`](requirements.md). Base: F00 (escaneo de `/proc` de P4), F01 (permisos), F02 (alertas), F03 (`/api/monitoreo`, bucle con `pendiente_muestra`).

---

## 1. Visión general

```text
hilo monitoreo, cada intervalo:
    si metricas activas:  metricas_tomar(...)                         (F03)
    si procesos activos:  procesos_tomar("/proc")
        ├─ sys_get_processes_state → resumen
        ├─ procesos_escanear(raiz)  → lista completa (nombre, pid, estado, rss)
        ├─ estado.procesos ← lista (contrato P4 de /api/estado)
        ├─ instantánea ← resumen + top 20 (lock_procesos)
        └─ eventfd_write(fd_aviso)
    si ambos desactivados: espera sin límite hasta reactivación o apagado

hilo HTTP:
    fd_aviso → difundir_alertas / difundir_metricas / difundir_procesos ("event: procesos")
    POST /api/proceso/kill → procesos_validar_kill(raiz "/proc") → kill(pid, SIGKILL) → alerta
```

| Pieza | Archivo | Requisito |
|---|---|---|
| Escaneo, top 20, guardas | `ProgramaIntermedio/procesos.c` (nuevo; sale de `metricas.c`) | R1, R4.2–R4.5 |
| Estado activo de procesos y bucle | `ProgramaIntermedio/metricas.c` | R3.3–R3.6 |
| JSON | `ProgramaIntermedio/json.c` | R2 |
| Endpoints y difusión | `ProgramaIntermedio/http.c`, filas en `auth.c` | R2, R3, R4 |
| Dashboard | `Dashboard/index.html`, `js/procesos.js` (nuevo), `js/app.js`, `js/metricas.js`, `css/styles.css` | R5 |
| Correcciones F03 | `metricas.c`, `scripts/pruebas_metricas.sh` | R6 |
| Pruebas | `pruebas/test_procesos.c`, `scripts/pruebas_procesos.sh` | R7 |

## 2. Tipos y prototipos (`comun.h`)

```c
#define TOP_PROCESOS		20
#define PID_MAXIMO		4194304
#define PF_KTHREAD		0x00200000UL

struct proceso {
	char nombre[TASK_COMM_LEN];
	pid_t pid;
	char estado;
	unsigned long rss_kb;
};

struct instantanea_procesos {
	int valida;
	char timestamp[32];
	struct processes_state resumen;
	struct proceso top[TOP_PROCESOS];
	size_t n_top;
	unsigned long mem_total_kb;
};

enum resultado_kill {
	KILL_OK, KILL_PID_INVALIDO, KILL_PID_UNO, KILL_DAEMON,
	KILL_KERNEL, KILL_NO_LIDER, KILL_NO_EXISTE
};

int procesos_escanear(const char *raiz, struct proceso **lista, size_t *cantidad);
size_t procesos_top(const struct proceso *lista, size_t n, struct proceso *top, size_t max);
double procesos_porcentaje(unsigned long rss_kb, unsigned long mem_total_kb);
enum resultado_kill procesos_validar_kill(const char *raiz, long pid, pid_t pid_daemon,
					  char nombre[TASK_COMM_LEN]);
void procesos_tomar(const char *raiz);
void procesos_instantanea(struct instantanea_procesos *destino);

int procesos_activos(void);
int procesos_cambiar_estado(int activo);

void json_procesos(struct buf *b, const struct instantanea_procesos *p);
```

`struct proceso` gana `rss_kb`; `json_estado` sigue escribiendo solo `nombre`, `pid` y `estado` (R1.5).

## 3. `procesos.c`

### 3.1 Escaneo (R1.1, R1.4)

Se mueve aquí el escaneo de P4 desde `metricas.c` (`leer_proceso` + `escanear_procesos`), recibiendo `raiz` en vez de `"/proc"` fijo para poder probarlo:
- `<raiz>/<pid>/comm` → nombre;
- `<raiz>/<pid>/stat` → estado (después del último `)`);
- `<raiz>/<pid>/status` → `VmRSS:` en kB (si falta, 0).

Si falta `comm` o `stat`, la entrada se omite (el proceso terminó entre `readdir` y la lectura).

### 3.2 Top 20 (R1.2, R1.3)

- `procesos_top`: copia la lista, `qsort` por `rss_kb` descendente y `pid` ascendente, y devuelve los primeros `max`.
- `procesos_porcentaje(rss, total)`: `total ? rss * 100.0 / total : 0`; se serializa con `%.2f`.
- `mem_total_kb` con `sysinfo(&si)`: `si.totalram * si.mem_unit / 1024`.

### 3.3 Guardas de `kill` (R4.2–R4.5)

`procesos_validar_kill(raiz, pid, pid_daemon, nombre)`, en este orden:
1. `pid < 1 || pid > PID_MAXIMO` → `KILL_PID_INVALIDO`.
2. `pid == 1` → `KILL_PID_UNO`.
3. Leer `<raiz>/<pid>/status`; si no existe → `KILL_NO_EXISTE`. Tomar `Tgid:`.
4. `pid == pid_daemon || tgid == pid_daemon` → `KILL_DAEMON` (cubre los hilos `monitoreo`, `archivos` e `ipc`).
5. Leer `<raiz>/<pid>/stat`: el campo 9 contado después del `)` (estado = campo 3, flags = campo 9). Si `flags & PF_KTHREAD` o `pid == 2` → `KILL_KERNEL`.
6. `tgid != pid` → `KILL_NO_LIDER`.
7. Copiar el nombre de `comm` en `nombre` → `KILL_OK`.

El orden importa: un hilo del daemon debe responder "propio daemon", no "no corresponde a un proceso".

### 3.4 Toma e instantánea

```c
static struct instantanea_procesos instantanea;
static pthread_mutex_t lock_procesos = PTHREAD_MUTEX_INITIALIZER;
```

`procesos_tomar(raiz)`:
1. `sys_get_processes_state` (error → `registrar`, resumen anterior); `procesos_escanear`; `sysinfo`.
2. Calcular el top fuera de cualquier mutex.
3. Si `!procesos_activos()` → descartar (misma carrera que R6.1).
4. Bajo `lock_procesos`: llenar `instantanea` (`valida = 1`, timestamp, resumen, top, `mem_total_kb`).
5. Bajo `estado.lock`: reemplazar `estado.procesos`/`estado.n_procesos` y `estado.resumen` (contrato de `/api/estado`); liberar la lista anterior fuera del mutex.
6. `eventfd_write(fd_aviso, 1)` y marcar `hay_procesos_nuevos` (variable atómica) para que `difundir_procesos` sepa que hay algo que enviar.

`procesos_instantanea(destino)` copia bajo `lock_procesos`.

## 4. Estado activo y bucle (`metricas.c`, R3)

- `procesos_activos` / `procesos_cambiar_estado`: misma técnica que las métricas (`__atomic_exchange_n`; al activar, `pendiente_muestra = 1` + `pthread_cond_signal`).
- Bucle de `hilo_monitoreo`:

  ```c
  while (ejecutando) {
      pendiente_muestra = 0;
      unlock(lock_espera);
      if (metricas_activas()) metricas_tomar("/proc/meminfo", "/proc/vmstat");
      if (procesos_activos()) procesos_tomar("/proc");
      lock(lock_espera);
      if (!metricas_activas() && !procesos_activos()) {
          while (ejecutando && !pendiente_muestra)
              pthread_cond_wait(&cond_espera, &lock_espera);
      } else {
          limite = ahora + intervalo;
          while (ejecutando && !pendiente_muestra &&
                 pthread_cond_timedwait(&cond_espera, &lock_espera, &limite) != ETIMEDOUT)
              ;
      }
  }
  ```

  `monitoreo_despertar()` (apagado) ya hace `signal` con `ejecutando = 0`, así que la espera sin límite también termina.

## 5. Correcciones de F03 (R6)

- `metricas_tomar`: dentro del bloque con `lock_metricas` tomado, `if (!metricas_activas()) { unlock; return 0; }` antes de asignar el `id`.
- `pruebas_metricas.sh`, fase 13: detener el daemon de la batería y arrancar otro con la misma configuración salvo `intervalo = 5`. Desactivar y esperar 6 s, reactivar y medir: `event: metricas` con `"primera":true` en menos de 1 s. Después, restaurar el daemon de la batería para las fases 14–15 (o moverlas antes).
- Marcar las dos advertencias como `Corregido:` en `specs/03-metricas/review.md`, con su evidencia.

## 6. `http.c` y `auth.c`

Filas nuevas:

```c
{ "GET",  "/api/procesos",           ROL_COMUN },
{ "POST", "/api/monitoreo/procesos", ROL_ADMIN },
{ "POST", "/api/proceso/kill",       ROL_ADMIN },
```

- **`atender_procesos`**: `procesos_instantanea` → `json_procesos`. Si `valida == 0`, responde `top: []` y el resumen en cero.
- **`atender_monitoreo_procesos(c, usuario)`**: misma estructura que `atender_monitoreo_metricas`, con sus textos.
- **`json_monitoreo`**: `{"metricas":…,"procesos":…}`.
- **`atender_kill(c, usuario)`**:
  1. `cJSON_ParseWithLength`; `pid` debe ser `cJSON_IsNumber` con `valuedouble` entero (`valuedouble == (double)(long)valuedouble`); si no → 400 `PID invalido`.
  2. `procesos_validar_kill("/proc", pid, getpid(), nombre)` → códigos y mensajes de R4.2–R4.5.
  3. `kill((pid_t)pid, SIGKILL)`: `0` → alerta `MOT_PROCESO_MATADO` con `Proceso %s (PID %ld) terminado por %s` y 200; `ESRCH` → 404; otro → `registrar` + 500.
- **`difundir_procesos()`**: si `__atomic_exchange_n(&hay_procesos_nuevos, 0)` era 1 → `procesos_instantanea` y trama `event: procesos\ndata: <json>\n\n`. Sin `id:`: es el estado completo, no un historial.

Sobre la carrera entre validar y matar (el PID se podría reutilizar en ese instante): el enunciado exige `sys_kill`. Con `pidfd_send_signal` se cerraría la ventana, pero no es `sys_kill`. Se documenta en el Manual Técnico como limitación conocida.

## 7. Dashboard

| Archivo | Cambio |
|---|---|
| `index.html` | `#panel-procesos`: cabecera con `<label data-rol="admin_user"><input type="checkbox" id="interruptor-procesos"> Escaneo de procesos</label>` y `<span id="estado-procesos">`; tabla con PID · Nombre · Estado · Memoria · % memoria · (columna `data-rol="admin_user"` con **Terminar**); `<p id="procesos-aviso" aria-live="polite">` para el resultado del kill; `<p class="aviso-desactivado" hidden>Escaneo de procesos desactivado</p>`. Se quitan el filtro por estado y el ordenamiento por columnas de P4 (el top ya viene ordenado); se conserva el filtro de texto. Carga `js/procesos.js`. |
| `js/procesos.js` | Nuevo. `const procesos = { cargar(), aplicar(datos), aplicarEstado(activo), conectarInterruptor() }`. Pinta tarjetas y tabla; la barra de % es un `<span>` con `style.width`. **Terminar** usa `confirm()` con el texto exacto de R5.5 y luego `api.post('/api/proceso/kill', {pid})`; muestra `Proceso <nombre> (PID <pid>) terminado` o el `error`. Clic en una fila (fuera del botón) → `consultarPid(pid)` si es admin. Solo `textContent`. |
| `js/app.js` | Quitar `actualizar()` y el sondeo de `/api/estado`, junto con el filtro por estado y el ordenamiento. `setInterval(() => api.sesion(), 60000)`. En `conectarEventos`: `onopen` → `procesos.cargar()`; `addEventListener('procesos', …)`; el manejador de `monitoreo` llama a `metricas.aplicarEstado(d.metricas)` y `procesos.aplicarEstado(d.procesos)`. |
| `css/styles.css` | Barra de % (`--serie-usada`), botón **Terminar** con `--critico`, aviso. |

## 8. Pruebas

### 8.1 `pruebas/test_procesos.c` (sin root)

Crea con `mkdtemp` un `/proc` falso. Cada `<pid>/` lleva `comm`, `stat` (`pid (nombre) S ppid … flags …`) y `status` (`Tgid:`, `VmRSS:`):

| PID | Contenido | Para |
|---|---|---|
| 1 | `init`, RSS 10000 | guarda PID 1 |
| 2 | `kthreadd`, flags con `PF_KTHREAD`, sin `VmRSS` | guarda kernel |
| 50 | `kworker/0:1`, flags `PF_KTHREAD` | guarda kernel |
| 500 | daemon falso, `Tgid 500` | guarda daemon (se pasa `pid_daemon = 500`) |
| 501 | hilo del daemon, `Tgid 500` | guarda daemon por hilo |
| 600 | `app`, `Tgid 600`, RSS 300000 | caso OK |
| 601 | hilo de 600, `Tgid 600` | no líder |
| 100–124 | 25 procesos con RSS distintos, dos con el mismo RSS | top 20 y desempate |
| 700 | solo `comm` (sin `stat`) | se omite |

Casos:
1. `procesos_escanear` lee nombre, estado y RSS; omite el 700; el 2 tiene RSS 0.
2. `procesos_top` devuelve 20, ordenados por RSS descendente; a igual RSS, el PID menor va primero.
3. `procesos_porcentaje(300000, 1000000)` → 30.0; con total 0 → 0.
4. Guardas: 0 y 4194305 → `PID_INVALIDO`; 1 → `PID_UNO`; 500 y 501 → `DAEMON`; 2 y 50 → `KERNEL`; 601 → `NO_LIDER`; 999 → `NO_EXISTE`; 600 → `OK` con nombre `app`.
5. `procesos_cambiar_estado`: 0 → 1, 0 → 0, 1 → 1.
6. `json_procesos` se parsea con cJSON; `porcentaje_memoria` es número; `top` tiene ≤ 20 elementos.

### 8.2 `scripts/pruebas_procesos.sh` (sudo, VM)

Reutiliza `usuarios_prueba.sh`; logins por stdin; puerto `8095`; carpeta `~/so2_logs/pruebas_procesos`; **`intervalo = 3`** (para que la prueba de reactivación inmediata distinga). Lanza un proceso de prueba `python3 -I -c` que reserva y toca 300 MB y luego duerme.

| # | Verificación | Esperado |
|---|---|---|
| 1 | `GET /api/procesos` sin cookie | 401 |
| 2 | Común: `/api/procesos` | 200; `top` ≤ 20, ordenado por `rss_kb` desc; `porcentaje_memoria` = `rss_kb / MemTotal × 100` ± 0.05 |
| 3 | El proceso de 300 MB | aparece en `top` en ≤ 2 intervalos con `rss_kb` ≥ 250000 |
| 4 | SSE del común | `event: procesos` en ≤ intervalo + 1 s |
| 5 | Común: kill y toggle | 403 y 403 |
| 6 | Admin: kill con `{"pid":"abc"}`, `{"pid":1.5}`, `{"pid":0}` | 400 × 3 |
| 7 | Admin: kill PID 1 / PID del daemon / un hilo del daemon (de `/proc/<pid>/task`) / PID 2 / un `kworker` | 403 con el mensaje exacto de cada caso; todos siguen vivos |
| 8 | Admin: kill de un hilo de un proceso de prueba multihilo (`python3` con un `threading.Thread`) | 400 `El PID no corresponde a un proceso` |
| 9 | Admin: kill de un PID inexistente | 404 |
| 10 | Admin: kill del proceso de 300 MB | 200 `SIGKILL`; el proceso muere en ≤ 1 s; alerta `MEDIUM` `Proceso matado` con su nombre, PID y `so2t_admin` |
| 11 | Admin: desactivar procesos | alerta `MEDIUM` `Escaneo de procesos desactivado por so2t_admin`; `event: monitoreo` con `"procesos":false`; `/api/monitoreo` con ambas claves |
| 12 | Desactivado durante 4 s | ningún `event: procesos`; `/api/procesos` con `activo` false; `event: metricas` sigue llegando |
| 13 | Desactivar otra vez | `cambio` false, sin alerta nueva |
| 14 | Activar | alerta `LOW`; `event: procesos` en < 1 s |
| 15 | Desactivar métricas **y** procesos; esperar 5 s | el hilo `monitoreo` no consume tics (`/proc/<pid>/task/<tid>/stat`, campos 14+15 sin cambio) |
| 16 | Reactivar ambos; salida del daemon sin contraseñas; `SIGTERM` | código 0 |

## 9. Decisiones

| Decisión | Alternativa descartada | Motivo |
|---|---|---|
| Top 20 por SSE (`event: procesos`), sin sondeo | Seguir sondeando `/api/estado` | Mismo canal que alertas y métricas; `/api/estado` queda para compatibilidad y pruebas de F00. |
| `GET /api/sesion` cada 60 s para mantener la sesión | SSE que renueva la sesión | No cambia el contrato de F01 (expira por inactividad de peticiones) y además detecta la expiración. |
| `SIGKILL` | `SIGTERM` | "Matar" en el enunciado; `SIGTERM` puede ignorarse y la UI mostraría un éxito falso. |
| Guardas leyendo `/proc` desde una raíz configurable | Leer `/proc` directo | Se pueden probar los 7 casos sin root con un árbol falso. |
| Rechazar hilos no líderes | Matar todo el grupo del hilo | `kill` sobre un TID ajeno afectaría a todo el proceso sin que el admin lo vea en la tabla. |
