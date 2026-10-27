# Especificación Técnica — Práctica 4

**Sistema de Monitoreo y Seguridad con Dashboard en Tiempo Real**  
Carné: 202100106 — Sistemas Operativos 2, 2S2026

---

## 1. Objetivo General y Filosofía de Diseño

Integrar las seis llamadas al sistema desarrolladas de forma aislada en las Prácticas 2 y 3 dentro de un único kernel Linux 6.12.69 LTS, orquestarlas mediante un daemon en lenguaje C que complementa la instrumentación con `/proc/meminfo` y `/proc`, y exponer las métricas en tiempo real a un dashboard web.

El principio rector es **evitar la espera activa (cero polling ciego en eventos)** mediante un patrón productor-consumidor real:
- **Productor:** El hilo de kernel `log_watch` detecta una anomalía y encola un mensaje en el canal IPC del kernel.
- **Consumidor:** El daemon en espacio de usuario permanece bloqueado en `sys_ipc_channel_receive` y despierta únicamente cuando existe un evento genuino, transmitiéndolo de inmediato al frontend vía **Server-Sent Events (SSE)**.

---

## 2. Estado Actual Verificado del Repositorio

| Componente | Estado | Ubicación / Evidencia |
|---|---|---|
| **1. Kernel Base** | Compilado e instalado en sistema | `/boot/vmlinuz-6.12.69-so2-202100106` |
| **2. Archivos Overlay Kernel (P4)** | **Modificados e integrados localmente** | `Practica_4_2S2026/linux-6.12.69/` |
| ↳ `mi_syscall.c` | Portado a 6.12.69 | `kernel/mi_syscall.c` |
| ↳ `syscall_64.tbl` | 6 syscalls registradas (548–553) | `arch/x86/entry/syscalls/syscall_64.tbl` |
| ↳ `Makefile` | Incluye `mi_syscall.o`, `ipc_channel.o`, `log_watch.o` | `kernel/Makefile` |
| ↳ `syscalls.h` | Prototipos y declaraciones agregados | `include/linux/syscalls.h` |
| ↳ `ipc_channel.c` | Expone función `ipc_channel_enqueue_kernel` | `kernel/ipc_channel.c` |
| ↳ `log_watch.c` | Dispara notificación IPC al registrar hallazgo | `kernel/log_watch.c` |
| **3. Daemon en C (Backend)** | Implementado y probado | `Practica_4_2S2026/daemon/` |
| **4. Dashboard Web (Frontend)** | Implementado y probado | `Practica_4_2S2026/frontend/` |

> **Nota de compilación:** Los archivos fuente del kernel en `Practica_4_2S2026/linux-6.12.69/` ya cuentan con todas las modificaciones listas. Para aplicar los cambios al sistema operativo, se debe sincronizar este overlay sobre el árbol de compilación del kernel y realizar una única compilación/instalación.

---

## 3. Mapa Final de Syscalls

| Nº | Nombre en Tabla | Función en Kernel | Archivo | Origen | Rol en Práctica 4 |
|---|---|---|---|---|---|
| **548** | `ipc_channel_send` | `sys_ipc_channel_send` | `kernel/ipc_channel.c` | P3 | Emisión de mensajes IPC desde espacio de usuario. |
| **549** | `ipc_channel_receive` | `sys_ipc_channel_receive` | `kernel/ipc_channel.c` | P3 | Bloqueo pasivo del daemon en espera de eventos de seguridad. |
| **550** | `start_log_watch` | `sys_start_log_watch` | `kernel/log_watch.c` | P3 | Inicio del hilo de monitoreo autónomo en segundo plano. |
| **551** | `stop_log_watch` | `sys_stop_log_watch` | `kernel/log_watch.c` | P3 | Detención segura del hilo de monitoreo al finalizar el daemon. |
| **552** | `get_process_info` | `sys_get_process_info` | `kernel/mi_syscall.c` | P2 | Consulta bajo demanda de métricas de un PID específico. |
| **553** | `get_processes_state` | `sys_get_processes_state` | `kernel/mi_syscall.c` | P2 | Conteo global verificado por kernel de estados de procesos. |

---

## 4. Capa 1 — Kernel (Arquitectura de Notificación Interna)

### 4.1 Notificación Interna de Kernel a Canal IPC

Para evitar llamar a una syscall desde espacio de kernel (lo cual fallaría por validaciones de punteros con `strncpy_from_user`), `ipc_channel.c` provee una interfaz interna que comparte la misma cola, mutex y cola de espera (`wait_queue_head_t`):

```c
int ipc_channel_enqueue_kernel(const char *mensaje);
```

Cuando `log_watch_registrar_hallazgo()` detecta una coincidencia en los archivos vigilados:
1. Escribe la línea completa en el archivo de log centralizado (requisito P3).
2. Construye el mensaje formateado en memoria de kernel.
3. Invoca `ipc_channel_enqueue_kernel()`.
4. La función encola el nodo en `ipc_queue` bajo protección de `ipc_queue_lock` y despierta a los procesos dormidos con `wake_up_interruptible(&ipc_queue_wq)`.

### 4.2 Formato Compacto y Mitigación de Límite (256 bytes)

La constante `IPC_MSG_MAX_LEN` es de **256 bytes**. Para garantizar que el encolado no falle con `-EMSGSIZE`, el formato es delimitado por pipes (`|`):

```text
EVT|<timestamp_epoch>|<palabra>|<ruta_origen>|<linea>
```

- **`timestamp_epoch`:** Segundos UNIX generados con `ktime_get_real_seconds()` (ancho fijo, nunca se corta).
- **`palabra`:** Palabra clave que disparó la alarma.
- **`ruta_origen`:** Ruta absoluta del archivo de log vigilado.
- **`linea`:** Fragmento de la línea coincidente. Al ubicarse al final, si `snprintf` debe truncar el búfer a 256 bytes, solo se trunca el final de la línea. El log centralizado en disco retiene la línea íntegra.

---

## 5. Capa 2 — Daemon Orquestador en C

El daemon se ejecuta como un proceso de fondo continuo en la máquina virtual y coordina las dos naturalezas de datos: eventos asíncronos y métricas periódicas.

### 5.1 Arquitectura Concurrente (Modelo de Hilos)

```text
┌────────────────────────────────────────────────────────────────────────┐
│                        DAEMON EN C (Proceso Principal)                 │
│                                                                        │
│  ┌───────────────────────┐   ┌──────────────────────────────────────┐  │
│  │ Hilo 1: Eventos IPC   │   │ Hilo 2: Métricas Periódicas          │  │
│  │ (Event-Driven)        │   │ (Polling cada N seg)                 │  │
│  │                       │   │                                      │  │
│  │ Bloqueado en:         │   │ 1. Invoca sys_get_processes_state    │  │
│  │ sys_ipc_channel_      │   │ 2. Lee /proc/meminfo                 │  │
│  │ receive()             │   │ 3. Escanea /proc/[pid] para lista    │  │
│  └───────────┬───────────┘   └──────────────────┬───────────────────┘  │
│              │                                  │                      │
│              ▼                                  ▼                      │
│        ┌──────────────────────────────────────────────┐                │
│        │        Estado Compartido en Memoria          │                │
│        │         (Protegido con pthread_mutex)        │                │
│        └──────────────────────┬───────────────────────┘                │
│                               │                                        │
│  ┌────────────────────────────┴─────────────────────────────────────┐  │
│  │ Hilo 3 / Loop Servidor HTTP + SSE (Socket TCP puerto 8080)        │  │
│  │ - GET /api/estado   -> Snapshot JSON completo                     │  │
│  │ - GET /api/eventos  -> Stream SSE (Push inmediato a clientes)     │  │
│  │ - GET /api/proceso  -> Invoca sys_get_process_info(pid)           │  │
│  │ - GET /             -> Sirve dashboard frontend estático          │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

### 5.2 Resolución Arquitectónica de Procesos (`sys_get_processes_state` vs `/proc`)

Existe una distinción fundamental que resuelve el requerimiento del enunciado:
1. **Métricas Globales Verificadas (`sys_get_processes_state`):**
   - La syscall de P2 retorna contadores globales del kernel: `total`, `running`, `sleeping`, `zombie`.
   - El daemon invoca esta syscall periódicamente para alimentar las **tarjetas de resumen** del dashboard. Esto valida formalmente el uso de la syscall exigida en la rúbrica.
2. **Tabla Detallada de Procesos Activos (Escaneo de `/proc`):**
   - Como la syscall del kernel solo retorna conteos agregados, el daemon realiza un escaneo de `/proc` en espacio de usuario para poblar la lista detallada requerida en el JSON.
   - Lee los subdirectorios numéricos `/proc/[pid]`, extrayendo:
     - Nombre desde `/proc/[pid]/comm`.
     - Estado desde `/proc/[pid]/stat` (tercer campo: `R`, `S`, `Z`, `D`, `T`).
     - PID correspondiente.

### 5.3 Métricas de Memoria (`/proc/meminfo`)

El daemon abre y lee `/proc/meminfo` periódicamente, calculando:

| Clave JSON | Fórmula de Cálculo | Explicación |
|---|---|---|
| `memoria_usada` | `MemTotal - MemAvailable` | Memoria física en uso activo por el sistema. |
| `memoria_libre` | `MemAvailable` | Memoria disponible para nuevas asignaciones sin swapping. |
| `memoria_cache` | `Cached + Buffers` | Memoria en caché de página y buffers de E/S. |
| `swap_usada` | `SwapTotal - SwapFree` | Espacio de intercambio actualmente ocupado. |

### 5.4 Contrato Canónico de la API JSON

#### Snapshot Completo (`GET /api/estado`)

```json
{
  "memoria_usada": 2097152,
  "memoria_libre": 6291456,
  "memoria_cache": 1048576,
  "swap_usada": 0,
  "resumen_procesos": {
    "total": 142,
    "running": 2,
    "sleeping": 140,
    "zombie": 0
  },
  "procesos": [
    { "nombre": "systemd", "pid": 1, "estado": "S" },
    { "nombre": "daemon_monitor", "pid": 1234, "estado": "S" }
  ],
  "eventos_seguridad": [
    {
      "archivo_origen": "/home/kwlopez/so2_logs/auth.log",
      "palabra_detectada": "ERROR",
      "contenido": "Failed password for root",
      "timestamp": "2026-10-05 22:10:00"
    }
  ]
}
```

#### Consulta de Proceso por PID (`GET /api/proceso?pid=1234`)
Invoca directamente a `sys_get_process_info(pid, &pinfo)`:

```json
{
  "pid": 1234,
  "nombre": "daemon_monitor",
  "runtime_ns": 45120300,
  "memory_kb": 8192
}
```

En caso de PID no encontrado, responde con código HTTP 404:
```json
{ "error": "Proceso no encontrado (ESRCH)" }
```

### 5.5 Ciclo de Vida y Resiliencia

1. **Inicialización:**
   - Registra manejadores de señales para `SIGINT` y `SIGTERM`.
   - Crea el archivo de log central si no existe.
   - Invoca `sys_start_log_watch` con las rutas de logs a vigilar y almacena el `watch_id`.
2. **Resiliencia en ejecución:**
   - Si una invocación a `sys_get_processes_state` o `sys_get_process_info` retorna error (ej. `-EINVAL` o `-ESRCH`), el daemon registra el fallo en consola/log y **continúa operando sin cerrarse**.
3. **Terminación Limpia (Graceful Shutdown):**
   - Al recibir `SIGINT` (`Ctrl+C`) o `SIGTERM`:
     - Invoca `sys_stop_log_watch(watch_id)` para detener el hilo de kernel de forma segura.
     - Cierra sockets y libera descriptores de clientes SSE activos.
     - Destruye mutexes y sale con código 0.

---

## 6. Capa 3 — Dashboard Web (Frontend en Tiempo Real)

El dashboard web consume la información expuesta por el daemon y proporciona las **cinco visualizaciones obligatorias**:

| Nº | Visualización | Tipo de Componente | Fuente de Datos | Modo de Actualización |
|---|---|---|---|---|
| **1** | **Desglose de uso de memoria** | Gráfico de Pie / Donut (Usada, Libre, Caché) | `/proc/meminfo` | Periódica (polling HTTP cada 2-3s a `/api/estado`) |
| **2** | **Evolución de memoria en el tiempo** | Gráfico de Líneas multiserie (Memoria física vs Swap) | `/proc/meminfo` | Periódica (agrega puntos históricos en cada refresco) |
| **3** | **Tabla de procesos activos** | Tabla filtrable y ordenable (Nombre, PID, Estado) + Tarjetas con conteo total de estados | `sys_get_processes_state` + escaneo `/proc` | Periódica (cada 2-3s a `/api/estado`) |
| **4** | **Panel de Eventos de Seguridad** | Tabla de alertas en tiempo real con indicador sonoro/visual de nuevo evento | Canal IPC (`log_watch`) | **Event-Driven estricto vía SSE (`/api/eventos`)** |
| **5** | **Consulta puntual por PID** | Formulario con campo de entrada numérico + Tarjeta con detalles (Nombre, PID, Tiempo de CPU en ms, Memoria en KB) | `sys_get_process_info` | Bajo demanda al enviar formulario (`/api/proceso?pid=N`) |

---

## 7. Transporte de Datos — Implementación de SSE en C

### 7.1 Protocolo Server-Sent Events

El daemon expone el endpoint `GET /api/eventos` manteniendo la conexión HTTP abierta:

```http
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
Access-Control-Allow-Origin: *

```

Cada vez que el hilo de eventos de kernel despierta y parsea una notificación, emite hacia todos los clientes SSE conectados:

```text
data: {"archivo_origen":"/home/kwlopez/so2_logs/app.log","palabra_detectada":"FAIL","contenido":"Database connection timeout","timestamp":"2026-10-05 22:15:00"}

```

En el frontend JavaScript, la suscripción se realiza de forma nativa sin librerías:

```javascript
const eventSource = new EventSource('/api/eventos');
eventSource.onmessage = (event) => {
    const nuevoEvento = JSON.parse(event.data);
    agregarEventoAlPanel(nuevoEvento);
    mostrarAlertaVisual(nuevoEvento);
};
```

### 7.2 Manejo de Concurrencia de Red en C

Para que mantener clientes SSE conectados no bloquee las peticiones REST (`/api/estado` y `/api/proceso`), el servidor HTTP implementa:
- **Modelo Multiplexado I/O (`select` o `poll`):** Un hilo maneja el socket de escucha y los clientes conectados.
- **Lista de Descriptores SSE:** Los clientes que solicitan `/api/eventos` se registran en una lista de suscriptores activos. Si un cliente se desconecta, se remueve limpiamente.
- **Peticiones REST:** Se atienden de inmediato y se cierra la conexión (`Connection: close`).

---

## 8. Convenciones y Buenas Prácticas del Proyecto

- **Código Limpio:** Código en C conciso y estructurado, sin comentarios innecesarios en la lógica obvia; las justificaciones residen en el manual técnico.
- **Ubicación de Logs:** Los logs vigilados deben residir en rutas de usuario (ej. `~/so2_logs/`), **nunca en `/tmp/`**, para evitar que la restricción del kernel `fs.protected_regular` impida la escritura al hilo de kernel.
- **Rutas Absolutas:** Siempre invocar `sys_start_log_watch` con rutas absolutas canónicas.
- **Compilación en C:** Flags estándar de compilación: `gcc -Wall -Wextra -pthread -O2`.

---

## 9. Matriz de Riesgos y Mitigaciones

| Riesgo Técnico | Impacto | Estrategia de Mitigación |
|---|---|---|
| **Truncamiento de evento IPC** | Eventos con líneas largas exceden 256 bytes | Formato delimitado por pipes situando el campo `linea` al final; el log centralizado en disco almacena la línea completa. |
| **Llenado de cola IPC (128 msgs)** | Pérdida de eventos con retorno `-EAGAIN` | El hilo de eventos del daemon consume los mensajes de inmediato tan pronto son encolados. |
| **Bloqueo del servidor HTTP por cliente SSE** | Dashboard no puede consultar `/api/estado` | Multiplexación con `poll()`/`select()` para separar clientes SSE persistentes de peticiones HTTP efímeras. |
| **PID no existente en consulta** | Fallo en la llamada a `sys_get_process_info` | Comprobar retorno `-ESRCH`, responder HTTP 404 estructurado y no tumbar el servidor. |
| **Cierre inesperado del daemon** | Hilo de kernel `log_watch` huérfano | Capturar señales `SIGINT` y `SIGTERM` para ejecutar `sys_stop_log_watch(watch_id)` antes de salir. |

---

## 10. Plan de Tareas de Implementación (SDD / ODD)

### Fase 1: Kernel y Syscalls (Capa 1)
- [x] **Tarea 1.1:** Portar `mi_syscall.c` al árbol 6.12.69 con prototipos en `include/linux/syscalls.h` y registrar syscalls 552 y 553 en `syscall_64.tbl`.
- [x] **Tarea 1.2:** Exponer `ipc_channel_enqueue_kernel` en `kernel/ipc_channel.c` y vincular `log_watch.c` para disparar notificación `EVT|...`.
- [x] **Tarea 1.3:** Compilar/generar la imagen del kernel modificado (`bzImage ready #3`) con las 6 syscalls integradas y verificadas en `System.map`.
- [x] **Tarea 1.4:** Instalar el kernel en `/boot` (`instalar_kernel.sh`), reiniciar en `6.12.69-so2-202100106` y validar con `./test_fase1`.

### Fase 2: Daemon Orquestador en C (Capa 2)
- [x] **Tarea 2.1:** Implementar módulo de métricas del sistema: lectura de `/proc/meminfo` y escaneo de `/proc` para construir la lista de procesos activos.
- [x] **Tarea 2.2:** Implementar llamada a `sys_get_processes_state` para métricas globales de estados.
- [x] **Tarea 2.3:** Implementar hilo consumidor de eventos bloqueado en `sys_ipc_channel_receive`, parseador del mensaje `EVT|...` y buffer circular de eventos.
- [x] **Tarea 2.4:** Implementar servidor HTTP en C con sockets TCP y multiplexación: endpoints `/api/estado`, `/api/proceso?pid=N` y `/api/eventos` (SSE).
- [x] **Tarea 2.5:** Integrar manejo de señales (`SIGINT`, `SIGTERM`), inicio de monitoreo con `sys_start_log_watch` y parada segura con `sys_stop_log_watch`.

### Fase 3: Dashboard Web en Tiempo Real (Capa 3)
- [x] **Tarea 3.1:** Crear estructura HTML/CSS moderna (modo oscuro, dashboard responsivo).
- [x] **Tarea 3.2:** Implementar Gráfico 1 (Pie/Donut de uso de memoria) y Gráfico 2 (Línea temporal física vs swap) con Chart.js o SVG nativo.
- [x] **Tarea 3.3:** Implementar Tabla de Procesos activos con buscador por nombre/PID y tarjetas de resumen global de estados.
- [x] **Tarea 3.4:** Implementar Panel de Eventos de Seguridad conectado a `EventSource('/api/eventos')`, renderizando nuevas alertas en tiempo real.
- [x] **Tarea 3.5:** Implementar formulario de consulta puntual por PID consumiendo `/api/proceso?pid=N`.

### Fase 4: Pruebas de Integración y Manual Técnico
- [x] **Tarea 4.1:** Prueba de extremo a extremo: inyectar palabras clave en logs vigilados y evidenciar notificación instantánea en el dashboard sin refrescar la página.
- [x] **Tarea 4.2:** Medición de uso de CPU del daemon en reposo (evidenciar `%CPU = 0.0` y estado `S`).
- [x] **Tarea 4.3:** Redactar Manual Técnico en Markdown cubriendo arquitectura, rol de cada syscall, justificación del mecanismo event-driven frente a polling, y reporte de 3 errores resueltos.
