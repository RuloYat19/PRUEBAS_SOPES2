# Manual técnico — Sistema Integral de Monitoreo y Seguridad

## 1. Propósito

Implementar una arquitectura de observabilidad y seguridad que reutiliza syscalls del kernel Linux, un daemon en C y un dashboard HTML/CSS/JavaScript. La versión autorizada por el tutor para este proyecto es **Linux 6.18.48**.

## 2. Arquitectura

- **Kernel:** `get_process_info`, `get_processes_state`, `ipc_channel_send`, `ipc_channel_receive`, `start_log_watch`, `stop_log_watch`.
- **Sensor eBPF:** tracepoint `sys_enter_openat`; filtro de ruta y exclusión del PID del daemon; publicación de metadatos en Ring Buffer.
- **Daemon:** hilos separados para métricas/procesos, consumo de eventos de archivos e IPC bloqueante; usa `/proc/meminfo`, `/proc/vmstat`, OpenSSL EVP SHA-256, JSON-C y PAM.
- **Dashboard:** consulta `/api/status`, autentica por `/api/login` y permite controles administrativos en `/api/control`.

## 3. Estructura

```text
ProyectoUnico_2S2026/
├── Kernel/
├── ProgramaIntermedio/
│   ├── daemon.c
│   ├── Makefile
│   ├── config.json
│   ├── blacklist.json
│   └── ebpf/
└── Dashboard/
    ├── index.html
    ├── styles.css
    └── app.js
```

## 4. Dependencias

En Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential gcc make clang llvm bpftool libbpf-dev libelf-dev libssl-dev libpam0g-dev libjson-c-dev pkg-config
```

El kernel debe tener habilitados `CONFIG_BPF`, `CONFIG_BPF_SYSCALL`, `CONFIG_BPF_EVENTS`, `CONFIG_DEBUG_INFO_BTF` y `CONFIG_FTRACE_SYSCALLS`. Verifica la configuración real antes de compilar eBPF.

## 5. Compilación

Desde la carpeta del proyecto:

```bash
cd ProgramaIntermedio
make clean
make
```

`make` genera primero `ebpf/vmlinux.h`, compila el objeto BPF, genera el skeleton de libbpf y compila `daemon`.

## 6. Ejecución

Para depuración en primer plano:

```bash
cd ProgramaIntermedio
sudo ./daemon --foreground
```

Para ejecutarlo desacoplado de la terminal:

```bash
cd ProgramaIntermedio
sudo ./daemon
sleep 1
cat /tmp/proyecto_unico.pid
cat /tmp/proyecto_unico.log
```

El modo normal hace doble `fork`, crea una sesión, cambia el directorio de trabajo a `/`, redirige stdin/stdout/stderr a `/dev/null` y registra mensajes en `/tmp/proyecto_unico.log`. La configuración y el dashboard se compilan con rutas absolutas a partir de `CURDIR`.

Para detenerlo:

```bash
sudo kill -TERM "$(cat /tmp/proyecto_unico.pid)"
tail -n 50 /tmp/proyecto_unico.log
```

## 7. API HTTP

El servidor escucha solamente en `127.0.0.1:8080`.

```bash
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool
```

- `GET /api/status`: métricas, procesos, alertas y archivos observados.
- `POST /api/login`: autenticación PAM, JSON `{"username":"...","password":"..."}`.
- `POST /api/control`: acciones administrativas, cabecera `X-Role: admin_user` y JSON `{"action":"metrics_off"}`. En una versión de producción debe utilizarse una sesión o token firmado; no confiar únicamente en una cabecera enviada por el cliente.

Dashboard: `http://127.0.0.1:8080/`.

## 8. PAM y grupos

Crear los grupos si aún no existen:

```bash
sudo groupadd -f admin_user
sudo groupadd -f common_user
sudo usermod -aG admin_user TU_USUARIO
# o, para una cuenta estándar:
sudo usermod -aG common_user OTRO_USUARIO
```

Cierra y vuelve a abrir la sesión para actualizar los grupos. La autenticación PAM usa el servicio `login`; su configuración puede requerir privilegios de root. No almacenes contraseñas en archivos de configuración.

## 9. Blacklist SHA-256

`blacklist.json` admite un arreglo `signatures`, cada elemento con `sha256`, `name` y `description`. El hash de archivo vacío incluido es únicamente una firma de prueba para validar el flujo; no representa malware real. Genera el SHA-256 de un archivo de prueba con:

```bash
sha256sum archivo_de_prueba.txt
```

La cuarentena del ejemplo cambia permisos a `0000`. Es una simulación de cuarentena y no reemplaza un antivirus; el administrador debe conservar un procedimiento seguro para restaurar permisos.

## 10. Syscalls: integración

Los archivos de `Kernel/kernel/` son referencias de integración. Deben revisarse contra las versiones que ya compilaste en tus prácticas. Las firmas de las syscalls, las estructuras compartidas, `include/linux/syscalls.h`, `arch/x86/entry/syscalls/syscall_64.tbl` y `kernel/Makefile` deben coincidir. No copies encima de tu kernel que funciona sin hacer respaldo.

## 11. Limitaciones pendientes de validar

- El sensor de referencia observa `openat` con flags de escritura/creación; no equivale todavía a confirmar que la escritura haya terminado y el archivo se haya cerrado. Para satisfacer estrictamente ese requisito, correlaciona la apertura y los eventos de escritura/cierre, o utiliza un hook adecuado que reporte el evento después de modificar el archivo.
- El top 20 por RSS requiere que la syscall de procesos exponga cada proceso; el endpoint de ejemplo presenta contadores agregados mientras se integra esa estructura.
- Los controles de API usan `X-Role` como prototipo. Para evaluación, la autorización debe asociarse a una sesión autenticada y validarse en el daemon en cada petición.
- La terminación de procesos, la carga de blacklist desde Dashboard, la cuarentena manual, la gestión de varios logwatch y la tabla completa de procesos deben completarse y probarse según el enunciado del proyecto.

## 12. Pruebas recomendadas

1. Compilar y arrancar el kernel 6.18.48 modificado.
2. Verificar las seis syscalls con programas de prueba independientes.
3. Ejecutar daemon en primer plano y revisar `/tmp/proyecto_unico.log`.
4. Confirmar `/api/status`.
5. Iniciar sesión con usuario PAM de `admin_user` y con `common_user`.
6. Probar alertas de `log_watch` y confirmar que IPC bloquea sin polling.
7. Adjuntar el sensor eBPF y crear/modificar un archivo directamente dentro del directorio configurado.
8. Comparar SHA-256, comprobar alertas HIGH y verificar la cuarentena de un archivo descartable.
9. Cerrar la terminal y comprobar que el modo normal sigue activo; detenerlo con `kill -TERM`.
