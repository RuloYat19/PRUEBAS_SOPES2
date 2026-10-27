# Integración de Syscalls: Sistema de Monitoreo y Seguridad con Dashboard en Tiempo Real

---

## 2. Enunciado de la Práctica

En esta práctica el estudiante integrará el trabajo realizado en las **Prácticas 2 y 3**, reutilizando las syscalls previamente desarrolladas para construir un sistema completo de monitoreo del sistema operativo con visualización en tiempo real.

El objetivo es demostrar cómo mecanismos que se construyeron de forma aislada (consulta de procesos, comunicación entre procesos, monitoreo de eventos) pueden combinarse para formar una herramienta coherente de instrumentación — el mismo tipo de integración que se espera en el proyecto final del curso.

Para lograrlo, el estudiante deberá:

- Reutilizar `sys_get_process_info` / `sys_get_processes_state` para poblar una tabla de procesos consultable.
- **Rediseñar** el uso del canal IPC (`sys_ipc_channel_send` / `receive`) para que el daemon de monitoreo reaccione a eventos en vez de hacer polling constante.
- Reutilizar `sys_start_log_watch` / `sys_stop_log_watch` para alimentar un panel de "eventos de seguridad" en el dashboard.
- Construir un programa intermedio en C que orqueste las syscalls anteriores, lea métricas de memoria desde `/proc/meminfo`, y transforme toda la información a JSON.
- Construir un dashboard web que visualice todo en tiempo real.

---

### 2.1 Descripción del problema a resolver

Las herramientas de monitoreo e instrumentación reales (como los agentes de seguridad EDR o los dashboards de observabilidad) no dependen de un solo mecanismo del kernel: combinan múltiples fuentes de información (estado de procesos, eventos del sistema, métricas de recursos) y las presentan de forma unificada para que un analista pueda tomar decisiones.

Hasta ahora, cada práctica construyó una pieza aislada:

- **Práctica 2:** consulta de información de procesos bajo demanda.
- **Práctica 3:** comunicación entre procesos y monitoreo autónomo de eventos.

En esta práctica, el estudiante deberá resolver el problema de **integrar estas piezas en un sistema unificado**, demostrando que comprende no sólo cómo construir cada mecanismo por separado, sino cómo coordinarlos entre sí.

---

### 2.2 Alcance de la práctica

El sistema estará compuesto por **tres capas**:

| Capa | Ubicación | Responsabilidad |
|---|---|---|
| **1. Kernel Modificado** | Máquina Virtual | Contiene las syscalls ya desarrolladas en P2 y P3. **No se requiere implementar ninguna syscall nueva.** |
| **2. Backend / Programa Intermedio** | Máquina Virtual | Actúa como orquestador. Invoca las syscalls, lee `/proc/meminfo` para las métricas de memoria, transforma la información a JSON, y la expone vía HTTP/WebSocket. |
| **3. Dashboard Web** | Puede ejecutarse en el host | Interfaz visual que consume la información del backend y la muestra en tiempo real. |

---

#### Figura 1: Arquitectura del sistema integrado

> **Archivo Draw.io:** [`arquitectura.drawio`](arquitectura.drawio)
> **Imagen original:** [`imgs/arquitectura.png`](imgs/arquitectura.png)

```text
┌───────────────────────────────────────────────────────────────────────┐
│ KERNEL SPACE (Maquina Virtual)                                        │
│                                        ┌─────────────────────┐        │
│                                        │ Hilo de kernel:     │        │
│                                        │ log_watch           │        │
│                                        │ (kthread_run)       │        │
│                                        └──────────┬──────────┘        │
│                                                   │ notifica evento   │
│                                                   ▼                   │
│  ┌──────────────┐ ┌───────────────┐ ┌───────┐ ┌──────────────────┐    │
│  │sys_get_      │ │sys_get_       │ │/proc/ │ │ Canal IPC        │    │
│  │process_info  │ │processes_     │ │meminfo│ │ sys_ipc_channel_ │    │
│  │(pid)         │ │state()        │ │       │ │ send / receive   │    │
│  └──────┬───────┘ └───────┬───────┘ └───┬───┘ └────────┬─────────┘    │
└─────────┼─────────────────┼─────────────┼──────────────┼──────────────┘
          │ consulta        │ polling     │ lectura      │ notificacion
          │ bajo demanda    │             │ periodica    │ bloqueante
          ▼                 ▼             ▼              ▼
     ┌────────────────────────────────────────────────────────────┐
     │ USER SPACE - Daemon en C (Maquina Virtual)                 │
     │  ┌───────────────────────┐   ┌──────────────────────────┐  │
     │  │ Ciclo periodico       │   │ Bloqueado en:            │  │
     │  │ polling:              │   │ sys_ipc_channel_receive()│  │
     │  │ - invoca sys_get_     │   │ (espera real, sin        │  │
     │  │   processes_state     │   │  polling)                │  │
     │  │ - lee /proc/meminfo   │   │                          │  │
     │  └───────────┬───────────┘   └────────────┬─────────────┘  │
     │              └───────────┬────────────────┘                │
     │                          ▼                                 │
     │            ┌──────────────────────────┐                    │
     │            │ Generador de JSON        │                    │
     │            │ memoria + procesos +     │                    │
     │            │ eventos_seguridad        │                    │
     │            └────────────┬─────────────┘                    │
     │                         ▼                                  │
     │            ┌──────────────────────────┐                    │
     │            │ Servidor HTTP / WebSocket│                    │
     │            └────────────┬─────────────┘                    │
     └─────────────────────────┼──────────────────────────────────┘
                               │ JSON tiempo real
                               ▼
  ┌────────────────────────────────────────────────────────────────────┐
  │ DASHBOARD WEB (Navegador - Host)                                   │
  │ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────┐ ┌──────────┐  │
  │ │3. Tabla  │ │1.Desglose│ │2.Evolucion│ │4. Panel   │ │5.Consulta│  │
  │ │de proce- │ │de memoria│ │ memoria   │ │Eventos de │ │ por PID  │  │
  │ │sos activos││  (pie)   │ │ (lineas)  │ │Seguridad  │ │          │  │
  │ │          │ │          │ │           │ │(event-    │ │          │  │
  │ │          │ │          │ │           │ │driven,IPC)│ │          │  │
  │ └──────────┘ └──────────┘ └──────────┘ └───────────┘ └──────────┘  │
  └────────────────────────────────────────────────────────────────────┘
```

---

#### 2.2.1 Syscalls a integrar

> **Todas ya desarrolladas — no hay syscalls nuevas.**

| Syscall | Origen | Función |
|---|---|---|
| `sys_get_process_info(pid)` | Práctica 2 | Información detallada de un proceso. |
| `sys_get_processes_state()` | Práctica 2 | Estado de todos los procesos activos. |
| `sys_ipc_channel_send(mensaje)` / `sys_ipc_channel_receive()` | Práctica 3 | Canal de comunicación entre procesos. |
| `sys_start_log_watch(rutas, log_central, palabra_clave)` / `sys_stop_log_watch(id)` | Práctica 3 | Monitoreo de eventos vía hilo de kernel. |

---

#### 2.2.2 Rol de cada mecanismo dentro del sistema

Cada syscall reutilizada debe tener una función clara dentro del dashboard, **no simplemente estar presente**:

**`sys_get_process_info` / `sys_get_processes_state`**
Alimentan la tabla de procesos activos del dashboard, con la posibilidad de consultar el detalle de un proceso específico por PID.

**`sys_ipc_channel_send` / `receive` — uso rediseñado**
El daemon **no debe hacer polling constante** revisando si hay eventos de seguridad. En su lugar, debe existir un mecanismo (puede ser el propio hilo de `log_watch` extendido, o lógica adicional) que envíe un mensaje por el canal IPC cuando `log_watch` detecte la palabra clave en algún archivo.

El daemon en espacio de usuario debe quedar **bloqueado** en `sys_ipc_channel_receive` esperando esas notificaciones, y solo entonces actualizar el panel de eventos del dashboard.

Esto demuestra un **patrón productor-consumidor real** entre kernel y espacio de usuario, evitando desperdiciar CPU revisando constantemente si hay novedades.

**`sys_start_log_watch` / `sys_stop_log_watch`**
Siguen encargándose de vigilar los archivos de log; ahora, además, **disparan la notificación IPC** descrita arriba cuando detectan una coincidencia.

**Métricas de memoria (`/proc/meminfo`)**
El daemon lee este archivo (interfaz ya expuesta por el kernel, sin necesidad de código de kernel adicional) para poblar las gráficas de memoria y páginas del dashboard.

---

#### 2.2.3 Programa intermedio (Daemon en C)

El programa intermedio debe:

1. Iniciar el monitoreo de eventos (`log_watch`) y quedar a la espera de notificaciones vía el canal IPC, en vez de hacer polling continuo del panel de seguridad.
2. Invocar `sys_get_processes_state` de forma periódica (esto **sí** puede ser polling, ya que es una consulta de estado, no un evento) para mantener la tabla de procesos actualizada.
3. Leer `/proc/meminfo` periódicamente para las métricas de memoria.
4. Estructurar toda la información en JSON.
5. Exponer los datos al dashboard vía HTTP o WebSocket.
6. Ejecutarse de forma continua, como un servicio/daemon.
7. Manejar errores de forma adecuada (ej. si una syscall falla, no debe caerse todo el daemon).

**Estructura mínima del JSON generado:**

```json
{
  "memoria_usada": 0,
  "memoria_libre": 0,
  "memoria_cache": 0,
  "swap_usada": 0,
  "procesos": [
    { "nombre": "", "pid": 0, "estado": "" }
  ],
  "eventos_seguridad": [
    {
      "archivo_origen": "",
      "palabra_detectada": "",
      "contenido": "",
      "timestamp": ""
    }
  ]
}
```

---

#### 2.2.4 Dashboard web (Frontend)

Debe incluir, como mínimo, estas **cinco visualizaciones**:

| Nº | Visualización | Tipo | Fuente de datos |
|---|---|---|---|
| 1 | Desglose de uso de memoria | Gráfico de pie (usada, libre, caché) | `/proc/meminfo` |
| 2 | Evolución del uso de memoria en el tiempo | Gráfico de líneas (memoria física vs. swap) | `/proc/meminfo` |
| 3 | Tabla de procesos activos | Tabla con nombre, PID y estado | `sys_get_processes_state` |
| 4 | Panel de Eventos de Seguridad | Tabla en tiempo real con los hallazgos más recientes | Mecanismo de notificación IPC (ver 2.2.2) |
| 5 | Campo de consulta por PID | Entrada de texto + detalle del proceso | `sys_get_process_info` |

El dashboard debe actualizarse **en tiempo real** (intervalos regulares o mediante WebSocket), y el panel de eventos de seguridad en particular debe reflejar el patrón **event-driven** descrito en 2.2.2: no debe depender de refrescar la página ni de polling ciego.

---

### 2.3 Requerimientos técnicos

- **Kernel Linux:** Versión **6.12.69 Long Term (LT)**.
- **Lenguaje C:** Para la implementación de la syscall y el programa de usuario.
- **Distribución:** Linux Mint u otra distribución basada en Debian.
- **Editor de texto:** VS Code, Vim, Nano.
- **Terminal de Linux:** Para compilación, pruebas y depuración.
- **Opcional:** `gdb`, `strace`, `dmesg` para depuración avanzada.

---

## 3. Entregables

| Tipo | Descripción |
|---|---|
| **Repositorio en GitLab** | Enlace a la rama (`Practica_4_2S2026`, nombre = carné) con el código del kernel modificado, todas las syscalls (reutilizadas), el programa intermedio y el dashboard. Debe incluir instrucciones claras para compilar y ejecutar todo el sistema. |
| **Ajustes a Syscalls existentes** | Evidencia de la correcta reutilización/adaptación de las syscalls de P2 y P3, especialmente el rediseño del canal IPC como mecanismo de notificación desde `log_watch`. |
| **Programa de usuario (Daemon)** | Código en C del daemon, incluyendo el ejecutable compilado, que orquesta las syscalls, lee `/proc/meminfo` y genera el JSON. |
| **Dashboard Web** | Código del dashboard con las 5 visualizaciones requeridas. |
| **Manual Técnico** | Documento en Markdown que describa: arquitectura general del sistema, qué rol cumple cada syscall, el diseño del mecanismo de notificación vía IPC (por qué se diseñó así y no con polling puro), y al menos **3 errores** encontrados durante la integración con su solución. |

---

## 4. Checklist de Validación

### 4.1 Requisitos indispensables

- [ ] **Cumplimiento de la tecnología establecida:** La práctica se desarrolla en un sistema GNU/Linux basado en Debian, en VM o instalación nativa.
- [ ] **Integridad de la entrega:** Se entrega el enlace a la rama del repositorio GitLab con el código completo y la documentación.
- [ ] **Compilación funcional:** El kernel modificado arranca (boot) sin errores.
- [ ] **Kernel modificado:** Se utiliza el mismo kernel compilado desde la Práctica 1.
- [ ] **Sin polling puro en eventos de seguridad:** El panel de "Eventos de Seguridad" se alimenta vía el mecanismo de notificación IPC descrito en 2.2.2, no mediante refresco ciego constante.

---

### 4.2 Reutilización de `sys_get_process_info` / `sys_get_processes_state`

- [ ] `sys_get_process_info` funciona correctamente dentro del sistema integrado.
- [ ] `sys_get_processes_state` funciona correctamente dentro del sistema integrado.
- [ ] Ambas alimentan la tabla de procesos del dashboard.
- [ ] La consulta por PID devuelve el detalle del proceso solicitado.

---

### 4.3 Canal IPC como mecanismo de notificación

- [ ] El daemon se bloquea esperando eventos reales provenientes de `log_watch`.
- [ ] No hay polling ciego disfrazado de IPC: el bloqueo es real.
- [ ] `log_watch` dispara la notificación IPC al detectar una coincidencia de la palabra clave.
- [ ] El daemon reacciona correctamente al recibir la notificación y actualiza el panel de eventos.
- [ ] Se puede evidenciar el consumo de CPU del daemon en espera (sin espera activa).

---

### 4.4 Daemon funcional

- [ ] Orquesta todas las syscalls (P2 y P3).
- [ ] Lee `/proc/meminfo` correctamente.
- [ ] Genera el JSON completo con todos los campos requeridos.
- [ ] Expone los datos vía HTTP o WebSocket.
- [ ] Se ejecuta de forma continua como servicio/daemon.
- [ ] Maneja errores adecuadamente: si una syscall falla, el daemon no se cae.

---

### 4.5 Dashboard funcional

- [ ] **Visualización 1:** Desglose de uso de memoria (gráfico de pie).
- [ ] **Visualización 2:** Evolución del uso de memoria en el tiempo (gráfico de líneas, física vs. swap).
- [ ] **Visualización 3:** Tabla de procesos activos (nombre, PID, estado).
- [ ] **Visualización 4:** Panel de Eventos de Seguridad en tiempo real (event-driven).
- [ ] **Visualización 5:** Campo de consulta por PID con detalle del proceso.
- [ ] Todas se actualizan correctamente en tiempo real.

---

### 4.6 Integración completa

- [ ] El sistema completo funciona de punta a punta.
- [ ] No requiere pasos manuales adicionales entre capas.

---

### 4.7 Documentación técnica

- [ ] Describe la arquitectura general del sistema.
- [ ] Explica qué rol cumple cada syscall dentro del sistema.
- [ ] Explica el diseño del mecanismo de notificación vía IPC y por qué no se usó polling puro.
- [ ] Documenta al menos **3 errores** encontrados durante la integración, con su solución.
- [ ] Incluye instrucciones claras para compilar y ejecutar todo el sistema.

---

### 4.8 Defensa oral

- [ ] **Pregunta 1 (arquitectura):** Explica cómo se integran las syscalls y por qué.
- [ ] **Pregunta 2 (IPC/notificación):** Explica las ventajas del mecanismo de notificación sobre el polling puro.
