# Product Overview — Proyecto Único

**Sistema Integral de Monitoreo y Seguridad del Sistema Operativo**
Carné: 202100106 — Sistemas Operativos 2, 2S2026
Fuente de verdad de requisitos: [`../enunciado.md`](../enunciado.md). Si este documento y el enunciado discrepan, gana el enunciado.

---

## 1. Propósito

Plataforma de observabilidad y seguridad que trabaja desde el kernel hacia arriba. Combina en un solo sistema lo que hoy hacen herramientas separadas:

- **Monitor de recursos** (estilo `top`/`htop`): memoria, swap, páginas activas/inactivas, fallos de página y top 20 de procesos por RSS.
- **Motor antivirus simulado**: un sensor eBPF vigila un directorio, el daemon calcula SHA-256 de cada archivo creado/modificado y lo compara contra `blacklist.json`; si coincide, alerta `HIGH` y cuarentena (`chmod 0000`).
- **Detector de integridad**: cambios de hash o timestamp en archivos benignos generan alerta `LOW`.
- **Vigilancia de logs**: los `log_watch` del kernel (Práctica 3) notifican palabras clave por el canal IPC, sin polling.
- **Control de acceso**: login contra PAM y permisos por grupo Unix (`admin_user` / `common_user`), aplicados en el daemon en C.

El proyecto **reutiliza** las syscalls de las Prácticas 2, 3 y 4 (no se crean syscalls nuevas) y evoluciona el daemon y el dashboard de la Práctica 4.

## 2. Usuarios y roles

| Rol | Grupo Unix | Quién es | Qué necesita |
|---|---|---|---|
| **Administrador** | `admin_user` | Operador de seguridad del equipo | Ver todo y **actuar**: activar/desactivar sensores, configurar ruta vigilada, subir blacklist, cuarentena manual, matar procesos, gestionar logwatch. |
| **Usuario común** | `common_user` | Observador / auditor | Solo **lectura**: métricas, procesos, alertas y archivos analizados. |
| *(sin grupo)* | — | Cualquier otra cuenta del sistema | **Acceso denegado** aunque la contraseña PAM sea correcta. |

Reglas de rol:

- Pertenecer a ambos grupos ⇒ se trata como **administrador**.
- Los usuarios se crean en tiempo de ejecución desde la consola (`useradd` + `usermod -aG admin_user|common_user`); el daemon no necesita reiniciarse.
- La autorización la decide **el daemon en C** en cada petición. El frontend solo oculta controles; nunca es la barrera de seguridad.

### Matriz de permisos

| Acción | Admin | Común |
|---|:---:|:---:|
| Ver métricas del sistema | ✔ | ✔ |
| Ver procesos activos (top 20) | ✔ | ✔ |
| Ver alertas | ✔ | ✔ |
| Ver listado de archivos analizados | ✔ | ✔ |
| Activar/desactivar escaneo de archivos (attach/detach eBPF) | ✔ | ✘ |
| Activar/desactivar métricas del sistema | ✔ | ✘ |
| Activar/desactivar escaneo de procesos | ✔ | ✘ |
| Actualizar blacklist (subir JSON) | ✔ | ✘ |
| Configurar ruta a monitorear | ✔ | ✘ |
| Consultar proceso por PID (`sys_get_process_info`) | ✔ | ✘ |
| Matar proceso por PID (`kill`, con confirmación) | ✔ | ✘ |
| Poner archivos en cuarentena / ver cuarentena | ✔ | ✘ |
| Crear y finalizar logwatch | ✔ | ✘ |

## 3. Capacidades del producto

### 3.1 Métricas del sistema (Hilo 1)

- Lectura periódica de `/proc/meminfo`: `MemTotal`, `MemFree`, `MemAvailable`, `Buffers`, `Cached`, `Active`, `Inactive`, `SwapTotal`, `SwapFree`.
- Lectura de `/proc/vmstat`: `pgfault` y `pgmajfault` con **delta entre muestras**.
- Gráficas en el dashboard:
  1. Páginas activas vs. inactivas
  2. Memoria y swap en el tiempo
  3. Libre vs. caché vs. utilizada
  4. `pgfault` vs. `pgmajfault`

### 3.2 Procesos (Hilo 1)

- `sys_get_processes_state` como base del listado; RSS por proceso complementado desde `/proc/[pid]` cuando la syscall no la trae.
- Top 20 por memoria calculado **en espacio de usuario**: PID, nombre, % de memoria.
- Consulta puntual por PID con `sys_get_process_info` (solo admin).
- Terminar proceso con `kill(2)`. **Prohibido**: PID 1, el propio daemon y hilos del kernel.

### 3.3 Sensor de archivos y antivirus (Hilo 2)

- El eBPF **solo** filtra por ruta (archivos directamente dentro del directorio configurado, sin subdirectorios) e ignora el PID del daemon; envía `{ruta, acción, pid}` por Ring Buffer.
- El daemon: SHA-256 (OpenSSL EVP) → compara contra la blacklist en memoria → clasifica → actúa.
- Estado por archivo: **benigno** / **infectado**, con timestamp de última modificación.
- Cuarentena automática (hash malicioso) y manual (admin) = `chmod 0000`.
- Vista de amenaza: nombre de la firma, archivo afectado, descripción.

### 3.4 Logwatch e IPC (Hilo 3)

- Al arrancar, el daemon lanza `sys_start_log_watch` con su archivo de configuración; al terminar, `sys_stop_log_watch`.
- Un hilo dedicado queda **bloqueado** en `sys_ipc_channel_receive` (cero polling).
- El admin crea logwatch nuevos (ruta central + rutas a vigilar + palabra clave) y finaliza cualquiera por ID desde una tabla.

### 3.5 Alertas

Cada alerta lleva: tipo (`archivos`, `sistema`, `sesion`, `logs`), motivo, descripción, severidad y fecha/hora. Se muestran **más recientes primero** y con color por severidad.

| Severidad | Disparadores | Color |
|---|---|---|
| **LOW** (informativo) | Archivo modificado · Monitoreo activado · Login exitoso | Azul/verde |
| **MEDIUM** (advertencia) | Monitoreo desactivado · Login fallido · Palabra clave en log · Blacklist actualizada · Proceso matado | Ámbar |
| **HIGH** (crítico) | Hash malicioso detectado · Archivo en cuarentena (auto o manual) | Rojo |

## 4. Fuera de alcance

- Crear syscalls nuevas (solo se reutilizan las de P2/P3/P4).
- Cualquier lógica de detección, severidad o comparación de hashes **dentro del kernel** (excepciones permitidas: filtro de ruta del eBPF y coincidencia de palabra clave de `log_watch`).
- Cuarentena real (mover/cifrar archivos): es simulada con permisos `0000`.
- Vigilancia recursiva de subdirectorios.
- Escaneo con polling del canal IPC o de la carpeta vigilada.

## 5. Criterios de éxito (rúbrica)

1. Daemon en **C**, en segundo plano, que no cae ante errores de syscalls, archivos o clientes.
2. Kernel **6.12.69 LTS** con soporte eBPF (`CONFIG_BPF`, `CONFIG_BPF_SYSCALL`, `CONFIG_BPF_EVENTS`, `CONFIG_DEBUG_INFO_BTF`, `CONFIG_FTRACE_SYSCALLS`) y las syscalls de P4 integradas.
3. **Al menos 3 hilos** (monitoreo, archivos/eBPF, IPC) con mutex en alertas, blacklist y métricas.
4. Comunicación daemon ↔ dashboard en **JSON** sobre HTTP/SSE o WebSocket.
5. Todo dentro de `ProyectoUnico_2S2026/` con las carpetas `Kernel`, `ProgramaIntermedio` (+ `ebpf/`) y `Dashboard`.
6. **Manual Técnico** y **Manual de Usuario** en Markdown, con capturas para ambos roles.

## 6. Puntos abiertos

- **Formato de `blacklist.json`:** no hay formato oficial (el enlace del enunciado se perdió). Por decisión del usuario (2026-10-21) se usa un formato propio y estricto: objeto con `nombre`, `version`, `actualizado` opcionales y `firmas` (1–5000 objetos con exactamente `sha256`, `nombre`, `descripcion`). Definición completa en `specs/06-antivirus/requirements.md` R1.
- **Rutas de logwatch desde el dashboard:** solo archivos directamente dentro del directorio de logs del daemon (`dirname` de `log_central`, por defecto `~/so2_logs`), sin enlaces, porque el kthread de `log_watch` corre como root. La falta de verificación de permisos en las syscalls 550/551 se documenta como limitación de P3 y no se corrige en el kernel. Decisiones del usuario (2026-10-21); ver `specs/07-logwatch/`.
- **Entregable "mínimo 2 hilos"** vs. "al menos 3 hilos" en el resto del enunciado: se implementan **3 o más** para cumplir ambos.
