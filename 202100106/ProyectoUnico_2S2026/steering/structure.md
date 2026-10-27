# Project Structure — Proyecto Único

Organización de carpetas y archivos. Los nombres de las carpetas de entrega (`Kernel`, `ProgramaIntermedio`, `ProgramaIntermedio/ebpf`, `Dashboard`) **los fija el enunciado** y no se cambian.

---

## 1. Contexto del repositorio

```text
mapa/                               # repositorio (rama 202100106)
├── Practica_1_2S2026/              # syscall básica + programa de prueba
├── Practica_2_2S2026/              # get_process_info / get_processes_state
├── Practica_3_2S2026/              # ipc_channel + log_watch (kthread)
├── Practica_4_2S2026/              # integración: kernel overlay + daemon + dashboard
└── ProyectoUnico_2S2026/           # ← este proyecto (todo lo entregable vive aquí)
```

Las prácticas anteriores son **referencia de solo lectura**. Se copia y adapta su código dentro de `ProyectoUnico_2S2026/`; nunca se edita una práctica ya entregada.

| De | Hacia | Qué se reutiliza |
|---|---|---|
| `Practica_4_2S2026/linux-6.12.69/` | `Kernel/` | Overlay con las 6 syscalls (548–553). |
| `Practica_4_2S2026/daemon/` | `ProgramaIntermedio/` | `main.c`, `metricas.c`, `eventos.c`, `http.c`, `json.c`, `comun.h`. |
| `Practica_4_2S2026/frontend/` | `Dashboard/` | `index.html`, `css/styles.css`, `js/app.js`, `js/charts.js`. |
| `Practica_4_2S2026/*.sh` | `scripts/` | `preparar_kernel.sh` (con BTF habilitado), `instalar_kernel.sh`, `demo.sh`, `pruebas_integracion.sh`. |

## 2. Árbol objetivo

```text
ProyectoUnico_2S2026/
├── README.md                       # portada: qué es, cómo levantarlo, enlaces a manuales
├── AGENTS.md                       # reglas para el agente implementador
├── enunciado.md                    # enunciado oficial en Markdown (fuente de verdad)
├── steering/                       # contexto permanente del proyecto (este directorio)
│   ├── product.md
│   ├── tech.md
│   └── structure.md
├── specs/                          # un directorio por feature (flujo SDD)
│   └── NN-nombre/
│       ├── requirements.md         # historias + criterios de aceptación (o bugfix.md)
│       ├── design.md               # arquitectura, secuencias, errores, pruebas
│       ├── tasks.md                # tareas con casillas y evidencia
│       └── review.md               # hallazgos del revisor (cuando existan)
│
├── Kernel/                         # SOLO archivos modificados, con la ruta original del kernel
│   ├── arch/x86/entry/syscalls/syscall_64.tbl
│   ├── include/linux/syscalls.h
│   ├── include/linux/ipc_channel.h
│   └── kernel/
│       ├── Makefile
│       ├── mi_syscall.c            # get_process_info, get_processes_state (P2)
│       ├── ipc_channel.c           # ipc_channel_send/receive + enqueue interno (P3/P4)
│       └── log_watch.c             # start/stop_log_watch (P3/P4)
│
├── ProgramaIntermedio/             # daemon en C
│   ├── Makefile                    # compila ebpf/ y luego el daemon
│   ├── daemon_monitor              # ejecutable compilado (se entrega)
│   ├── daemon.conf                 # puerto, intervalos, ruta vigilada, logwatch inicial
│   ├── blacklist.json              # base de firmas inicial
│   ├── comun.h                     # números de syscall, structs compartidas, estado global
│   ├── rutas.h                     # apertura segura sin enlaces con openat2
│   ├── main.c                      # args, daemonización, señales, arranque/parada de hilos
│   ├── config.c                    # lectura de daemon.conf
│   ├── metricas.c                  # meminfo, vmstat (deltas), derivadas
│   ├── procesos.c                  # /proc, Top 20 RSS, kill con guardas
│   ├── sensor.c                    # libbpf (load/attach/detach), ring buffer, mapas
│   ├── analisis.c                  # SHA-256 (EVP), estado de archivos, cuarentena
│   ├── blacklist.c                 # parseo/validación JSON + tabla de firmas
│   ├── rutas.c                     # helper abrir_sin_enlaces con openat2
│   ├── logwatch.c                  # registro y validación de hasta 16 logwatch
│   ├── ipc.c                       # hilo IPC, sys_ipc_channel_receive
│   ├── alertas.c                   # cola de alertas con severidad y tipo
│   ├── auth.c                      # PAM, grupos, sesiones y permisos por endpoint
│   ├── http.c                      # servidor HTTP + SSE con poll(), ruteo
│   ├── peticion.c                  # parser HTTP, headers, cookies, query string
│   ├── json.c                      # serializador JSON con cJSON
│   ├── pruebas/                    # pruebas unitarias en C (make pruebas)
│   │   ├── test_alertas.c
│   │   ├── test_analisis.c
│   │   ├── test_auth.c
│   │   ├── test_blacklist.c
│   │   ├── test_config.c
│   │   ├── test_logwatch.c
│   │   ├── test_metricas.c
│   │   ├── test_peticion.c
│   │   └── test_procesos.c
│   ├── lib/
│   │   ├── cJSON.c                 # parser JSON vendorizado (MIT)
│   │   └── cJSON.h
│   └── ebpf/
│       ├── Makefile                # vmlinux.h → sensor.bpf.o → sensor.skel.h
│       ├── sensor.bpf.c            # filtro por ruta + descarte del TGID del daemon
│       ├── sensor.h                # struct evento_archivo compartida con el daemon
│       └── filtro_ruta.h           # macro de filtrado ruta_en_directorio
│
├── Dashboard/                      # HTML + CSS + JS, servido por el daemon
│   ├── index.html                  # dashboard principal
│   ├── login.html                  # inicio de sesión PAM
│   ├── css/styles.css
│   └── js/
│       ├── alertas.js              # panel de alertas por severidad
│       ├── antivirus.js            # amenazas, panel cuarentena, base de firmas
│       ├── api.js                  # fetch + EventSource, manejo de 401/403
│       ├── app.js                  # inicialización y enrutamiento visual
│       ├── archivos.js             # archivos analizados, ruta vigilada
│       ├── auth.js                 # login/logout, adaptación por rol
│       ├── charts.js               # gráficas de métricas en canvas
│       ├── logwatch.js             # crear/listar/finalizar logwatch
│       ├── metricas.js             # 4 gráficas en tiempo real
│       └── procesos.js             # top 20, búsqueda por PID y kill
│
├── scripts/
│   ├── correr_pruebas.sh           # ejecutor maestro de las 9 suites de pruebas
│   ├── preparar_kernel.sh          # descarga + overlay Kernel/ + config eBPF/BTF + make
│   ├── instalar_kernel.sh
│   ├── verificar_kernel.sh         # post-arranque: versión, BTF, config eBPF, syscalls
│   ├── setup_usuarios.sh           # grupos admin_user/common_user, /etc/pam.d/so2_auth
│   ├── demo.sh                     # arranque y escenarios de demostración
│   ├── demo_antivirus.sh           # inyección de muestras benignas y malware
│   ├── pruebas_auth.sh             # suite F01
│   ├── pruebas_alertas.sh          # suite F02
│   ├── pruebas_metricas.sh         # suite F03
│   ├── pruebas_procesos.sh         # suite F04
│   ├── pruebas_sensor.sh           # suite F05
│   ├── pruebas_antivirus.sh        # suite F06
│   ├── pruebas_logwatch.sh         # suite F07
│   ├── pruebas_integracion.sh      # suite de integración
│   ├── probar_archivos_prueba.sh   # validación con muestras de archivos_prueba
│   ├── usuarios_prueba.sh          # helper de creación/borrado de usuarios temporales
│   └── test_syscalls.c             # validación de las 6 syscalls
│
├── archivos_prueba/                # muestras de prueba y blacklist externa
│
├── docs/
│   ├── Manual_Tecnico.md
│   └── Manual_Usuario.md
│
└── imgs/                           # capturas numeradas para los manuales (NN-descripcion.png)
```

Los módulos `.c` listados son la división propuesta; se pueden fusionar o dividir si un archivo crece demasiado, pero sin mezclar responsabilidades de hilos distintos en un mismo módulo.

## 3. Responsabilidad por capa

| Carpeta | Capa | Regla clave |
|---|---|---|
| `Kernel/` | Kernel | Respetar la ruta original del árbol 6.12.69; solo archivos tocados. Ninguna lógica de detección aquí. |
| `ProgramaIntermedio/ebpf/` | Kernel (sensor) | Solo filtro por ruta y PID del daemon. Compilado con Clang. |
| `ProgramaIntermedio/` | Usuario | Toda la detección, severidad, autorización y respuesta. |
| `Dashboard/` | Presentación | Solo consume la API; nunca decide permisos. |
| `scripts/` | Operación | Reproducibilidad de compilación, instalación y demo. |
| `docs/` + `imgs/` | Entrega | Manuales en Markdown con capturas para admin y usuario común. |

## 4. Convenciones de nombres

- **Archivos C y JS:** `snake_case`/minúsculas en español (`metricas.c`, `alertas.js`).
- **Funciones C:** `modulo_accion` (`sensor_adjuntar`, `blacklist_cargar`, `alertas_agregar`).
- **Constantes y números de syscall:** `MAYUSCULAS` (`SYS_GET_PROCESS_INFO`).
- **Claves JSON:** `snake_case` en español (`memoria_usada`, `palabra_detectada`), igual que P4.
- **Capturas:** `imgs/NN-descripcion-corta.png`, numeradas en el orden en que aparecen en los manuales.
- **Archivos de eBPF:** `*.bpf.c` para el programa; `*.skel.h` y `vmlinux.h` son generados.

## 5. Generados y no versionados

| Archivo | ¿Se versiona? | Motivo |
|---|---|---|
| `ProgramaIntermedio/daemon_monitor` | **Sí** | El enunciado pide incluir el ejecutable. |
| `*.o`, `ebpf/*.bpf.o` | No | Ya cubierto por `.gitignore` (`*.o`). |
| `ebpf/vmlinux.h` | No | Depende del kernel instalado; se regenera con `bpftool`. |
| `ebpf/sensor.skel.h` | No | Generado por `bpftool gen skeleton`. |
| `so2_build_workspace/`, `*.log` | No | Ya excluido en `.gitignore`. |

Agregar al `.gitignore` raíz: `**/ebpf/vmlinux.h` y `**/ebpf/*.skel.h`.

## 6. Dónde va cada cosa nueva

- ¿Nuevo endpoint? Ruteo en `http.c`, permiso en `auth.c`, lógica en el módulo de su dominio.
- ¿Nueva alerta? Constante de tipo/severidad en `comun.h`, generación vía `alertas_agregar()` desde el módulo que detecta el evento.
- ¿Nuevo campo en el evento eBPF? Cambiar `ebpf/sensor.h` (compartido) y ambos lados a la vez.
- ¿Nueva vista? Sección en `index.html` + módulo propio en `Dashboard/js/`.
- ¿Decisión técnica o error resuelto? Documentarlo en `docs/Manual_Tecnico.md`, no como comentario largo en el código.
