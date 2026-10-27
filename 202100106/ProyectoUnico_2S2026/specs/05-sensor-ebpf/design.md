# F05 — Sensor eBPF e integridad de archivos · Diseño

Requisitos: [`requirements.md`](requirements.md). Base: F01 (permisos, `usuarios_prueba.sh`), F02 (`alerta_emitir`, `fd_aviso`, SSE), F03/F04 (`/api/monitoreo`, `event: monitoreo`, patrón `*_desde`).

---

## 1. Visión general

```text
kernel (sensor.bpf.c)
  sys_enter_openat/open/creat ─ ¿TGID = daemon? ¿ruta directamente dentro de config_sensor? ─ sí → pendientes[pid_tgid]
  sys_exit_openat/open/creat  ─ borra pendientes[pid_tgid]
  kprobe fd_install(fd, file) ─ pendientes[pid_tgid] existe → abiertos[file] ← evento
  kprobe security_file_release(file) ─ abiertos[file] existe → Ring Buffer "eventos" ← evento; borra
                                     (liberación = último cierre: después de todas las escrituras)

hilo archivos (sensor.c)                     hilo HTTP (http.c)
  poll([fd_control, epoll del ring], -1)       POST /api/monitoreo/archivos → sensor_activar / sensor_desactivar
  lock_sensor: ring_buffer__consume_n → lote    POST /api/ruta → sensor_cambiar_ruta
  si generación nueva: analisis_linea_base      fd_aviso → difundir_archivos ("event: archivo" / "event: archivos")
  por evento: analisis_procesar
      ├─ intención de escritura y dir actual
      ├─ open O_NOFOLLOW + fstat regular + SHA-256 (sin mutex)
      ├─ lock_archivos: listado (creado / modificado / sin cambio)
      └─ alerta "Archivo modificado" + eventfd_write(fd_aviso)
```

| Pieza | Archivo | Requisito |
|---|---|---|
| Programa eBPF, mapas, filtro | `ProgramaIntermedio/ebpf/sensor.bpf.c`, `sensor.h`, `filtro_ruta.h`, `Makefile` (nuevos) | R1 |
| Carga, activación, ruta, hilo `archivos` | `ProgramaIntermedio/sensor.c` (se reescribe) | R2, R3 |
| SHA-256, listado, análisis inicial, validación de ruta | `ProgramaIntermedio/analisis.c` (nuevo) | R3.2, R4 |
| JSON | `ProgramaIntermedio/json.c` | R5 |
| Endpoints y difusión | `ProgramaIntermedio/http.c`, filas en `auth.c` | R2.3, R3, R5 |
| Arranque y apagado | `ProgramaIntermedio/main.c`, `Makefile` | R2.1, R2.2, R2.7 |
| Dashboard | `Dashboard/index.html`, `js/archivos.js` (nuevo), `js/app.js`, `css/styles.css` | R6 |
| Pruebas | `pruebas/test_analisis.c`, `scripts/pruebas_sensor.sh`, `scripts/correr_pruebas.sh` | R7 |

## 2. Encabezados compartidos con el eBPF

`ebpf/sensor.h`: solo tipos de C simples, para que lo incluyan tanto `sensor.bpf.c` (con `vmlinux.h`) como el daemon (con `-Iebpf`):

```c
#define SENSOR_RUTA_MAX		256
#define SENSOR_COMM_MAX		16
#define SENSOR_FLAGS_CREAT	01101

struct config_sensor {
	unsigned int largo;
	char ruta[SENSOR_RUTA_MAX];
};

struct evento_archivo {
	unsigned int pid;
	unsigned int flags;
	char proceso[SENSOR_COMM_MAX];
	char ruta[SENSOR_RUTA_MAX];
};
```

- `config_sensor.ruta` es la ruta canónica **con `/` final** y `largo` es su `strlen`.
- `SENSOR_FLAGS_CREAT` = `O_CREAT | O_WRONLY | O_TRUNC` (`creat(2)` no recibe `flags`). Es metadato, no una decisión.

`ebpf/filtro_ruta.h`: la **única** lógica de decisión del sensor. Es C puro, sin helpers de BPF, y se compila igual en el eBPF y en `test_analisis.c`:

```c
static inline int ruta_en_directorio(const char *ruta, const char *dir, unsigned int largo)
```

Devuelve 1 si `ruta[0..largo)` es igual a `dir[0..largo)` y, desde `largo` hasta el primer `'\0'`, hay al menos un carácter y ninguna `/`. Devuelve 0 si `largo` es 0 o es `>= SENSOR_RUTA_MAX`, o si no aparece `'\0'` antes de `SENSOR_RUTA_MAX`. Un solo `for (i = 0; i < SENSOR_RUTA_MAX; i++)` con límite constante, como pide el verificador.

## 3. `ebpf/sensor.bpf.c`

```c
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "sensor.h"
#include "filtro_ruta.h"
```

### 3.1 Mapas (nombres de 15 caracteres o menos, como los muestra `bpftool`)

| Mapa | Tipo | Clave → valor | Uso |
|---|---|---|---|
| `config_sensor` | `ARRAY`, 1 | `__u32` → `struct config_sensor` | escrito por el daemon con `bpf_map_update_elem()` |
| `pid_daemon` | `ARRAY`, 1 | `__u32` → `__u32` | TGID del daemon |
| `auxiliar` | `PERCPU_ARRAY`, 1 | `__u32` → `struct evento_archivo` | búfer de trabajo (la pila del eBPF es de 512 bytes) |
| `pendientes` | `HASH`, 1024 | `__u64` (`pid_tgid`) → `struct evento_archivo` | apertura en curso que pasó el filtro |
| `abiertos` | `LRU_HASH`, 8192 | `__u64` (puntero a `struct file`) → `struct evento_archivo` | archivo abierto que pasó el filtro |
| `eventos` | `RINGBUF`, 256 KiB | — | al daemon |

### 3.2 Programas

| Sección | Función | Hace |
|---|---|---|
| `tracepoint/syscalls/sys_enter_openat` | `ent_openat` | `registrar_apertura((const char *)ctx->args[1], ctx->args[2])` |
| `tracepoint/syscalls/sys_enter_open` | `ent_open` | `registrar_apertura((const char *)ctx->args[0], ctx->args[1])` |
| `tracepoint/syscalls/sys_enter_creat` | `ent_creat` | `registrar_apertura((const char *)ctx->args[0], SENSOR_FLAGS_CREAT)` |
| `tracepoint/syscalls/sys_exit_{openat,open,creat}` | `sal_openat`, `sal_open`, `sal_creat` | borra `pendientes[pid_tgid]` (falle o no la apertura) |
| `kprobe/fd_install` | `BPF_KPROBE(instalar_fd, unsigned int fd, struct file *archivo)` | si hay `pendientes[pid_tgid]`, lo copia a `abiertos[(__u64)archivo]` y lo borra de `pendientes` |
| `kprobe/security_file_release` | `BPF_KPROBE(liberar_archivo, struct file *archivo)` | si hay `abiertos[(__u64)archivo]`, `bpf_ringbuf_reserve` + copia + `bpf_ringbuf_submit`, y lo borra de `abiertos` |

`registrar_apertura(ruta_usuario, flags)`, en este orden:
1. `tgid = bpf_get_current_pid_tgid() >> 32`; si es igual a `pid_daemon[0]` → return.
2. `cfg = bpf_map_lookup_elem(&config_sensor, &cero)`; si `!cfg || cfg->largo == 0` → return.
3. `aux = lookup(&auxiliar)`; `n = bpf_probe_read_user_str(aux->ruta, SENSOR_RUTA_MAX, ruta_usuario)`; si `n <= 0 || n >= SENSOR_RUTA_MAX` → return (error o ruta posiblemente truncada).
4. Si `!ruta_en_directorio(aux->ruta, cfg->ruta, cfg->largo)` → return.
5. `aux->pid = tgid; aux->flags = flags; bpf_get_current_comm(aux->proceso, …)`; `bpf_map_update_elem(&pendientes, &pid_tgid, aux, BPF_ANY)`.

`char LICENSE[] SEC("license") = "GPL";` (los helpers `bpf_probe_read_*` son GPL).

Ambos símbolos de los kprobes están en el kernel de la VM (`/proc/kallsyms`: `T fd_install`, `T security_file_release`). En 6.12, `security_file_release` se llama desde `__fput`, es decir, en el último cierre. Ahí no se lee ningún campo de `struct file`: el puntero solo sirve de clave.

### 3.3 `ebpf/Makefile`

```make
CLANG ?= clang
BPFTOOL ?= bpftool

all: sensor.skel.h

vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@.tmp && mv $@.tmp $@

sensor.bpf.o: sensor.bpf.c sensor.h filtro_ruta.h vmlinux.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_x86 -I. -c $< -o $@

sensor.skel.h: sensor.bpf.o
	$(BPFTOOL) gen skeleton $< name sensor_bpf > $@.tmp && mv $@.tmp $@

clean:
	rm -f vmlinux.h sensor.bpf.o sensor.skel.h *.tmp

.PHONY: all clean
```

`Makefile` principal: `CFLAGS += -Iebpf`; `LDLIBS = -lpam -lbpf -lcrypto`; `analisis.c` en `FUENTES`; `sensor.o: ebpf/sensor.skel.h`; regla `ebpf/sensor.skel.h: ebpf/sensor.bpf.c ebpf/sensor.h ebpf/filtro_ruta.h` → `$(MAKE) -C ebpf`; `clean` también hace `$(MAKE) -C ebpf clean`. `make pruebas` no necesita el skeleton (no enlaza `sensor.o`), pero sí `analisis.o` y `-lcrypto` en las pruebas que enlazan `json.o`.

## 4. `analisis.c` (sin libbpf)

```c
#define MAX_ARCHIVOS		256
#define LOTE_EVENTOS		64
#define SENSOR_DIR_MAX		200

enum estado_archivo { ARCHIVO_BENIGNO, ARCHIVO_INFECTADO };
enum accion_archivo { ACCION_INICIAL, ACCION_CREADO, ACCION_MODIFICADO };
enum resultado_analisis { ANALISIS_IGNORADO, ANALISIS_SIN_CAMBIO, ANALISIS_CREADO, ANALISIS_MODIFICADO };
enum resultado_ruta { RUTA_OK = 0, RUTA_INVALIDA = -1, RUTA_NO_DIRECTORIO = -2, RUTA_LARGA = -3, RUTA_ERROR_MAPA = -4 };

struct archivo_analizado {
	unsigned long seq;
	char ruta[SENSOR_RUTA_MAX];
	char sha256[65];
	long long tamano;
	struct timespec mtime;
	enum estado_archivo estado;
	enum accion_archivo accion;
	unsigned int pid;
	char proceso[SENSOR_COMM_MAX];
	char analizado[32];
};

struct estado_sensor {
	int activo;
	char ruta[PATH_MAX];
	char error[128];
};

int analisis_validar_ruta(const char *entrada, char *canonica, size_t tam);
int analisis_sha256_fd(int fd, char hex[65]);
enum resultado_analisis analisis_procesar(const char *dir, unsigned long generacion,
					  const struct evento_archivo *e);
size_t analisis_linea_base(const char *dir, unsigned long generacion);
void analisis_reiniciar(void);
unsigned long analisis_generacion(void);
size_t analisis_desde(unsigned long ultimo_seq, struct archivo_analizado *destino, size_t max);
size_t analisis_listado(struct archivo_analizado *destino, size_t max);
void analisis_publicar_sensor(int activo, const char *ruta, const char *error);
void analisis_estado_sensor(struct estado_sensor *destino);
```

- **Estado:** `static struct archivo_analizado listado[MAX_ARCHIVOS]`, `cantidad`, `ultimo_seq`, `generacion`, `struct estado_sensor publicado`, todo bajo `lock_archivos`. `json.c` lee el estado del sensor desde aquí, así `json.o` y las pruebas no dependen de libbpf.
- **`analisis_validar_ruta`**, en este orden:
  1. `entrada` vacía, sin `/` inicial o de `PATH_MAX` caracteres o más → `RUTA_INVALIDA`;
  2. `realpath` falla o `stat` no es directorio → `RUTA_NO_DIRECTORIO`;
  3. la canónica es `/` → `RUTA_INVALIDA`;
  4. `strlen > SENSOR_DIR_MAX` → `RUTA_LARGA`.

  Escribe la canónica sin `/` final.
- **`analisis_sha256_fd`**: `EVP_MD_CTX_new` / `EVP_DigestInit_ex(EVP_sha256())` / `read` en bloques de 64 KiB (reintenta `EINTR`) / `EVP_DigestFinal_ex`; hex en minúsculas. Devuelve `-1` si falla.
- **`analisis_procesar(dir, generacion, e)`**:
  1. Fuerza `e->ruta[SENSOR_RUTA_MAX-1] = '\0'` (en una copia).
  2. Sin intención de escritura (`(flags & O_ACCMODE) == O_RDONLY` y sin `O_CREAT|O_TRUNC`) → `IGNORADO`.
  3. Si `ruta_en_directorio(ruta, dir + "/", largo)` es falso → `IGNORADO`. Es el mismo filtro del eBPF, para descartar eventos de una ruta anterior.
  4. `open(O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)`, `fstat`, `S_ISREG` y SHA-256, **sin mutex**. Si algo falla → `IGNORADO`.
  5. Con `lock_archivos`: si `generacion != generacion actual` → `IGNORADO`. Busca la ruta:
     - si no está → inserta (si el listado está lleno, reemplaza la entrada de menor `seq`) con `CREADO`;
     - si está y `sha256` o `mtime` (`tv_sec` y `tv_nsec`) difieren → actualiza con `MODIFICADO`;
     - si no difieren → `SIN_CAMBIO`, sin tocar la entrada.

     Asigna `seq = ++ultimo_seq` y `analizado = ahora`.
  6. Fuera del mutex: con `CREADO` o `MODIFICADO`, `alerta_emitir(MOT_ARCHIVO_MODIFICADO, "El archivo %s fue %s por el PID %u (%s)", ruta, "creado"|"modificado", pid, proceso)` y `eventfd_write(fd_aviso, 1)`.
- **`analisis_linea_base(dir, generacion)`**: `opendir` y, por cada entrada distinta de `.` y `..` cuya ruta quepa en `SENSOR_RUTA_MAX`, abre, comprueba y calcula el hash como en el paso 4. Con `lock_archivos`: aborta si la generación cambió; si no, inserta o actualiza con `ACCION_INICIAL`, `pid = 0` y `proceso = ""`. Para al llegar a `MAX_ARCHIVOS`. **Sin alertas.** Al final hace un solo `eventfd_write(fd_aviso, 1)`. Devuelve cuántos archivos analizó.
- **`analisis_reiniciar`**: vacía el listado y hace `generacion++`. No notifica.
- **`analisis_desde`**: entradas con `seq > ultimo_seq`, en orden de `seq` ascendente. **`analisis_listado`**: todas, de `seq` descendente.

## 5. `sensor.c` (libbpf y hilo `archivos`)

```c
int sensor_iniciar(const char *ruta_configurada);
int sensor_activar(void);
int sensor_desactivar(void);
int sensor_cambiar_ruta(const char *entrada, char *canonica, size_t tam);
int sensor_activo(void);
void sensor_finalizar(void);
void *hilo_archivos(void *sin_uso);
void archivos_despertar(void);
```

Estado privado bajo `lock_sensor`: `struct sensor_bpf *esqueleto`, `struct ring_buffer *consumidor`, `char ruta_actual[PATH_MAX]`, `int ruta_valida`, `char error_sensor[128]`, más `int fd_control` (`eventfd`, `EFD_NONBLOCK|EFD_CLOEXEC`). **Orden de locks: `lock_sensor` → `lock_archivos`, nunca al revés.** Después de cada cambio de estado: `analisis_publicar_sensor(...)` (dentro de `lock_sensor`), luego soltar el lock y `eventfd_write(fd_control, 1)`.

- **`sensor_iniciar(ruta)`**: crea `fd_control`; `libbpf_set_print(imprimir_libbpf)`, que manda solo `LIBBPF_WARN` a `registrar("libbpf: %s", …)` sin el `\n` final (R2.8); `analisis_validar_ruta`. Si la ruta no vale, `ruta_valida = 0`, guarda la ruta tal cual y `error_sensor = "La ruta vigilada no es un directorio existente"`.
- **`sensor_activar()`** (devuelve 1 si cambió, 0 si ya estaba activo y -1 si hubo error, con `error_sensor` lleno):
  1. si `esqueleto` → 0;
  2. si `!ruta_valida` → -1;
  3. `sensor_bpf__open_and_load()` → si falla, `error_sensor = "No se pudo cargar el sensor: <strerror(errno)>"`;
  4. `bpf_map_update_elem(bpf_map__fd(esqueleto->maps.pid_daemon), &cero, &pid, BPF_ANY)` con `pid = getpid()`, y `config_sensor` con `ruta_actual + "/"`, **antes** de adjuntar;
  5. `sensor_bpf__attach()`;
  6. `ring_buffer__new(bpf_map__fd(esqueleto->maps.eventos), recibir_evento, NULL, NULL)`.

  Si falla 4–6 → `sensor_bpf__destroy` y -1. Si todo sale bien: `error_sensor = ""`, `analisis_reiniciar()`, publicar y 1.
- **`sensor_desactivar()`**: si no hay `esqueleto` → 0. Si no: `ring_buffer__free`, `sensor_bpf__destroy` (desadjunta y descarga), punteros a `NULL`, publicar, 1.
- **`sensor_cambiar_ruta(entrada, canonica, tam)`**: `analisis_validar_ruta` **fuera** del lock (toca el disco). Con el lock: si es igual a `ruta_actual` → 0. Si no: copia, `ruta_valida = 1`, limpia `error_sensor`, actualiza `config_sensor` si hay `esqueleto` (si `bpf_map_update_elem` falla → `RUTA_ERROR_MAPA`), `analisis_reiniciar()`, publica y devuelve 1.
- **`hilo_archivos`**:

  ```text
  gen_analizada = 0
  mientras ejecutando:
      pf = [fd_control]; con lock_sensor: si consumidor → pf += ring_buffer__epoll_fd(consumidor)
      poll(pf, n, -1)          (EINTR → continuar)
      si fd_control legible → eventfd_read
      si !ejecutando → salir
      con lock_sensor:
          cantidad = 0; si consumidor → ring_buffer__consume_n(consumidor, LOTE_EVENTOS)
          dir ← ruta_actual; gen ← analisis_generacion(); activo ← esqueleto != NULL
      si activo y gen != gen_analizada → analisis_linea_base(dir, gen); gen_analizada = gen
      por cada evento del lote → analisis_procesar(dir, gen, &lote[i])
  ```

  `recibir_evento(ctx, data, tam)` solo copia a `static struct evento_archivo lote[LOTE_EVENTOS]` si `tam == sizeof(struct evento_archivo)`; no hace nada más. El hash y las alertas nunca ocurren con `lock_sensor` tomado. Si el HTTP destruye el consumidor mientras este hilo duerme en `poll`, el `eventfd_write(fd_control)` lo despierta y el hilo vuelve a leer el estado bajo el lock. El descriptor epoll es *level-triggered*: si quedan más de 64 eventos, el siguiente `poll` vuelve enseguida.
- **`archivos_despertar()`**: `eventfd_write(fd_control, 1)` (apagado).
- **`sensor_finalizar()`**: `sensor_desactivar()` y `close(fd_control)`.

`main.c`:
- después de `daemon(3)` y de crear `fd_aviso`, y antes de crear los hilos: `sensor_iniciar(conf.ruta_vigilada)`; si `sensor_activar() < 0` → `registrar("sensor eBPF no disponible: %s", error)`;
- al apagar, después del `pthread_join` del hilo `archivos`: `sensor_finalizar()`.

## 6. JSON, `http.c` y `auth.c`

Filas nuevas (16 en total):

```c
{ "GET",  "/api/archivos",           ROL_COMUN },
{ "POST", "/api/monitoreo/archivos", ROL_ADMIN },
{ "POST", "/api/ruta",               ROL_ADMIN },
```

- **`json_archivo(b, a)`**: `{"ruta","nombre"` (lo que va después de la última `/`), `"sha256","tamano","mtime"` (`%Y-%m-%d %H:%M:%S` local), `"estado"` (`benigno`|`infectado`), `"accion"` (`inicial`|`creado`|`modificado`), `"pid","proceso","analizado"}`. Cadenas con `json_cadena`.
- **`json_archivos(b)`**: `analisis_estado_sensor` + `analisis_listado` en un arreglo de `malloc` (no en la pila) → `{"activo","ruta","archivos":[…]}`.
- **`json_monitoreo(b)`**: `{"metricas","procesos","archivos","ruta_vigilada","sensor_error"}`.
- **`leer_activo(c, &activo)`**: helper nuevo que interpreta `{"activo":bool}`. Lo usan `atender_monitoreo_metricas`, `atender_monitoreo_procesos` y `atender_monitoreo_archivos`; se quita el código repetido sin cambiar respuestas ni mensajes.
- **`atender_monitoreo_archivos(c, usuario)`**: `r = activo ? sensor_activar() : sensor_desactivar()`.
  - `r < 0` → 500 `No se pudo cargar el sensor eBPF`;
  - `r == 1` → alerta de R2.4 + trama `event: monitoreo`;
  - respuesta `{"archivos":<sensor_activo()>,"cambio":<r == 1>}`.
- **`atender_ruta(c, usuario)`**: `ruta` debe ser `cJSON_IsString`; `sensor_cambiar_ruta`.
  - `RUTA_INVALIDA`, `RUTA_NO_DIRECTORIO`, `RUTA_LARGA` → 400 con los mensajes de R3.2;
  - `RUTA_ERROR_MAPA` → 500 `No se pudo actualizar el sensor`;
  - con 1 → `registrar("ruta vigilada cambiada a %s por %s")` + `event: monitoreo`;
  - respuesta `{"ruta":<canónica>,"cambio":bool}`.
- **`difundir_archivos()`** (en el despertar por `fd_aviso`, junto a las otras): `static unsigned long gen_difundida, seq_difundido`.
  - Si `analisis_generacion() != gen_difundida` → trama `event: archivos` con `json_archivos`; luego `gen_difundida = gen` y `seq_difundido` = el mayor `seq` enviado.
  - Si no → `analisis_desde(seq_difundido, lote, 64)` en bucle, con una trama `event: archivo` por entrada.

## 7. Dashboard

| Archivo | Cambio |
|---|---|
| `index.html` | `#panel-archivos` después de `#panel-procesos`. Cabecera: `<label data-rol="admin_user"><input type="checkbox" id="interruptor-archivos"> Escaneo de archivos</label>`, `<span id="estado-archivos">`, `<span id="ruta-actual">`, `<p id="sensor-error" hidden>`. Formulario `data-rol="admin_user"` `#form-ruta` con `<input id="ruta-vigilada">` y el botón **Cambiar ruta**. `<p id="archivos-aviso" aria-live="polite">`. Tabla Archivo · Estado · Última modificación · SHA-256 · Último evento. `<p class="aviso-desactivado" hidden>Escaneo de archivos desactivado</p>`, que no tapa la cabecera (mismo arreglo que `5340ba7890`). Carga `js/archivos.js`. |
| `js/archivos.js` | Nuevo. `const archivos = { cargar(), aplicarLista(d), aplicarArchivo(a), aplicarEstado(m), conectarControles() }`. Inserta o actualiza por `ruta` y reordena por `analizado`; máximo 256 filas. Estado con una clase por valor (`benigno`/`infectado`); el SHA-256 muestra 12 caracteres y lleva el completo en `title`. Interruptor → `api.post('/api/monitoreo/archivos', {activo})`; formulario → `api.post('/api/ruta', {ruta})`; muestra la ruta canónica o el `error`. Solo `textContent`. |
| `js/app.js` | En `conectarEventos`: `onopen` → `archivos.cargar()`; `addEventListener('archivo', …)` y `addEventListener('archivos', …)`; el manejador de `monitoreo` también llama a `archivos.aplicarEstado(d)`. Al iniciar: `archivos.conectarControles()`. |
| `css/styles.css` | Estado `benigno` con el color de éxito existente e `infectado` con `--critico`; hash en monoespaciado. |

## 8. Pruebas

### 8.1 `pruebas/test_analisis.c` (sin root)

Directorio temporal con `mkdtemp` (borrado al final). Enlaza `analisis.o alertas.o json.o metricas.o procesos.o lib/cJSON.o -lcrypto`.

1. **Filtro** (`ruta_en_directorio` con `dir = "/d/v/"`, `largo = 5`):
   - `/d/v/a.txt` → 1;
   - `/d/v/sub/a.txt`, `/d/v/`, `/d/va/a.txt`, `/d/a.txt`, `d/v/a.txt` → 0;
   - `largo` 0 → 0;
   - una ruta de 256 bytes sin `'\0'` → 0.
2. **SHA-256** con `analisis_sha256_fd`:
   - archivo vacío → `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`;
   - `abc` → `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`;
   - 1 000 000 de `a` (cruza bloques de 64 KiB) → `cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0`.
3. **Validación de rutas**:
   - `""`, `relativa`, `/` → `INVALIDA`;
   - inexistente o un archivo regular → `NO_DIRECTORIO`;
   - un directorio con `/./` y `/` final → `OK`, con la canónica limpia;
   - un enlace simbólico a un directorio → la canónica del destino;
   - un anidamiento de directorios de más de 200 caracteres → `LARGA`.
4. **`analisis_procesar`** sobre `<tmp>/v`:
   - archivo nuevo → `CREADO` y una alerta `Archivo modificado` con la descripción exacta `El archivo <ruta> fue creado por el PID 4242 (prueba)`;
   - mismo contenido → `SIN_CAMBIO` y ninguna alerta nueva;
   - otro contenido → `MODIFICADO`;
   - `flags = O_RDONLY` → `IGNORADO`;
   - `<v>/sub/x`, otro directorio, un enlace simbólico dentro de `v`, un FIFO dentro de `v` (no debe bloquear) o un archivo inexistente → `IGNORADO`.
5. **Generación**: `analisis_procesar` con una generación vieja → `IGNORADO` y el listado sin cambios.
6. **Análisis inicial**: `v` con 3 archivos, un subdirectorio y un enlace → 3 entradas `inicial`, `pid` 0 y ninguna alerta nueva; `analisis_reiniciar` vacía el listado e incrementa la generación.
7. **Límite**: 257 archivos procesados → 256 entradas; la primera ya no está.
8. **JSON**:
   - `json_archivos` se parsea con cJSON y tiene `activo`, `ruta` y `archivos`;
   - cada entrada tiene las 10 claves de R5.1;
   - `analisis_desde(0, …)` devuelve en orden de `seq` y `analisis_listado` en orden inverso.

`test_auth.c`: 16 filas, con las 3 nuevas.

### 8.2 `scripts/pruebas_sensor.sh` (sudo, VM)

Reutiliza `usuarios_prueba.sh`; logins por stdin; puerto **8094**. Directorios del usuario real, creados con `sudo -u "$SUDO_USER"`: `~/so2_vigilado/pruebas_sensor` (con `sub/` y dos archivos previos, `base1.txt` y `base2.txt`) y `~/so2_vigilado/pruebas_sensor_b`. El `trap` los borra junto con el daemon y los usuarios. Las escrituras se hacen como `$SUDO_USER` (nunca como el daemon). El script falla de entrada si no están `bpftool` y `sha256sum`.

| # | Verificación | Esperado |
|---|---|---|
| 1 | Arranque | `/api/monitoreo`: `archivos` true, `ruta_vigilada` canónica, `sensor_error` vacío |
| 2 | Kernel | `bpftool prog show` lista `ent_openat` y `liberar_archivo`; `bpftool map dump name config_sensor` contiene la ruta con `/`; `pid_daemon` vale el PID del daemon |
| 3 | Análisis inicial | `base1.txt` y `base2.txt` con `accion` `inicial` y `sha256` igual a `sha256sum`; ninguna alerta `Archivo modificado` |
| 4 | Permisos | `/api/archivos` sin cookie → 401; común: `GET` 200, `POST /api/monitoreo/archivos` 403, `POST /api/ruta` 403 |
| 5 | Creación (`python3 -I` como usuario, imprime su PID) | en ≤ 2 s: alerta `LOW` `Archivo modificado`, tipo `archivos`, con `El archivo <ruta> fue creado por el PID <pid> (python3)`; entrada `creado` con ese `pid`; el SSE del común recibe `event: archivo` en ≤ 1 s |
| 6 | Redirección de shell: `bash -c 'echo cambio >> base1.txt'` | alerta `fue modificado por el PID`; `sha256` del listado igual a `sha256sum` **después** de escribir (prueba que el evento llega en la liberación) |
| 7 | Sin bucle | 3 s después de la fase 6: exactamente 1 alerta y 1 `event: archivo` para `base1.txt` |
| 8 | Solo lectura: `cat` y `python3 open(…, 'r')` | ninguna alerta ni `event: archivo` en 2 s |
| 9 | Apertura con escritura sin cambios: `open(…, 'r+').close()` | ninguna alerta |
| 10 | Fuera de alcance: `sub/x.txt` y `pruebas_sensor_b/y.txt` | ninguna alerta |
| 11 | Enlace: `ln -s <archivo fuera> enlace` dentro del directorio y `echo x > enlace` | ninguna alerta ni entrada para `enlace` |
| 12 | Admin desactiva | alerta `MEDIUM` `Escaneo de archivos desactivado por so2t_admin`; `event: monitoreo` con `archivos` false; `bpftool` ya no lista los programas ni `config_sensor`; una escritura no genera alerta; `/api/archivos` con `activo` false y la lista anterior |
| 13 | Desactivar otra vez | `cambio` false, sin alerta |
| 14 | Activar | alerta `LOW`; programas de nuevo en `bpftool`; `event: archivos`; el archivo escrito en la fase 12 aparece como `inicial`, sin alerta |
| 15 | Rutas inválidas | `{"ruta":"relativa"}`, `{"ruta":"/"}`, `{"ruta":5}` → 400 `Ruta invalida`; inexistente y archivo regular → 400 `La ruta no es un directorio existente`; directorio con canónica de más de 200 caracteres → 400 `Ruta demasiado larga` |
| 16 | Cambio de ruta a `pruebas_sensor_b/./` | 200 con la canónica y `cambio` true; `event: monitoreo` con la ruta nueva; `event: archivos`; `config_sensor` cambiado; escribir en el directorio viejo → sin alerta; escribir en el nuevo → alerta. Misma ruta otra vez → `cambio` false |
| 17 | Reposo | 5 s sin actividad de archivos: el hilo `archivos` no consume tics (`/proc/<pid>/task/<tid>/stat`, campos 14+15) |
| 18 | `SIGTERM` | código 0; `bpftool prog show` ya no lista el sensor; el log no tiene contraseñas ni errores de libbpf |

`correr_pruebas.sh`: suite "Feature F05 - Sensor eBPF" antes de F04.

## 9. Decisiones

| Decisión | Alternativa descartada | Motivo |
|---|---|---|
| Emitir en la liberación (`kprobe/security_file_release`) | Emitir en `sys_enter_openat` o `sys_enter_close` | El enunciado pide avisar cuando el archivo fue modificado **y cerrado**. Con `echo x > f`, bash cierra el descriptor original antes de escribir (`dup2`), y un proceso que termina no llama a `close`. Solo la liberación garantiza que el hash se calcula sobre el contenido final. |
| `kprobe/fd_install` para unir ruta y `struct file` | Recorrer la tabla de descriptores en `sys_exit_openat` | Recibe el puntero directamente y no hay que leer estructuras del kernel con CO-RE. |
| Filtro por cadena de ruta | Comparar el inodo del directorio padre | Es lo que describe el enunciado ("compara la ruta … con la ruta almacenada en el mapa"). Las rutas relativas quedan como limitación documentada. |
| Lecturas descartadas en el daemon | Filtrar `flags` en el eBPF | El filtro de ruta es la única lógica permitida en el eBPF. |
| Desactivar = destruir el sensor | Solo desadjuntar | La tabla de permisos dice "cargar y descargar eBPF". Destruir cubre ambas cosas y `bpftool` lo puede comprobar. |
| El hilo HTTP carga el sensor bajo `lock_sensor` | Pedírselo al hilo `archivos` | La respuesta refleja el resultado real (500 si falla). El hilo `archivos` puede estar calculando un hash largo y no debe frenar la API. |
| Análisis inicial sin alertas | Comparar contra el listado anterior al reactivar | El admin apagó el escaneo y ya hubo una alerta `MEDIUM`. Evita una avalancha de alertas al arrancar. |
| `O_NOFOLLOW` + `S_ISREG` + `O_NONBLOCK` | `open` simple | El daemon corre como root sobre un directorio que el usuario puede escribir: un enlace no debe llevarlo a archivos ajenos, ni un FIFO bloquearlo. F06 hará la cuarentena con `fchmod` sobre el descriptor ya abierto. |
| Estado del sensor publicado en `analisis.c` | `json.c` llama a `sensor.c` | `json.o` y las pruebas siguen sin depender de libbpf. |
