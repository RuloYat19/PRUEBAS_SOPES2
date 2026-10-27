# Technology Stack — Proyecto Único

Stack, herramientas, comandos y restricciones técnicas. Al proponer implementaciones, **preferir siempre este stack** y las piezas ya probadas en la Práctica 4 (`../../Practica_4_2S2026/`) antes que introducir dependencias nuevas.

---

## 1. Resumen del stack

| Capa | Tecnología | Notas |
|---|---|---|
| Kernel | Linux **6.12.69 LTS**, `LOCALVERSION=-so2-202100106` | Overlay con las 6 syscalls de P4 + configuración eBPF. |
| Syscalls | C (kernel), `SYSCALL_DEFINEn` | Reutilizadas, **no se crean nuevas**. |
| Sensor | eBPF en C (`*.bpf.c`), **Clang/LLVM**, CO-RE con `vmlinux.h` | Tracepoints `syscalls:sys_enter_*`, mapas + **Ring Buffer**. |
| Daemon | C (gnu11), **GCC + Make**, `pthread` | libbpf, OpenSSL (EVP SHA-256), Linux-PAM. |
| Transporte | HTTP/1.1 propio sobre sockets TCP + **SSE** | JSON en ambos sentidos. Puerto `8080`. |
| Dashboard | **HTML + CSS + JavaScript** vanilla | Gráficas en SVG/Canvas nativo (como P4), `EventSource` para tiempo real. |
| Docs | Markdown | Manual Técnico y Manual de Usuario. |
| SO de pruebas | Debian/Ubuntu (VM) | Daemon se ejecuta como `root`. |

## 2. Dependencias del sistema

```bash
sudo apt install -y build-essential make git bc bison flex libelf-dev libssl-dev \
    dwarves \
    clang llvm libbpf-dev linux-tools-common bpftool \
    libpam0g-dev zlib1g-dev
```

| Paquete | Para qué |
|---|---|
| `dwarves` (`pahole`) | **Obligatorio** para compilar el kernel con `CONFIG_DEBUG_INFO_BTF=y`. |
| `bpftool` | En Ubuntu puede venir en `linux-tools-$(uname -r)`; con kernel propio, compilarlo desde `tools/bpf/bpftool` del árbol 6.12.69 si el paquete no coincide. |
| `clang`, `llvm` | Compilar `sensor.bpf.c` con `-target bpf`. |
| `libbpf-dev`, `bpftool` | Cargar/adjuntar el eBPF, generar `vmlinux.h` y el skeleton. |
| `libssl-dev` | `EVP_DigestInit_ex/Update/Final_ex` con `EVP_sha256()`. |
| `libpam0g-dev` | `pam_start`, `pam_authenticate`, `pam_acct_mgmt`. |
| `libelf-dev`, `zlib1g-dev` | Dependencias de enlace de libbpf. |

## 3. Kernel

### 3.1 Syscalls disponibles (numeración de P4, `syscall_64.tbl`)

| Nº | Syscall | Firma | Origen | Uso en el proyecto |
|---|---|---|---|---|
| 548 | `ipc_channel_send` | `(const char *mensaje)` | P3 | Pruebas / inyección manual. |
| 549 | `ipc_channel_receive` | `(char *buffer)` | P3 | **Hilo 3** bloqueado esperando eventos de `log_watch`. |
| 550 | `start_log_watch` | `(const char *const *rutas, int cantidad, const char *ruta_log_central, const char *palabra_clave)` → `id` | P3 | Arranque del daemon y "Crear logwatch" desde el dashboard. |
| 551 | `stop_log_watch` | `(int id_monitoreo)` | P3 | Apagado del daemon y "Finalizar logwatch". |
| 552 | `get_process_info` | `(pid_t pid, struct process_info *info)` | P2 | Consulta por PID (admin). |
| 553 | `get_processes_state` | `(struct processes_state *state)` | P2 | Base del listado de procesos. |

Estructuras compartidas (deben coincidir byte a byte con el kernel):

```c
struct process_info   { char name[16]; pid_t pid; unsigned long long runtime_ns; unsigned long memory_kb; };
struct processes_state { unsigned int total, running, sleeping, zombie; };
```

Límites heredados: `IPC_MSG_MAX_LEN = 256`, cola IPC de 128 mensajes, `LOG_WATCH_MAX_FILES = 8`, `LOG_WATCH_MAX_KEYWORD = 64`, intervalo interno de `log_watch` 3000 ms. Formato del mensaje IPC: `EVT|<epoch>|<palabra>|<ruta_origen>|<linea>`.

> `sys_get_processes_state` devuelve **conteos agregados**, no una lista. El top 20 por RSS se arma en el daemon leyendo `/proc/[pid]/stat` + `/proc/[pid]/status` (`VmRSS`), como permite el enunciado. Los conteos de la syscall alimentan el resumen.

`kill` se hace con `kill(2)` de glibc (que invoca `sys_kill`). Guardas antes de enviar la señal: `pid != 1`, `pid != getpid()`, y que no sea hilo de kernel (flag `PF_KTHREAD` = `0x00200000` en el campo 9 de `/proc/[pid]/stat`, o `ppid == 2`).

### 3.2 Configuración obligatoria para eBPF

```text
CONFIG_BPF=y
CONFIG_BPF_SYSCALL=y
CONFIG_BPF_EVENTS=y
CONFIG_DEBUG_INFO=y              # requisito de BTF
CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_FTRACE_SYSCALLS=y
```

> ⚠️ **Diferencia con P4:** `Practica_4_2S2026/preparar_kernel.sh` ejecuta `--disable DEBUG_INFO ... --disable DEBUG_INFO_BTF --enable DEBUG_INFO_NONE` para acelerar la compilación. En este proyecto eso **rompe el sensor** (no hay `/sys/kernel/btf/vmlinux`, no hay CO-RE). El script del proyecto debe habilitar BTF y verificarlo.

Verificación tras arrancar el kernel nuevo:

```bash
uname -r                                   # 6.12.69-so2-202100106
ls -l /sys/kernel/btf/vmlinux              # debe existir
zgrep -E 'CONFIG_(BPF|BPF_SYSCALL|BPF_EVENTS|DEBUG_INFO_BTF|FTRACE_SYSCALLS)=' /boot/config-$(uname -r)
grep -E 'sys_(get_process|ipc_channel|start_log|stop_log)' /proc/kallsyms
```

### 3.3 Compilación del kernel (flujo heredado de P4)

```bash
./scripts/preparar_kernel.sh            # descarga, aplica overlay Kernel/, configura, compila
./scripts/instalar_kernel.sh            # modules_install + install + update-grub
sudo reboot                             # elegir 6.12.69-so2-202100106 en GRUB
```

## 4. Sensor eBPF (`ProgramaIntermedio/ebpf/`)

- **Hooks (F05):** tracepoints `syscalls/sys_enter_{openat,open,creat}` (filtro de ruta) y `sys_exit_*` (limpieza); `kprobe/fd_install` (une ruta y `struct file`) y `kprobe/security_file_release` (último cierre → Ring Buffer). Los `flags` viajan como metadato; el daemon decide si hubo intención de escritura. Detalle en `specs/05-sensor-ebpf/design.md`.
- **Mapas:**
  - `config_sensor` (`BPF_MAP_TYPE_ARRAY`, 1 entrada): directorio vigilado (`largo` + `char[256]`, con `/` final) — escrito por el daemon con `bpf_map_update_elem()`.
  - `pid_daemon` (`ARRAY`, 1 entrada): TGID del daemon para descartar sus propios eventos y evitar el bucle al calcular SHA-256.
  - `eventos` (`BPF_MAP_TYPE_RINGBUF`): `struct evento_archivo { pid; flags; proceso[16]; ruta[256]; }`.
  - Auxiliares de F05: `auxiliar` (`PERCPU_ARRAY`), `pendientes` (`HASH`) y `abiertos` (`LRU_HASH`).
- **Lógica permitida en kernel:** solo (1) comparar el prefijo de ruta y verificar que no haya otro `/` después (sin subdirectorios) y (2) descartar el PID del daemon. Nada de hashes ni severidades.
- **Limitación conocida:** rutas relativas en `openat` no se resuelven en eBPF; el daemon/ruta configurada usa rutas absolutas canónicas y se documenta la limitación en el Manual Técnico.

Comandos:

```bash
cd ProgramaIntermedio/ebpf
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -c sensor.bpf.c -o sensor.bpf.o
bpftool gen skeleton sensor.bpf.o > sensor.skel.h
# o simplemente:
make
```

Desde el daemon: `sensor_bpf__open_and_load()` → actualizar mapas → `sensor_bpf__attach()` (activar escaneo) / `sensor_bpf__detach()` (desactivar) → `ring_buffer__new()` + `ring_buffer__poll()` en el Hilo 2 (espera bloqueante del ring buffer, no polling activo de disco).

## 5. Daemon (`ProgramaIntermedio/`)

### 5.1 Compilación

```make
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread -D_GNU_SOURCE -Iebpf -Ilib
LDLIBS  = -lbpf -lelf -lz -lcrypto -lpam -lpthread
```

```bash
cd ProgramaIntermedio
make            # compila ebpf/ primero (skeleton) y luego el daemon
sudo ./daemon_monitor -c daemon.conf      # primer plano para depurar
sudo ./daemon_monitor -c daemon.conf -d   # segundo plano (daemon(3))
```

El ejecutable compilado **se entrega** en el repositorio (requisito del enunciado).

### 5.2 Modelo de hilos

| Hilo | Responsabilidad | Bloquea en |
|---|---|---|
| **T1 Monitoreo** | `/proc/meminfo`, `/proc/vmstat` (deltas), `sys_get_processes_state` + top 20 RSS, alertas de sistema | `nanosleep` entre muestras (intervalo configurable, ~2 s) |
| **T2 Archivos** | Consumo del Ring Buffer, SHA-256, comparación con blacklist, estado de archivos, cuarentena | `ring_buffer__poll()` |
| **T3 IPC** | `sys_ipc_channel_receive` → alerta MEDIUM "Palabra clave detectada en log" | la propia syscall (**prohibido el polling**) |
| **T4 HTTP** (main) | Servidor HTTP + SSE multiplexado con `poll()`, sirve `Dashboard/` estático | `poll()` |

- Un `pthread_mutex_t` por estructura compartida: `alertas`, `blacklist`, `metricas`, `archivos`, `sesiones`, `logwatches`. Nunca llamar a una syscall bloqueante con un mutex tomado.
- Activar/desactivar métricas o procesos = bandera atómica que T1 consulta; el hilo no se destruye.
- Señales `SIGINT`/`SIGTERM`: detener todos los `log_watch`, desadjuntar eBPF, cerrar sockets y salir con 0. `SIGPIPE` ignorada (clientes SSE desconectados).
- Ningún error de syscall, archivo, JSON o cliente termina el proceso: se registra y se genera alerta si aplica.

### 5.3 Seguridad y autenticación

- **PAM:** servicio propio `/etc/pam.d/so2-monitor` (`auth required pam_unix.so` / `account required pam_unix.so`), `pam_start` con función de conversación que entrega la contraseña recibida.
- **Rol:** `getgrouplist()` contra `admin_user` y `common_user`; ambos ⇒ admin; ninguno ⇒ 403 aunque PAM acepte.
- **Sesión:** token aleatorio (`getrandom`) guardado en el daemon y enviado como cookie `HttpOnly; SameSite=Strict`. `EventSource` envía la cookie al mismo origen.
- **Autorización:** cada endpoint declara su rol mínimo y se valida en C antes de ejecutar.
- Alta de usuarios en caliente:

```bash
sudo groupadd -f admin_user && sudo groupadd -f common_user
sudo useradd -m -G admin_user ana && sudo passwd ana
sudo useradd -m -G common_user luis && sudo passwd luis
```

### 5.4 JSON

- **Salida:** escritor propio (`json.c` de P4, con escape de cadenas).
- **Entrada (blacklist, login, configuración):** **cJSON** vendorizado en `ProgramaIntermedio/lib/` (un `.c` + un `.h`, licencia MIT). Justificación para el Manual Técnico: parsear JSON arbitrario subido por el usuario de forma robusta sin escribir un parser propio.
- Blacklist en memoria: tabla hash en C indexada por SHA-256 (hex de 64 caracteres), reemplazada atómicamente bajo mutex al subir un JSON válido. Si el JSON es inválido, se rechaza y la blacklist anterior se conserva.

### 5.5 API HTTP (contrato base)

| Método | Ruta | Rol | Descripción |
|---|---|---|---|
| `POST` | `/api/login` / `/api/logout` | — | PAM + rol; crea/borra sesión. |
| `GET` | `/api/sesion` | común | Usuario y rol actuales. |
| `GET` | `/api/estado` | común | Snapshot: memoria, páginas, fallos, resumen y top 20. |
| `GET` | `/api/eventos` | común | **SSE**: alertas, métricas y archivos en tiempo real. |
| `GET` | `/api/alertas` | común | Historial (más recientes primero). |
| `GET` | `/api/archivos` | común | Archivos analizados (benigno/infectado, mtime). |
| `GET` | `/api/proceso?pid=N` | admin | `sys_get_process_info`. |
| `POST` | `/api/proceso/kill` | admin | `kill(2)` con guardas. |
| `POST` | `/api/monitoreo/{metricas,procesos,archivos}` | admin | Activar/desactivar (archivos = attach/detach eBPF). |
| `POST` | `/api/ruta` | admin | Directorio vigilado → `config_sensor`. |
| `POST` | `/api/blacklist` | admin | Subir y validar JSON de firmas. |
| `GET`/`POST` | `/api/cuarentena` | admin | Listar / poner archivo en cuarentena. |
| `GET`/`POST`/`DELETE` | `/api/logwatch` | admin | Listar / crear / finalizar logwatch por ID. |

Errores siempre en JSON: `{"error": "..."}` con `400/401/403/404/500`.

## 6. Dashboard (`Dashboard/`)

- HTML + CSS + JS sin frameworks ni build. Servido por el daemon en `/`.
- Gráficas propias en SVG/Canvas (reutilizar `charts.js` de P4). Si se adopta una librería (p. ej. Chart.js desde CDN), **justificarla en el Manual Técnico**.
- Tiempo real con `EventSource('/api/eventos')`; acciones con `fetch` + JSON.
- La UI se adapta al rol de `/api/sesion`, pero **toda** autorización ocurre en el daemon.
- Confirmación obligatoria antes de matar un proceso.

## 7. Pruebas y verificación

```bash
# Syscalls tras reiniciar en el kernel nuevo
./scripts/test_syscalls

# Sensor eBPF cargado y adjunto
sudo bpftool prog show | grep -i sensor
sudo bpftool map dump name config_sensor

# Antivirus: archivo cuyo SHA-256 está en la blacklist
echo "contenido de prueba" > /home/$USER/so2_vigilado/malo.txt
sha256sum /home/$USER/so2_vigilado/malo.txt
stat -c '%a %n' /home/$USER/so2_vigilado/malo.txt   # 0 tras la cuarentena

# Logwatch → alerta MEDIUM sin recargar el dashboard
echo "ERROR intento de acceso" >> ~/so2_logs/auth.log

# CPU en reposo (sin polling)
top -b -n 1 -p "$(pidof daemon_monitor)"

# API
curl -s -c c.txt -d '{"usuario":"ana","password":"..."}' localhost:8080/api/login
curl -s -b c.txt localhost:8080/api/estado | head -c 400
```

`scripts/pruebas_integracion.sh` automatiza el recorrido (heredado de P4).

## 8. Convenciones técnicas

- Identificadores, comentarios y mensajes en **español**, coherente con P1–P4 (`metricas.c`, `servidor_iniciar`).
- **Sin comentarios en el código** (C, eBPF, JS, CSS, HTML, Bash, Makefile). Excepciones: shebang, overlay `Kernel/` copiado de P4 y archivos de configuración. Las justificaciones viven en el Manual Técnico.
- Compilar sin warnings con `-Wall -Wextra`.
- Logs vigilados y carpeta monitoreada en rutas de usuario (`~/so2_logs/`, `~/so2_vigilado/`), **nunca en `/tmp`** (`fs.protected_regular` bloquea al hilo de kernel).
- Rutas siempre absolutas y canónicas (`realpath`) antes de pasarlas al kernel o al mapa eBPF.
- Scripts Bash con `set -euo pipefail` y los helpers `log/warn/err/fase` de P4.
- Commits con prefijo corto como en el historial (`add:`, `upd:`, `fix:`).
