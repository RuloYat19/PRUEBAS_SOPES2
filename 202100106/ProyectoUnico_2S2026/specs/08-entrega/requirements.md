# F08 — Documentación y entrega · Requisitos

## Introducción

Cerrar el proyecto con lo que pide el enunciado en **Entregables** y **Criterios de evaluación**:
- el **Manual Técnico** y el **Manual de Usuario** en Markdown, con capturas;
- un repositorio ordenado según la estructura fijada (`Kernel`, `ProgramaIntermedio` con `ebpf/`, `Dashboard`, todo dentro de `ProyectoUnico_2S2026`);
- el ejecutable compilado al día;
- antes de documentar, las 6 advertencias que dejó la revisión de F07 (A1–A6).

No se agrega funcionalidad nueva.

## Glosario

- **Captura:** imagen PNG en `imgs/NN-descripcion-corta.png` (numeración de dos dígitos en el orden en que aparece en los manuales).
- **Limitación conocida:** comportamiento aceptado y documentado que no se corrige en este proyecto.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Advertencias de F07

EL implementador DEBERÁ corregir A1–A6 de [`../07-logwatch/review.md`](../07-logwatch/review.md):
1. **A1:** `logwatch_validar` compara las claves distinguiendo mayúsculas (`cJSON_GetObjectItemCaseSensitive`). `{"PALABRA":…,"central":…,"rutas":…}` → `400` `Cuerpo invalido`. Caso nuevo en `test_logwatch`.
2. **A2:** caso en `test_logwatch` con la base reemplazada por un enlace (`rename` + `symlink`): la ruta vigilada da `Ruta invalida: …` y la central da `Ruta central invalida`.
3. **A3:** en `pruebas_logwatch.sh`, las variables, `limpiar` y el `trap` van **antes** de `usuarios_prueba_crear`.
4. **A4:** la configuración de prueba usa solo claves válidas (`dashboard`, `blacklist`) y no tiene `log_daemon`. El log del daemon de prueba no contiene `clave desconocida`; la fase 12 lo comprueba.
5. **A5:** la fase 9 comprueba que cada uno de los 15 `DELETE` devuelve `200`.
6. **A6:** `logwatch_finalizar_todos` registra en el log `logwatch <id> detenido` si `stop` devuelve 0, y `sys_stop_log_watch(<id>) fallo: <strerror>` si falla. Caso nuevo en `test_logwatch` con un `stop` simulado que falla, que verifica que el registro igual queda vacío.

## R2 — Manual Técnico (`docs/Manual_Tecnico.md`)

1. DEBERÁ cubrir, como mínimo, lo que pide el enunciado:
   1. la **integración de las syscalls** reutilizadas (548–553):
      - número, firma, práctica de origen y quién las usa en el daemon;
      - los archivos de `Kernel/` y la ruta original de cada uno;
      - cómo se compila e instala el kernel 6.12.69 con las opciones eBPF/BTF del enunciado (§2.3.15);
   2. el **sensor eBPF**:
      - hooks;
      - mapas (nombre, tipo, clave, valor y para qué sirve cada uno);
      - el **Ring Buffer** y la estructura del evento;
      - el filtro por ruta (solo directamente dentro del directorio);
      - la exclusión del PID del daemon;
      - la carga, activación y desactivación con libbpf;
      - cómo el hilo `archivos` consume los eventos;
   3. la **funcionalidad del programa intermedio**:
      - los hilos (HTTP/SSE, `monitoreo`, `archivos`, `ipc`) y qué hace cada uno;
      - qué los despierta (sin polling);
      - los mutex, con el **orden de bloqueo** (`lock_sensor` → `lock_archivos`, y los locks hoja);
      - el **manejo de errores** (cada fallo responde con un código y nunca termina el proceso), las señales y el apagado ordenado;
   4. la **conexión con el frontend**:
      - la API HTTP completa (método, ruta, rol, cuerpo, respuestas y errores);
      - los eventos SSE (`alerta`, `metricas`, `procesos`, `monitoreo`, `archivo`, `archivos` y `amenaza`) con un ejemplo de cada uno;
      - las sesiones y la cookie;
      - la tabla de permisos.
2. Además DEBERÁ incluir:
   - un diagrama de arquitectura (kernel ↔ eBPF ↔ daemon ↔ dashboard) y un diagrama de secuencia del flujo "archivo modificado → hash → detección → cuarentena → alerta → dashboard", ambos en **Mermaid** dentro del Markdown;
   - la autenticación PAM y los grupos;
   - el motor de alertas: catálogo, severidades y tipos;
   - el formato de `blacklist.json` y sus reglas;
   - la cuarentena;
   - el logwatch y su regla de rutas;
   - `daemon.conf` (todas las claves con su valor por defecto);
   - la justificación de las dependencias: cJSON vendorizado (MIT), libbpf, OpenSSL EVP y PAM, y que el dashboard no usa frameworks;
   - cómo compilar, ejecutar y correr las pruebas (`make pruebas`, `sudo scripts/correr_pruebas.sh`).
3. DEBERÁ tener una sección **Decisiones de seguridad** con las del proyecto:
   - el daemon como root;
   - `openat2` + `RESOLVE_NO_SYMLINKS`;
   - `fchmod` sobre el descriptor;
   - la cuarentena manual solo sobre archivos del listado;
   - el búfer de 1 MiB solo para admin;
   - las rutas de logwatch limitadas;
   - las guardas del `kill`;
   - las contraseñas nunca registradas.
4. DEBERÁ tener una sección **Limitaciones conocidas** con, al menos:
   - las del sensor eBPF (F05 R1.6): rutas relativas o `dirfd`, `rename`/`mv`, `openat2`/`io_uring`, `mmap`;
   - las del kernel de P3, todas sin corregir por decisión del usuario (2026-10-21):
     - 548/549/550/551 no verifican permisos: cualquier usuario local puede crear o detener un `log_watch`, leer archivos de root a través de la ruta central, inyectar mensajes falsos en el canal IPC (alertas `Palabra clave detectada en log` falsas) o consumir mensajes que eran para el daemon;
     - el kthread vuelve a abrir las rutas por nombre cada 3 s y sigue enlaces;
   - los logwatch creados desde el dashboard no se guardan al reiniciar.
5. Todo lo que diga el manual DEBERÁ coincidir con el código entregado: nombres de funciones, archivos, constantes, textos de error y rutas. Ningún dato puede salir de una versión anterior.

## R3 — Manual de Usuario (`docs/Manual_Usuario.md`)

1. DEBERÁ explicar, paso a paso y con capturas, el uso del dashboard **para administrador y para usuario común**:
   - requisitos y arranque (cómo se levanta el daemon y la URL);
   - inicio y cierre de sesión, y qué pasa con un usuario sin grupo;
   - el panel de métricas y sus gráficas;
   - procesos (top 20, búsqueda por PID, matar con confirmación);
   - alertas (colores por severidad y su orden);
   - el escaneo de archivos (interruptor y ruta vigilada);
   - amenazas detectadas, cuarentena manual y automática, y la base de firmas;
   - logwatch (crear, la tabla y finalizar);
   - qué ve y qué no ve el usuario común.
2. DEBERÁ incluir una tabla "Qué puede hacer cada rol", igual a la tabla de permisos del enunciado.
3. DEBERÁ incluir una sección de **problemas frecuentes** con su solución, al menos:
   - el puerto ocupado;
   - `sensor eBPF no disponible`;
   - la blacklist inválida;
   - `Ruta invalida` en logwatch;
   - la sesión expirada.
4. Cada paso con interfaz DEBERÁ tener su captura. Las capturas usan solo usuarios de demostración, sin contraseñas visibles ni datos personales.

## R4 — Capturas (`imgs/`)

1. Las capturas de la lista de diseño §4 DEBERÁN existir en `imgs/` con su nombre exacto, en PNG, y cada una DEBERÁ estar referenciada por al menos un manual.
2. Ninguna imagen de `imgs/` puede quedar sin referenciar, y ningún manual puede referenciar una imagen inexistente.
3. Las capturas las toma el usuario en la VM (requieren el daemon como root y un navegador). El implementador escribe los manuales con las referencias definitivas.

## R5 — README y steering

1. `README.md` DEBERÁ quedar como portada, de 200 líneas como máximo:
   - qué es el proyecto;
   - requisitos;
   - inicio rápido (kernel, dependencias, `setup_usuarios.sh`, compilación, arranque, URL);
   - el mapa de carpetas;
   - los enlaces a los dos manuales y a `specs/`.

   El detalle que hoy está en las secciones 5–12 se mueve al Manual Técnico, sin duplicarse.
2. `steering/structure.md` §2 DEBERÁ reflejar el árbol real entregado: archivos `.c`/`.h`/`.js` y scripts que existen, sin archivos que no existen (por ejemplo `docs/arquitectura.drawio` si no se entrega).

## R6 — Repositorio listo para entregar

1. `Kernel/` DEBERÁ contener solo los archivos modificados, con su ruta original del árbol 6.12.69, y cada uno DEBERÁ ser idéntico al del árbol con el que se compiló el kernel instalado (`$ARBOL` de `scripts/preparar_kernel.sh`). Comprobado por el revisor el 2026-10-22: los 7 archivos son idénticos.
2. `ProgramaIntermedio/daemon_monitor` DEBERÁ corresponder al código entregado: tras `make clean all`, `git status` no muestra cambios.
3. `ProgramaIntermedio/ebpf/` DEBERÁ contener `sensor.bpf.c`, `sensor.h`, `filtro_ruta.h` y su `Makefile`; los generados (`vmlinux.h`, `*.skel.h`, `*.bpf.o`) siguen ignorados.
4. El repositorio NO DEBERÁ versionar:
   - binarios de prueba, logs ni cookies;
   - archivos de `so2_logs`/`so2_vigilado`;
   - archivos que pertenezcan a root;
   - contraseñas ni tokens.

   La revisión se hace con `git ls-files` y `git grep`.
5. `specs/README.md` DEBERÁ marcar las features 00–07 como `Aprobada` y la 08 según su estado.
6. Las suites DEBERÁN seguir con `0 FALLIDAS` en `sudo scripts/correr_pruebas.sh`, con la versión final del código.
