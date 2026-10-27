# F08 — Documentación y entrega · Diseño

Requisitos: [`requirements.md`](requirements.md). Fuentes:
- el código entregado (fuente de verdad);
- `README.md` actual (secciones 1–13);
- `steering/`;
- los `requirements.md`/`design.md`/`review.md` de F00–F07 (decisiones y limitaciones);
- `enunciado.md` (Entregables y Criterios de evaluación).

---

## 1. Orden de trabajo

```text
T1 advertencias F07 (código + pruebas) → T2 Manual Técnico → T3 Manual de Usuario (referencias a capturas)
→ T4 README portada + steering/structure.md → T5 revisión de repositorio
→ T6 (usuario, VM) capturas + correr_pruebas + revisión final de los manuales
```

Primero se corrige el código (T1), para que los manuales describan la versión final. Las capturas (T6) se toman al final, cuando la interfaz ya no cambia.

## 2. `docs/Manual_Tecnico.md` — índice

Un solo archivo, en español, con tabla de contenido enlazada. Las tablas y los bloques de código se copian del código real, no de los specs.

1. **Introducción**: objetivo, componentes y versión del kernel.
2. **Arquitectura**:
   - diagrama Mermaid `flowchart` con kernel (syscalls 548–553, `log_watch` kthread, `ipc_channel`), sensor eBPF (tracepoints/kprobes, mapas, ring buffer), daemon (4 hilos y módulos) y dashboard (HTTP + SSE);
   - tabla de módulos del daemon (archivo → responsabilidad → hilo que lo usa).
3. **Kernel y syscalls**:
   - tabla 548–553 (número, nombre, firma, práctica de origen, quién la llama y para qué);
   - archivos de `Kernel/` con su ruta original;
   - límites heredados (`IPC_MSG_MAX_LEN`, cola de 128, `LOG_WATCH_MAX_FILES`, intervalo de 3 s);
   - formato `EVT|…`;
   - compilación con `preparar_kernel.sh`: opciones `CONFIG_BPF*`, `CONFIG_DEBUG_INFO_BTF` y `CONFIG_FTRACE_SYSCALLS`;
   - `instalar_kernel.sh` y `verificar_kernel.sh`.
4. **Sensor eBPF**:
   - hooks (los 6 tracepoints y los 2 kprobes, con el porqué de `fd_install` y `security_file_release`);
   - tabla de mapas (`config_sensor`, `pid_daemon`, `auxiliar`, `pendientes`, `abiertos`, `eventos`: tipo, clave, valor, tamaño y uso);
   - `struct evento_archivo`;
   - `ruta_en_directorio` (solo directamente dentro, compartido con las pruebas);
   - exclusión del TGID del daemon;
   - Makefile (`vmlinux.h` → `.bpf.o` → skeleton, `llvm-strip -g`);
   - libbpf: carga, `attach`, `destroy` y `ring_buffer__consume_n`.
5. **Programa intermedio**:
   1. hilos y despertadores: HTTP con `poll` + `fd_aviso`, `monitoreo` con `timedwait`, `archivos` con `poll` sobre `fd_control` + ring buffer, `ipc` bloqueado en 549;
   2. sincronización: tabla de **todos** los mutex del código (`lock_alertas`, `lock_metricas`, `lock_espera`, `lock_procesos`, `lock_sesiones`, `lock_registro`, `lock_sensor`, `lock_archivos`, `lock_blacklist`, `lock_logwatch`, y los de `comun.h`), con qué protege cada uno, orden `lock_sensor → lock_archivos`, locks hoja y la regla "ninguna syscall bloqueante con un mutex tomado";
   3. manejo de errores: respuestas JSON de error, `411`/`413`/`431`, sensor no disponible, blacklist inválida al arrancar, `ENOSYS` en IPC, señales (`SIGTERM`/`SIGINT`) y apagado ordenado (orden de `join`, `logwatch_finalizar_todos`, `sensor_finalizar`, `blacklist_finalizar`);
   4. `daemon.conf`: tabla de claves, valor por defecto y validación;
   5. autenticación PAM, grupos, sesiones, cookie, expiración y tabla de permisos (las 24 filas de `auth.c`);
   6. alertas: catálogo de `alertas.c` (motivo, tipo y severidad), orden y límites;
   7. métricas y procesos: claves de `/proc`, deltas, top 20 y guardas del `kill`;
   8. análisis de archivos, antivirus y cuarentena: SHA-256 EVP, listado, generación, `analizar_abierto`, re-evaluación, cuarentena automática y manual, formato de `blacklist.json` (R1 de F06) y firmas de demostración;
   9. logwatch: registro, regla de rutas y ciclos.
6. **Conexión con el frontend**:
   - tabla completa de la API (método, ruta, rol, cuerpo, `200` y errores);
   - eventos SSE con un ejemplo JSON de cada uno (`alerta`, `metricas`, `procesos`, `monitoreo`, `archivo`, `archivos`, `amenaza`);
   - reconexión y `id:`;
   - diagrama Mermaid `sequenceDiagram`: escritura en el directorio vigilado → eBPF → ring buffer → hilo `archivos` → SHA-256 → blacklist → `fchmod` → alertas → `fd_aviso` → SSE → dashboard.
7. **Dependencias y justificación**: libbpf, OpenSSL, PAM, cJSON vendorizado (MIT, versión) y por qué no hay frameworks en el dashboard.
8. **Decisiones de seguridad**: R2.3 de F08, con el porqué de cada una.
9. **Limitaciones conocidas**: R2.4 de F08.
10. **Compilación, ejecución y pruebas**:
    - `make`, `make pruebas`;
    - tabla de suites de `correr_pruebas.sh` (script, puerto y qué verifica);
    - `demo.sh` y `demo_antivirus.sh`.

## 3. `docs/Manual_Usuario.md` — índice

1. Qué es y requisitos (navegador, URL `http://localhost:8080`, cuenta en `admin_user` o `common_user`).
2. Arranque rápido: cómo levanta el daemon el responsable del equipo (`sudo ./daemon_monitor -d`) y cómo crear cuentas con `setup_usuarios.sh`.
3. Iniciar y cerrar sesión, y el error de usuario sin grupo.
4. Vista general del dashboard: tarjetas y paneles.
5. Métricas: las 4 gráficas y cómo leerlas.
6. Alertas: colores, orden y tipos.
7. Procesos: top 20, consulta por PID y matar con confirmación (solo admin).
8. Archivos analizados: estados, interruptor del escaneo y cambio de ruta (admin).
9. Antivirus: amenazas detectadas, cuarentena manual (admin), panel Cuarentena y Base de firmas (subir `blacklist.json`, errores).
10. Logwatch (admin): crear, la tabla y finalizar; qué rutas acepta.
11. Vista del usuario común: qué ve y qué no.
12. Tabla "Qué puede hacer cada rol".
13. Problemas frecuentes.

## 4. Capturas (`imgs/`)

Las toma el usuario en la VM, con `scripts/demo.sh` o el daemon real y los usuarios de demostración. El navegador va a ancho completo y sin datos personales. El nombre de cada archivo es exacto.

| Archivo | Qué muestra | Manual |
|---|---|---|
| `01-login.png` | Pantalla de inicio de sesión vacía | Usuario §3 |
| `02-login-error.png` | Error con un usuario sin grupo | Usuario §3 |
| `03-dashboard-admin.png` | Vista general como admin (tarjetas + métricas) | Usuario §4 |
| `04-metricas.png` | Las 4 gráficas con datos | Usuario §5 |
| `05-alertas.png` | Panel de alertas con LOW, MEDIUM y HIGH | Usuario §6 |
| `06-procesos.png` | Top 20 | Usuario §7 |
| `07-consulta-pid.png` | Resultado de la consulta por PID | Usuario §7 |
| `08-kill-confirmacion.png` | Diálogo de confirmación de matar | Usuario §7 |
| `09-archivos.png` | Archivos analizados con benignos e infectados | Usuario §8 |
| `10-ruta-vigilada.png` | Interruptor y formulario de ruta | Usuario §8 |
| `11-amenazas.png` | Amenazas detectadas tras `demo_antivirus.sh` | Usuario §9 |
| `12-cuarentena-confirmacion.png` | Confirmación de la cuarentena manual | Usuario §9 |
| `13-panel-cuarentena.png` | Panel Cuarentena con automáticas y manual | Usuario §9 |
| `14-base-firmas.png` | Base de firmas y aviso de actualización | Usuario §9 |
| `15-blacklist-error.png` | Error de blacklist inválida | Usuario §9 |
| `16-logwatch.png` | Panel Logwatch con la tabla | Usuario §10 |
| `17-logwatch-error.png` | `Ruta invalida: …` | Usuario §10 |
| `18-alerta-logwatch.png` | Alerta `Palabra clave detectada en log` | Usuario §10 |
| `19-dashboard-comun.png` | Vista del usuario común (sin paneles de admin) | Usuario §11 |
| `20-correr-pruebas.png` | Terminal con el resumen de `correr_pruebas.sh` en verde | Técnico §10 |

Los manuales las referencian con rutas relativas: `![Inicio de sesión](../imgs/01-login.png)`.

## 5. README portada (≤ 200 líneas)

1. Título, una línea de descripción y la tabla de componentes.
2. Requisitos (paquetes).
3. Inicio rápido en 6 pasos: kernel, verificación, usuarios, `make`, arranque y URL.
4. Estructura de carpetas (árbol corto).
5. Documentación: enlaces a `docs/Manual_Tecnico.md`, `docs/Manual_Usuario.md`, `specs/README.md` y `enunciado.md`.
6. Pruebas: `make -C ProgramaIntermedio pruebas` y `sudo scripts/correr_pruebas.sh`.

Las secciones 5–12 actuales pasan al Manual Técnico, actualizadas con el código.

## 6. Revisión del repositorio (T5)

Comandos que el implementador corre y pega en la evidencia:

```bash
git ls-files ProyectoUnico_2S2026 | grep -E '\.(o|log|txt)$|cookie|test_[a-z]+$|so2_(logs|vigilado)'   # → solo archivos esperados (p. ej. ninguno)
git ls-files ProyectoUnico_2S2026/ProgramaIntermedio/ebpf                                              # → Makefile, sensor.bpf.c, sensor.h, filtro_ruta.h
make -C ProyectoUnico_2S2026/ProgramaIntermedio clean all 2>&1 | grep -ci warning                       # → 0
git status --short                                                                                       # → vacío tras make
find ProyectoUnico_2S2026 -user root                                                                     # → vacío
git grep -nE 'password"?\s*[:=]\s*"[^"]{4,}' -- ProyectoUnico_2S2026 ':!*/specs/*'                      # → solo los placeholders de los scripts, sin valores reales
```

Para cada imagen, comprobar que algún manual la referencia, y para cada referencia, que la imagen existe:

```bash
for f in ProyectoUnico_2S2026/imgs/*.png; do grep -q "$(basename "$f")" ProyectoUnico_2S2026/docs/*.md || echo "sin referencia: $f"; done
grep -ohE '\.\./imgs/[^)]+' ProyectoUnico_2S2026/docs/*.md | sort -u | while read -r r; do [ -f "ProyectoUnico_2S2026/docs/$r" ] || echo "falta: $r"; done
```

## 7. Decisiones

| Decisión | Alternativa | Motivo |
|---|---|---|
| Diagramas en Mermaid dentro del Markdown | `arquitectura.drawio` + PNG exportado | El enunciado pide Markdown; Mermaid se versiona como texto, se ve en GitHub y no se desactualiza contra una imagen. |
| Capturas a cargo del usuario | El implementador automatiza un navegador | El daemon necesita root (PAM y eBPF) y el implementador no usa `sudo`. |
| README como portada; el detalle va al Manual Técnico | Mantener el README de 517 líneas | Evita que la misma información quede en dos lugares y diverja. |
| Advertencias de F07 antes de documentar | Documentar primero | Así los manuales describen la versión final. |
| Limitaciones de P3 solo documentadas | Corregir el kernel | Decisión del usuario (2026-10-21). |
