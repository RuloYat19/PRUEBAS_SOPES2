# AGENTS.md — Proyecto Único SO2 2S2026

Instrucciones para el agente que **implementa código** en este proyecto. Léelas completas antes de tocar cualquier archivo.

---

## 1. Roles

| Rol | Quién | Responsabilidad |
|---|---|---|
| **Usuario** | Estudiante (carné 202100106) | Decide alcance, comparte los prompts, aprueba commits y entregas. |
| **Implementador** | Tú (agente de código) | Ejecuta las tareas de `tasks.md` de **una** feature, con pruebas y evidencia. |
| **Revisor** | Otro agente | Escribe los specs y revisa tu trabajo contra ellos. Deja hallazgos en `review.md`. |

No redactas ni cambias requisitos ni diseño. Si un spec está mal, es ambiguo o no se puede cumplir, **detente y repórtalo**; no improvises otra solución.

## 2. Qué leer antes de empezar (en orden)

1. `steering/product.md` — propósito, roles, permisos y alertas.
2. `steering/tech.md` — stack, comandos, syscalls y convenciones técnicas.
3. `steering/structure.md` — dónde va cada archivo.
4. `specs/NN-nombre/requirements.md` (o `bugfix.md`) — qué construir y cómo se acepta.
5. `specs/NN-nombre/design.md` — cómo construirlo.
6. `specs/NN-nombre/tasks.md` — qué hacer, en orden.
7. `specs/NN-nombre/review.md` — si existe, hallazgos pendientes que debes corregir primero.

`enunciado.md` es la fuente de verdad final. Si un spec contradice al enunciado, repórtalo.

## 3. Flujo de trabajo por feature

Cada feature vive en `specs/NN-nombre/` y pasa por tres fases ya escritas por el revisor:

```text
requirements.md / bugfix.md  →  design.md  →  tasks.md  →  (tú implementas)  →  review.md
```

Reglas:

- Trabaja **solo** en la feature que indica el prompt, tarea por tarea y en el orden de `tasks.md`.
- Al terminar cada tarea: corre su verificación, marca `- [x]` y agrega debajo una línea `Evidencia:` con el comando ejecutado y su resultado real.
- Una tarea que no pudiste verificar queda `- [ ]` con `Pendiente:` y el motivo (por ejemplo, “requiere reiniciar en el kernel 6.12.69”).
- Nunca marques como hecha una tarea sin evidencia. Nunca inventes salidas de comandos.
- Si `review.md` tiene hallazgos abiertos, corrígelos antes de seguir y márcalos como `Corregido:` con la evidencia.
- Consulta Engram al empezar y guarda cada iteración al terminarla (sección 10).

## 4. Límites de edición

- Escribe **solo** dentro de `ProyectoUnico_2S2026/`, más el `.gitignore` de la raíz cuando una tarea lo pida.
- `Practica_1_2S2026/` … `Practica_4_2S2026/` son **de solo lectura**. Se copian y adaptan; nunca se modifican.
- No edites `enunciado.md`, `steering/` ni `requirements.md`/`design.md`. En `tasks.md` solo cambias casillas y líneas de evidencia.
- No agregues dependencias, librerías ni frameworks que no estén en `steering/tech.md`.
- No crees syscalls nuevas ni pongas lógica de detección, hashes o severidad en el kernel o en el eBPF.

## 5. Convenciones de código

- **C (daemon):** `gcc -Wall -Wextra -O2 -pthread -D_GNU_SOURCE`, **cero warnings**. Estilo del código de P4: tabs, llaves de función en línea propia, funciones `modulo_accion` en español.
- **eBPF:** `*.bpf.c` compilado con Clang `-target bpf`; solo filtro por ruta y PID del daemon.
- **Dashboard:** HTML + CSS + JS vanilla, sin build. Toda autorización vive en el daemon.
- **Bash:** `set -euo pipefail` y los helpers `log/warn/err/fase` de P4.
- Identificadores, mensajes, claves JSON y docs en **español**; claves JSON en `snake_case`.
- **Sin comentarios en el código.** Ningún comentario en C, eBPF, JS, CSS, HTML, Bash ni Makefile, tampoco en código copiado de prácticas anteriores (quítalos al portarlo). El código se explica con nombres claros; las justificaciones van al Manual Técnico y la memoria de cada iteración a Engram (sección 10).
  - Excepciones: la línea shebang (`#!/usr/bin/env bash`), los archivos de `Kernel/` (copia exacta del overlay de P4), el código de terceros vendorizado en `ProgramaIntermedio/lib/` (se copia sin modificar y conserva su licencia) y los archivos de configuración como `daemon.conf`, donde los comentarios documentan las claves.
- Ningún error de syscall, archivo, JSON o cliente HTTP puede terminar el daemon.
- Mutex en toda estructura compartida entre hilos. Nunca llames una syscall bloqueante con un mutex tomado.

## 6. Pruebas y verificación

- Cada requisito con criterio verificable tiene una prueba o comando en `tasks.md`; ejecútalo.
- Comprueba lo que sí puedes en el host (compilación, `bash -n`, `diff`, pruebas sin kernel) y marca como **Pendiente en VM** lo que requiere el kernel `6.12.69-so2-202100106` (syscalls 548–553, eBPF, PAM real).
- Archivos temporales de prueba fuera del repositorio (`mktemp -d`). No dejes basura en el árbol.
- Rutas de prueba de logs y carpeta vigilada en `~/so2_logs/` y `~/so2_vigilado/`, **nunca `/tmp`**.

## 7. Git

- Trabaja en la rama actual. **No hagas push**, no reescribas historia y no hagas operaciones destructivas.
- Commit local por tarea o grupo coherente, solo con archivos de la feature, mensaje con el prefijo del historial y con descripcion corta y concisa no mas de 50 caracteres: `add:`, `upd:`, `fix:` (ej. `add: F00 overlay del kernel en Kernel/`).
- El ejecutable `ProgramaIntermedio/daemon_monitor` **sí se versiona** (requisito del enunciado). Los `*.o`, `vmlinux.h` y `*.skel.h` no.
- Cuidado: el `.gitignore` raíz ignora `*ref*` y `*.log`. No nombres archivos entregables con `ref` en el nombre (ej. `preferencias.c`) ni con extensión `.log`.

## 8. Reporte final (obligatorio)

Al terminar la sesión, responde con exactamente este formato para que el usuario lo comparta con el revisor:

```markdown
## Reporte F<NN> — <nombre>

**Tareas:** <hechas>/<total>  ·  **Commits:** <hash corto> <mensaje>, …

### Hecho
- T<n>: <qué se hizo> — Evidencia: `<comando>` → <resultado>

### Pendiente
- T<n>: <motivo> (ej. requiere VM con kernel 6.12.69)

### Desviaciones del spec
- <ninguna | qué cambió, por qué y qué requisito afecta>

### Dudas para el revisor
- <ninguna | pregunta concreta>

### Engram
- Guardado: <topic_keys guardados | no disponible>
```

## 9. Comandos frecuentes

```bash
# Daemon
make -C ProgramaIntermedio clean all
sudo ProgramaIntermedio/daemon_monitor -c ProgramaIntermedio/daemon.conf        # primer plano
sudo ProgramaIntermedio/daemon_monitor -c ProgramaIntermedio/daemon.conf -d     # segundo plano

# Kernel (en la VM)
scripts/preparar_kernel.sh --sin-instalar
scripts/instalar_kernel.sh && sudo reboot
scripts/verificar_kernel.sh

# Pruebas
scripts/pruebas_integracion.sh
```

## 10. Memoria persistente (Engram)

Engram es la bitácora compartida entre el implementador y el revisor. Úsala para **consultar** antes de trabajar y para **guardar** cada iteración. Si las herramientas de Engram no están disponibles, sigue trabajando y escribe `Engram: no disponible` en el reporte.

### Proyecto

- Resuelve el proyecto con `mem_current_project` desde la raíz del repositorio. Debe devolver `mapa`.
- Si devuelve otro nombre, varios candidatos o nada, **no inventes uno**: pregunta al usuario cuál usar.
- Pasa siempre `project: "mapa"` en las consultas.

### Al empezar una sesión

1. `mem_context` del proyecto `mapa`.
2. `mem_search` con `F<NN>` y con el nombre de la feature, para recuperar iteraciones previas, decisiones y la revisión (`proyecto-unico/f<NN>/review`).
3. Si un resultado viene truncado, ábrelo completo con `mem_get_observation`.
4. Concilia lo que encuentres con `tasks.md` y `review.md`; los archivos del repositorio mandan si hay diferencias.

### Durante la feature (una vez por iteración)

Guarda con `mem_save` al cerrar cada tarea y cada vez que tomes una decisión, corrijas un bug o descubras algo no obvio. Usa `topic_key` para actualizar en lugar de duplicar:

| Qué | `topic_key` | `type` |
|---|---|---|
| Tarea terminada | `proyecto-unico/f<NN>/t<n>` | `decision` o `pattern` |
| Bug encontrado y corregido | `proyecto-unico/f<NN>/bug-<tema>` | `bugfix` |
| Hallazgo técnico / gotcha | `proyecto-unico/f<NN>/hallazgos` | `discovery` |
| Corrección de un hallazgo de `review.md` | `proyecto-unico/f<NN>/correcciones` | `bugfix` |

Contenido con este formato y rutas relativas al repositorio:

```text
**What**: qué se hizo
**Why**: requisito (R#) o motivo
**Where**: archivos tocados
**Learned**: problemas, decisiones o pendientes (omitir si no hay)
```

### Al terminar la sesión

1. Guarda el reporte final completo (sección 8) con `topic_key: proyecto-unico/f<NN>/reporte`.
2. Llama a `mem_session_summary` (objetivo, hecho, pendiente, próximos pasos, archivos relevantes).
3. Luego entrega el reporte al usuario. Guardar en Engram nunca reemplaza el reporte.

### Qué no guardar

Contraseñas (incluidas las de usuarios de prueba), tokens de sesión, salidas largas de compilación o logs completos. Resume y cita solo la línea relevante.

### Del lado del revisor

El revisor guarda cada revisión en `proyecto-unico/f<NN>/review` además de `review.md`, así que esa entrada siempre tiene el último estado de la revisión.
