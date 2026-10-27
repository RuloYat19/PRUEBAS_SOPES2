# F07 — Logwatch e IPC · Diseño

Requisitos: [`requirements.md`](requirements.md). Base:
- F00: syscalls 548–551, `hilo_ipc`, `ipc_despertar`, arranque y paro de los logwatch en `main.c`;
- F02: `alerta_emitir_log` y `MOT_PALABRA_EN_LOG`;
- F01: permisos, sesiones y consulta `?clave=valor`, como en `/api/proceso?pid=N`;
- F06: `abrir_sin_enlaces`.

---

## 1. Visión general

```text
arranque (main.c):
    base = dirname(realpath(conf.log_central))
    logwatch_iniciar(base)
    por cada palabra de daemon.conf → logwatch_crear(…, "configuracion", "")
hilo HTTP:
    GET    /api/logwatch        → logwatch_lista            → JSON
    POST   /api/logwatch        → logwatch_validar → logwatch_crear      (syscall 550 fuera del lock)
    DELETE /api/logwatch?id=N   → logwatch_finalizar                     (syscall 551 fuera del lock)
hilo ipc (sin cambios):
    sys_ipc_channel_receive (bloqueado) → "EVT|…" → alerta_emitir_log (MEDIUM)
apagado (main.c):
    logwatch_finalizar_todos() → luego detener_hilo_ipc()
```

| Pieza | Archivo | Requisito |
|---|---|---|
| `abrir_sin_enlaces` compartido | `ProgramaIntermedio/rutas.c` (nuevo); `analisis.c` deja de tener su copia `static` | R2.3 |
| Registro, validación, crear, finalizar | `ProgramaIntermedio/logwatch.c` (nuevo) | R1–R4 |
| Arranque y paro con el registro | `ProgramaIntermedio/main.c` | R1.1, R1.4 |
| Endpoints y filas de permiso | `ProgramaIntermedio/http.c`, `auth.c`, `json.c` | R2–R4 |
| Panel | `Dashboard/index.html`, `Dashboard/js/logwatch.js` (nuevo), `app.js`, `styles.css` | R6 |
| Pruebas | `pruebas/test_logwatch.c` (nuevo), `scripts/pruebas_logwatch.sh` (nuevo) | R7 |

## 2. `rutas.c`

```c
int abrir_sin_enlaces(const char *ruta, int flags, mode_t modo);
```

Es el mismo helper de F06 con un parámetro `modo`, que se usa solo con `O_CREAT`:
- `syscall(SYS_openat2, AT_FDCWD, ruta, &how, sizeof(how))`, con `how.flags = flags`, `how.mode = modo` (0 si no hay `O_CREAT`) y `how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS`;
- `analisis.c` llama `abrir_sin_enlaces(ruta, flags, 0)` en sus 4 sitios, y su comportamiento no cambia (el caso 16 de `test_analisis` sigue en verde).

## 3. `logwatch.c`

### 3.1 Tipos (`comun.h`)

```c
#define MAX_LOGWATCH		16
#define MAX_PALABRA_LOG		63

struct logwatch {
	int id;
	char palabra[MAX_PALABRA_LOG + 1];
	char central[PATH_MAX];
	char rutas[LOG_WATCH_MAX_FILES][PATH_MAX];
	int n_rutas;
	char origen[16];
	char usuario[MAX_USUARIO + 1];
	char creado[32];
};

struct peticion_logwatch {
	char palabra[MAX_PALABRA_LOG + 1];
	char central[PATH_MAX];
	char rutas[LOG_WATCH_MAX_FILES][PATH_MAX];
	int n_rutas;
};

typedef long (*fn_start_log_watch)(const char *const *rutas, int n, const char *central, const char *palabra);
typedef long (*fn_stop_log_watch)(int id);
```

### 3.2 API

| Función | Qué hace |
|---|---|
| `void logwatch_iniciar(const char *base)` | Guarda la base canónica (`realpath`) y vacía el registro. |
| `void logwatch_base(char *destino, size_t tam)` | Copia la base. |
| `int logwatch_validar(const char *json, struct peticion_logwatch *p, int *codigo, char *error, size_t tam)` | R2.1–R2.3 y R2.2.1–R2.2.9, en el orden del requisito. Devuelve 0, o -1 con `codigo` (400/409) y el texto exacto. Para la central inexistente **no** la crea; solo valida. |
| `int logwatch_crear(const struct peticion_logwatch *p, const char *origen, const char *usuario, struct logwatch *creado, char *error, size_t tam)` | Si la central no existe, la crea (§3.4). Llama a `start` **fuera** del lock y, si devuelve un ID, registra la entrada bajo `lock_logwatch`. Devuelve 0, -1 (límite, carrera) o -2 (fallo de la syscall, `error` = `strerror`). |
| `int logwatch_finalizar(int id, char *error, size_t tam)` | Bajo el lock comprueba que el `id` existe y copia la entrada; suelta el lock; llama a `stop`; vuelve a tomar el lock para quitarla (0 o `ESRCH`). Devuelve 0, `-ENOENT` (no está o `ESRCH`) o -1 (otro error, `error` = `strerror`). |
| `size_t logwatch_lista(struct logwatch *destino, size_t max)` | Copia ordenada por `id`. |
| `void logwatch_finalizar_todos(void)` | Copia los IDs bajo el lock, llama a `stop` para cada uno fuera del lock, registra en el log y vacía el registro. |
| `void logwatch_usar_syscalls(fn_start_log_watch, fn_stop_log_watch)` | Solo para `test_logwatch.c`. Por defecto apunta a envolturas de `syscall(SYS_START_LOG_WATCH, …)` y `syscall(SYS_STOP_LOG_WATCH, …)`, que devuelven `-1` con `errno`. |

`lock_logwatch` es hoja: dentro solo se copian datos (sin E/S, sin `registrar`, sin otros locks). Todas las mutaciones ocurren en el hilo HTTP, salvo `logwatch_finalizar_todos`, que corre en `main` después de que termina `servidor_bucle`.

### 3.3 Regla de ruta (R2.3)

`ruta_valida(ruta, es_central)`:
1. Largo de 1 a 255 bytes, sin `|` ni caracteres de control.
2. `ruta_en_directorio(ruta, base_con_barra, largo)` de `ebpf/filtro_ruta.h`. Ya rechaza `<base>/` vacío y las subcarpetas; el nombre `.` o `..` se rechaza aparte.
3. `fd = abrir_sin_enlaces(ruta, O_PATH | O_NOFOLLOW | O_CLOEXEC, 0)`:
   - ruta vigilada: falla → inválida; `fstat` no regular → inválida;
   - central: `ENOENT` → válida (se creará); otro error → inválida; existe y no es regular → inválida.

La comparación de R2.2.7 y R2.2.8 es por cadena exacta, porque todas las rutas ya son `<base>/<nombre>` canónicas.

### 3.4 Creación de la central

1. `abrir_sin_enlaces(central, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644)`.
2. `fstat` del directorio base (`abrir_sin_enlaces(base, O_PATH | O_DIRECTORY, 0)`) y `fchown(fd, st.st_uid, st.st_gid)`. Si `fchown` falla, se registra en el log y se sigue.
3. `EEXIST` no es error: otro logwatch la creó y ya se validó como regular.

### 3.5 `main.c`

- La lógica de arranque se queda en `main.c`: `preparar_archivo`, la central que no puede ser vigilada, y `n_rutas > 0`.
- Antes del bucle de palabras: `logwatch_iniciar(dirname de central)`.
- Cada palabra se arma como `struct peticion_logwatch` y se pasa a `logwatch_crear(&p, "configuracion", "", …)`, **sin** `logwatch_validar`: la configuración es de confianza y puede estar fuera de la base.
- Se elimina `watch_ids[]`. Al apagarse: `logwatch_finalizar_todos()` antes de `detener_hilo_ipc`, el mismo orden de hoy.

## 4. `http.c`, `auth.c` y JSON

### 4.1 Filas (24 en total)

```c
{ "GET",    "/api/logwatch", ROL_ADMIN },
{ "POST",   "/api/logwatch", ROL_ADMIN },
{ "DELETE", "/api/logwatch", ROL_ADMIN },
```

`DELETE` sin cuerpo no exige `Content-Length`. Si llega un cuerpo con `DELETE`, se ignora. Con otro método sigue el `405` de F01.

### 4.2 Respuestas

- `POST` → `logwatch_validar`; si pasa, `logwatch_crear(…, "dashboard", usuario, …)`:
  - `200` con `json_logwatch(entrada)`;
  - `-1` → `409` `Limite de 16 logwatch alcanzado`;
  - `-2` → `500` `No se pudo crear el logwatch: <strerror>`.
- `DELETE` → `id` de la consulta con `strtol`, comprobando el final de la cadena y > 0, si no `400` `ID invalido`:
  - `logwatch_finalizar`: 0 → `200`; `-ENOENT` → `404`; -1 → `500` `No se pudo finalizar el logwatch: <strerror>`.
- `GET` → `{"base":…,"logwatch":[json_logwatch…]}`.
- Todas las cadenas pasan por `json_cadena`.
- Las líneas de log de R2.5 y R3.5 las escribe `http.c` con `registrar`, después de la operación y fuera de cualquier lock.

### 4.3 Sin SSE nuevo

El registro es solo para admin y el SSE llega a todos los roles, así que no hay `event: logwatch`. El dashboard recarga con `GET`.

## 5. Dashboard

- `index.html`: sección `#panel-logwatch` con `data-rol="admin_user"`, el texto de la base, el formulario (`#form-logwatch`: `palabra`, `central`, `rutas` como `<textarea>`), el aviso y la tabla.
- `js/logwatch.js` (nuevo):
  - `cargarLogwatch()` hace `GET`, prellena `central` con `base + '/'` si está vacío y pinta la tabla con `textContent`;
  - `crearLogwatch()` separa `rutas` por líneas, quita espacios a los lados y descarta líneas vacías, hace `POST` y muestra el aviso;
  - `finalizarLogwatch(id, palabra)` usa `confirm()` con el texto exacto de R6.2 y hace `DELETE /api/logwatch?id=<id>`.
- `app.js`: llamar a `cargarLogwatch()` al abrir el SSE si el rol es admin. Usa el mismo patrón de recarga que el panel de cuarentena.

## 6. Pruebas

### 6.1 `pruebas/test_logwatch.c` (sin root)

Usa una base temporal con `mkdtemp` y `start`/`stop` simulados que guardan sus argumentos y devuelven lo que el caso pide.

1. Cada texto de R2.2.1–R2.2.9 con su código, incluido:
   - `/etc/passwd` → `Ruta invalida: /etc/passwd`;
   - un enlace dentro de la base → `Ruta invalida`;
   - una base reemplazada por un enlace → inválida;
   - una subcarpeta → inválida;
   - `.`, `..`, `|` y control en ruta y palabra.

   En cada caso: `start` no fue llamada y el registro no cambió.
2. Válido → `start` recibe exactamente las rutas, la central y la palabra; el registro tiene `origen` `dashboard`, el usuario y `creado` con formato.
3. Central inexistente → se crea con modo `0644` y dueño del directorio (sin root: el propio usuario).
4. `start` devuelve -1 con `ENOSYS` → -2, `No se pudo crear el logwatch: Function not implemented`, registro sin cambios.
5. Límite: 16 creados; el 17.º → 409 y `start` no fue llamada.
6. Ciclo (R2.2.8) en las dos direcciones contra un logwatch registrado.
7. Finalizar:
   - `stop` devuelve 0 → quitado;
   - `stop` con `ESRCH` → quitado y `-ENOENT`;
   - `stop` con `EPERM` → sigue en el registro y -1;
   - un ID que no existe → `-ENOENT` y `stop` no fue llamada.
8. `logwatch_finalizar_todos` llama a `stop` una vez por entrada y vacía el registro; `logwatch_lista` sale ordenada por `id`.
9. `json_logwatch` es parseable y tiene exactamente las 7 claves de R4.2.

`test_auth.c`: 24 filas, `DELETE /api/logwatch` admin y `PUT /api/logwatch` → -2.

### 6.2 `scripts/pruebas_logwatch.sh` (sudo, VM, puerto 8092)

- Preparación:
  - aborta, **antes** de crear usuarios, si existe `~/so2_logs/pruebas_logwatch` o si `pgrep -x daemon_monitor` encuentra otro daemon (el canal IPC es único);
  - `trap` que detiene el daemon, borra usuarios y, solo con `PREPARADO=1`, borra `~/so2_logs/pruebas_logwatch`.
- Configuración propia:
  - `log_central = <dir>/central.txt`;
  - `log_rutas = <dir>/inicial.log`;
  - `log_palabras = SO2T_INICIAL`;
  - `ruta_vigilada` en `~/so2_vigilado`, que el script crea y nunca borra.
- Esperas: las alertas se esperan con un límite de 10 s, porque el kthread revisa cada 3 s.

Las 12 fases:
1. `GET /api/logwatch`: admin `200` con `base` = `<dir>` y 1 entrada `configuracion`; común `403`; sin cookie `401`.
2. Agregar una línea con `SO2T_INICIAL` a `inicial.log` → alerta `MEDIUM` `Palabra clave detectada en log` con `inicial.log` y la línea, en `/api/alertas` y en el SSE; `central.txt` contiene la línea.
3. Crear `a.log` y `b.log` (como `$SUDO_USER`), luego `POST` con `palabra` `SO2T_NUEVA`, `central` `<dir>/central2.txt` y esas 2 rutas → `200`. `GET` la lista con `origen` `dashboard` y el usuario admin de prueba. `central2.txt` existe con el dueño de `<dir>`, y existe el kthread `log_watch_<id>` (`ps -e -o comm`).
4. Agregar `SO2T_NUEVA` a `b.log` → alerta con `b.log`.
5. Inválidos, con código y texto exactos: `/etc/shadow` como ruta; `/etc/x` como central; un enlace en `<dir>`; `|` en la palabra; central = ruta; `<dir>/central.txt` como ruta (409 por ciclo); cuerpo con una clave extra. Tras todos, la cantidad de kthreads `log_watch_*` y la lista no cambiaron, y `/etc/x` no existe.
6. Común `POST` y `DELETE` → `403`; sin cookie → `401`.
7. `DELETE ?id=<nuevo>` → `200`; el kthread desaparece; agregar `SO2T_NUEVA` a `b.log` no produce alerta en 8 s; el segundo `DELETE` → `404`; `?id=abc` → `400`.
8. Reposo: sin escribir logs, el `voluntary_ctxt_switches` del hilo `ipc` (TID con `comm` = `ipc` en `/proc/<pid>/task/*/comm`) **no cambia** en 5 s. Sin tolerancia.
9. Límite: crear hasta tener 16 (con 1 de configuración, 15 nuevos sobre `a.log`, palabras distintas) → el siguiente da `409` con el texto exacto. Finalizarlos.
10. `DELETE` del logwatch de configuración → `200`; la lista queda vacía.
11. Crear uno nuevo y apagar con `SIGTERM` → código 0; no queda ningún kthread `log_watch_*` que no existiera antes de la prueba.
12. El log del daemon no tiene contraseñas; `~/so2_logs` sigue existiendo.

## 7. Decisiones

| Decisión | Alternativa | Motivo |
|---|---|---|
| Rutas del dashboard solo directamente en el directorio de logs | Rutas libres | El kthread es root: con rutas libres, un admin del dashboard leería `/etc/shadow` (las líneas salen en alertas visibles para todos) o escribiría en `/etc`. Decisión del usuario (2026-10-21). |
| Base = `dirname(realpath(log_central))` | Nueva clave en `daemon.conf` | Con la configuración por defecto es `~/so2_logs`. Así la prueba puede aislarse en un subdirectorio sin claves nuevas. |
| Documentar, no corregir, la falta de permisos de 550/551 | `capable(CAP_SYS_ADMIN)` en el kernel | Evita recompilar el kernel ahora. Decisión del usuario (2026-10-21); va al Manual Técnico en F08. |
| Prohibir que un log central sea vigilado por cualquier logwatch | Solo dentro del mismo logwatch (como hoy en `main.c`) | Cada coincidencia se escribe en la central, que se volvería a leer en un ciclo sin fin. |
| Solo se finalizan logwatch del registro | Cualquier ID del kernel | El kernel no verifica permisos: el daemon no debe detener monitoreos de otros procesos. |
| Sin persistencia de los creados en el dashboard | Guardarlos en un archivo | El enunciado no lo pide; se evita otro archivo escrito por root. |
| Sin alerta al crear o finalizar | Alerta nueva | El catálogo de alertas lo fija el enunciado; queda la línea en el log del daemon. |
| `start`/`stop` inyectables solo para pruebas | Probar solo en la VM | Permite cubrir `ESRCH`, `ENOSYS` y el límite sin root ni kernel. |
