# F01 — Autenticación y roles (PAM) · Diseño

Requisitos: [`requirements.md`](requirements.md). Contrato base de la API: [`../../steering/tech.md`](../../steering/tech.md) §5.3–5.5.

---

## 1. Visión general

```text
navegador ──POST /api/login──► http.c ──► peticion.c (Content-Length, cuerpo)
                                  │
                                  ├─► cJSON (lib/) ─► usuario + password
                                  ├─► auth.c: auth_verificar_pam()   ── PAM "so2-monitor"
                                  ├─► auth.c: auth_rol_de_usuario()  ── getgrouplist()
                                  └─► auth.c: sesion_crear()         ── token getrandom()
                                                   │
navegador ◄── 200 + Set-Cookie so2_sesion ─────────┘

cualquier /api/* ──► permiso_buscar(metodo, ruta) ──► 404 / 405
                 ──► peticion_cookie("so2_sesion") ─► sesion_buscar() ──► 401
                 ──► rol_suficiente(rol, minimo) ─────────────────────► 403
                 ──► handler existente (estado, eventos, proceso, …)
```

| Pieza | Archivo | Requisito |
|---|---|---|
| Grupos, servicio PAM, alta de usuarios | `scripts/setup_usuarios.sh` | R1 |
| PAM, roles, sesiones, tabla de permisos | `ProgramaIntermedio/auth.c` (nuevo) | R2, R3, R4 |
| Encabezados, cookie, `Content-Length` | `ProgramaIntermedio/peticion.c` (nuevo) | R5 |
| POST, rutas, 401/403/411/413, cookies, cierre de SSE | `ProgramaIntermedio/http.c` | R3–R5 |
| Parser JSON de entrada | `ProgramaIntermedio/lib/cJSON.{c,h}` (vendorizado) | R3.1 |
| Login y adaptación por rol | `Dashboard/login.html`, `js/api.js`, `js/auth.js`, `js/app.js`, `index.html`, `css/styles.css` | R6 |
| `SUDO_USER` solo como root | `ProgramaIntermedio/config.c` | R7 |
| Pruebas | `pruebas/test_auth.c`, `pruebas/test_peticion.c`, `scripts/usuarios_prueba.sh`, `scripts/pruebas_auth.sh` | R8 |

## 2. Tipos y prototipos (`comun.h`)

```c
#define TAM_TOKEN		64
#define MAX_USUARIO		32
#define MAX_PASSWORD		128
#define MAX_SESIONES		64
#define SESION_INACTIVIDAD_S	1800
#define COOKIE_SESION		"so2_sesion"
#define SERVICIO_PAM		"so2-monitor"

enum rol { ROL_NINGUNO = 0, ROL_COMUN = 1, ROL_ADMIN = 2 };

const char *rol_nombre(enum rol rol);
enum rol rol_de_grupos(int en_admin, int en_comun);
int rol_suficiente(enum rol rol, enum rol minimo);

void auth_iniciar(void);
int auth_verificar_pam(const char *usuario, const char *password);
enum rol auth_rol_de_usuario(const char *usuario);

int sesion_crear(const char *usuario, enum rol rol, time_t ahora, char token[TAM_TOKEN + 1]);
int sesion_buscar(const char *token, time_t ahora, char usuario[MAX_USUARIO + 1], enum rol *rol);
int sesion_vigente(const char *token, time_t ahora);
void sesion_eliminar(const char *token);
void sesion_purgar(time_t ahora);

int permiso_buscar(const char *metodo, const char *ruta, enum rol *minimo);

int peticion_encabezado(const char *peticion, const char *nombre, char *valor, size_t tam);
int peticion_cookie(const char *peticion, const char *nombre, char *valor, size_t tam);
long peticion_content_length(const char *peticion);
```

`ROL_NINGUNO` funciona también como rol mínimo `publico`.

## 3. `auth.c`

### 3.1 Roles

- `rol_nombre`: `ROL_ADMIN` → `"admin_user"`, `ROL_COMUN` → `"common_user"`, otro → `"ninguno"`.
- `rol_de_grupos(en_admin, en_comun)`: admin si `en_admin`; si no, común si `en_comun`; si no, ninguno (R3.5).
- `rol_suficiente(rol, minimo)`: `rol >= minimo` (R4.7).
- `auth_rol_de_usuario(usuario)`:
  1. `getpwnam(usuario)`; si no existe → `ROL_NINGUNO`.
  2. `getgrouplist(usuario, pw->pw_gid, grupos, &n)` con 64 grupos de entrada; si devuelve `-1`, reintentar con el `n` que reporta.
  3. `getgrnam("admin_user")` y `getgrnam("common_user")`; un grupo inexistente cuenta como "no pertenece".
  4. `rol_de_grupos(...)`.

  Se llama en **cada login**, sin caché: `useradd`/`usermod -aG` hechos con el daemon en marcha se ven en el siguiente intento (R3.7).

### 3.2 PAM

```c
struct credenciales { const char *usuario; const char *password; };

static int conversar(int n, const struct pam_message **mensajes,
		     struct pam_response **respuestas, void *datos);
static void sin_espera(int estado, unsigned int microsegundos, void *datos);
```

- `conversar`: `calloc(n, sizeof(struct pam_response))`; `PAM_PROMPT_ECHO_OFF` → `strdup(password)`; `PAM_PROMPT_ECHO_ON` → `strdup(usuario)`; `PAM_ERROR_MSG`/`PAM_TEXT_INFO` → respuesta `NULL`; otro estilo o `strdup` fallido → liberar lo creado y `PAM_CONV_ERR`.
- `auth_verificar_pam`:
  ```c
  pam_start(SERVICIO_PAM, usuario, &conversacion, &h);
  pam_set_item(h, PAM_FAIL_DELAY, (const void *)sin_espera);
  r = pam_authenticate(h, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK);
  if (r == PAM_SUCCESS) r = pam_acct_mgmt(h, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK);
  pam_end(h, r);
  return r == PAM_SUCCESS ? 0 : -1;
  ```
- **Por qué `sin_espera`:** `pam_unix` pide ~2 s de espera tras un fallo. PAM corre en el hilo HTTP (el mismo que atiende SSE con `poll()`); esa espera congelaría a todos los clientes. Registrar `PAM_FAIL_DELAY` reemplaza la espera por una función vacía (R2.4). La justificación va al Manual Técnico (F08).
- El daemon corre como root, así que `pam_unix` lee `/etc/shadow` directamente. Sin root, `unix_chkpwd` solo valida al propio usuario: de ahí la advertencia de R2.5.

### 3.3 Sesiones

```c
struct sesion {
	int activa;
	char token[TAM_TOKEN + 1];
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t ultimo_uso;
};

static struct sesion sesiones[MAX_SESIONES];
static pthread_mutex_t lock_sesiones = PTHREAD_MUTEX_INITIALIZER;
```

- `sesion_crear`: `getrandom(bytes, 32, 0)` (falla → `-1`); token = hex en minúsculas; ocupa un hueco libre o, si no hay, el de `ultimo_uso` más antiguo (R3.12).
- `sesion_buscar`: compara el token con todos los activos con una comparación de **tiempo constante** (bucle con `|=` de XOR sobre los 64 bytes). Si `ahora - ultimo_uso > SESION_INACTIVIDAD_S` → desactiva y `-1`. Si es válido: renueva `ultimo_uso = ahora`, copia usuario y rol, `0`.
- `sesion_vigente`: como `sesion_buscar` pero **sin** renovar; la usa el barrido de SSE.
- `sesion_eliminar`, `sesion_purgar(ahora)`: desactivan y ponen a cero el token con `explicit_bzero`.
- `ahora` siempre llega como parámetro para probar la expiración sin esperar (R8.1).
- Todas toman `lock_sesiones`. Hoy solo las usa el hilo HTTP, pero el mutex queda listo para F02 (tech.md §5.2).

### 3.4 Tabla de permisos (R4.1)

```c
struct permiso { const char *metodo; const char *ruta; enum rol minimo; };

static const struct permiso permisos[] = {
	{ "POST", "/api/login",   ROL_NINGUNO },
	{ "POST", "/api/logout",  ROL_NINGUNO },
	{ "GET",  "/api/sesion",  ROL_COMUN },
	{ "GET",  "/api/estado",  ROL_COMUN },
	{ "GET",  "/api/eventos", ROL_COMUN },
	{ "GET",  "/api/proceso", ROL_ADMIN },
};
```

`permiso_buscar(metodo, ruta, &minimo)` → `0` si coincide ruta y método; `-2` si la ruta existe con otro método (405); `-1` si la ruta no existe (404). Las features siguientes **solo agregan filas** aquí.

### 3.5 `auth_iniciar` (R2.5, R2.6)

Llamada desde `main.c` antes de `servidor_iniciar`. Solo registra advertencias:
- `geteuid() != 0` → `La autenticacion PAM requiere ejecutar el daemon como root`.
- `access("/etc/pam.d/so2-monitor", R_OK) != 0` o `getgrnam` nulo para algún grupo → `Falta configurar PAM o grupos: ejecuta scripts/setup_usuarios.sh`.

## 4. `peticion.c`

Funciones puras sobre el texto de la petición (sin sockets), probadas en `test_peticion.c`:

- Los encabezados van desde la primera línea después de la de petición hasta `\r\n\r\n`; nunca se busca en el cuerpo.
- `peticion_encabezado`: nombre sin distinguir mayúsculas (`strncasecmp`) y seguido de `:`; recorta espacios del valor; `0` si existe, `-1` si no o si no cabe en `tam`.
- `peticion_cookie`: lee `Cookie`, separa por `;`, recorta espacios y busca `nombre=`. Para `so2_sesion`, `http.c` además exige 64 caracteres hexadecimales; si no, se trata como "sin cookie".
- `peticion_content_length`: `strtol` en base 10 sobre todo el valor; `-1` si falta, no es numérico, es negativo o hay basura después.

## 5. `http.c`

### 5.1 Cliente

```c
struct cliente {
	...lo de P4...
	size_t fin_encabezados;
	char token[TAM_TOKEN + 1];
	char cookie_salida[160];
};
```

- `token`: sesión con la que se abrió el SSE (vacío en clientes normales).
- `cookie_salida`: si no está vacío, `responder()` agrega `Set-Cookie: <valor>\r\n`.
- `responder()` deja de enviar `Access-Control-Allow-Origin: *`; tampoco lo envía `atender_sse()` (R5.5).
- `texto_de_codigo()` agrega `401 Unauthorized`, `403 Forbidden`, `411 Length Required`, `413 Payload Too Large`.

### 5.2 Lectura (`cliente_leer`)

1. Al encontrar `\r\n\r\n`, guarda `fin_encabezados` (offset del cuerpo).
2. Si el método es `POST`: `largo = peticion_content_length()`; `< 0` → 411; `fin_encabezados + largo > TAM_PETICION - 1` → 413; si aún no llegan `largo` bytes, `return` y espera más datos.
3. Con la petición completa → `atender(c)`.
4. Se conserva el `431` cuando el búfer se llena sin `\r\n\r\n`.

### 5.3 Despacho (`atender`)

```text
sscanf método y ruta (400 si falla)
separar ?consulta
si ruta empieza con "/api/":
    r = permiso_buscar(método, ruta, &mínimo)
    r == -1 → 404 "Recurso no encontrado"
    r == -2 → 405 "Metodo no permitido"
    si mínimo > ROL_NINGUNO:
        sin cookie válida o sesion_buscar() falla → 401 "Sesion requerida"
        !rol_suficiente(rol, mínimo)          → 403 "Permiso denegado"
    rutear al handler (cadena if/else de P4 + login, logout, sesion)
si no:
    método != GET → 405 "Metodo no permitido"
    atender_estatico()
```

La cookie se lee antes de rutear para tener `usuario`/`rol` disponibles en el handler.

### 5.4 Handlers nuevos

- **`atender_login(c, cuerpo, largo)`** (R3.1–R3.8):
  1. `cJSON_ParseWithLength(cuerpo, largo)`; validar objeto, `usuario`/`password` como cadena y las reglas de R3.2. Si falla → 400, sin PAM.
  2. Copiar a `char usuario[MAX_USUARIO + 1]`, `char password[MAX_PASSWORD + 1]`; `explicit_bzero` sobre el `valuestring` de `password` y `cJSON_Delete`.
  3. `auth_verificar_pam`; `explicit_bzero(password)` inmediatamente después, haya salido bien o mal.
  4. Falla → `registrar("Login fallido: %s")`, 401.
  5. `rol = auth_rol_de_usuario()`; `ROL_NINGUNO` → `registrar("Login sin grupo: %s")`, 403.
  6. `sesion_crear(usuario, rol, time(NULL), token)`; si falla → 500 `{"error":"No se pudo crear la sesion"}`.
  7. `cookie_salida = "so2_sesion=<token>; Path=/; HttpOnly; SameSite=Strict"`; `registrar("Login exitoso: %s (%s)")`; 200 `{"usuario":…,"rol":…}` con `json_cadena`.
- **`atender_logout(c)`** (R3.10): si hay cookie válida en forma, `sesion_eliminar(token)` y cerrar todo cliente SSE con ese `token`; `cookie_salida = "so2_sesion=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"`; 200 `{"ok":true}`.
- **`atender_sesion(c, usuario, rol)`** (R3.9): 200 `{"usuario":…,"rol":…}`.
- **`atender_sse`**: copia el token de la sesión a `c->token`.

### 5.5 Expiración de SSE (R3.11)

En `servidor_bucle`, en cada vuelta (`poll` despierta como máximo cada 15 s): `sesion_purgar(ahora)` y luego cerrar todo cliente con `sse && token[0] && !sesion_vigente(token, ahora)`. El dashboard consulta `/api/estado` cada pocos segundos, y eso renueva la sesión mientras la pestaña esté abierta.

## 6. `lib/cJSON`

- Vendorizar **cJSON v1.7.18** (MIT) desde `https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c` y `cJSON.h` a `ProgramaIntermedio/lib/`, **sin modificar**. Anotar el `sha256sum` de ambos en la evidencia de la tarea.
- Excepción a "sin comentarios": `lib/` es código de terceros y conserva su licencia y sus comentarios (actualizado en `AGENTS.md` §5).
- Si cJSON produce warnings con `-Wall -Wextra`, **no** se silencian en silencio: se reporta en "Dudas para el revisor".

## 7. `Makefile`

```make
CFLAGS = -Wall -Wextra -pthread -O2 -D_GNU_SOURCE -Ilib
LDLIBS = -lpam
FUENTES = main.c metricas.c ipc.c http.c json.c config.c sensor.c auth.c peticion.c lib/cJSON.c

$(PROGRAMA): $(OBJETOS)
	$(CC) $(CFLAGS) -o $@ $(OBJETOS) $(LDLIBS)

pruebas: config.o auth.o peticion.o pruebas/test_config.c pruebas/test_auth.c pruebas/test_peticion.c
	… compila y ejecuta test_config, test_auth (enlaza auth.o -lpam) y test_peticion
```

`clean` borra también `lib/cJSON.o` y los binarios de prueba nuevos. Cada prueba define su propio `registrar()` como `test_config.c`.

## 8. `config.c` (R7)

```c
if (geteuid() == 0 && sudo_user && *sudo_user) { …getpwnam… }
```

Prueba nueva en `test_config.c` (corre como usuario normal): con `SUDO_USER=root` y `HOME=<tmp>`, `~/x` se expande a `<tmp>/x`.

## 9. Dashboard

| Archivo | Cambio |
|---|---|
| `login.html` | Formulario `#login-formulario` (`#login-usuario`, `#login-password`, botón) y `<p id="login-error">`. Mismo `styles.css`. Carga `js/api.js` y `js/auth.js`. |
| `js/api.js` | Nuevo. `api.get(ruta)`, `api.post(ruta, cuerpo)` → `fetch` del mismo origen con `cache: 'no-store'`; si `status === 401` y la página no es `login.html` → `location.href = '/login.html'`. `api.sesion()` → `GET /api/sesion`. |
| `js/auth.js` | Nuevo. En `login.html`: envía el formulario con `api.post('/api/login', …)`; `200` → `location.href = '/'`; otro código → muestra `error` y conserva el usuario (R6.2). En `index.html`: `iniciarSesionUI()` pinta usuario/rol, conecta **Cerrar sesión** y oculta `[data-rol="admin_user"]` si el rol es `common_user`. |
| `index.html` | Bloque de usuario en el encabezado (`#sesion-usuario`, `#sesion-rol`, `#boton-logout`); `data-rol="admin_user"` en `#panel-pid`; carga `api.js` y `auth.js` antes de `app.js`. |
| `js/app.js` | Quitar la constante `API` y el `?api=`; usar `api.get`; antes de arrancar, `await api.sesion()` + `iniciarSesionUI()`; en `EventSource.onerror`, llamar `api.sesion()` (si la sesión expiró, redirige). |
| `css/styles.css` | Estilos mínimos para la tarjeta de login y el bloque de sesión, con las variables existentes. |

## 10. Scripts

### 10.1 `setup_usuarios.sh` (R1)

```bash
sudo scripts/setup_usuarios.sh                      # grupos + PAM + libpam0g-dev
sudo scripts/setup_usuarios.sh agregar ana admin    # useradd -m si falta, usermod -aG admin_user, passwd ana
sudo scripts/setup_usuarios.sh agregar luis comun
```

`/etc/pam.d/so2-monitor` se escribe completo cada vez (idempotente):

```text
auth    required pam_unix.so
account required pam_unix.so
```

### 10.2 `usuarios_prueba.sh` (librería, se usa con `source`)

- `usuarios_prueba_crear`: crea `so2t_admin` (`admin_user`), `so2t_comun` (`common_user`), `so2t_ambos` (ambos) y `so2t_nadie` (ninguno) con `useradd -M -s /usr/sbin/nologin`; contraseña aleatoria por usuario (`head -c 18 /dev/urandom | base64 | tr -d '/+='`) asignada con `chpasswd` por stdin. Las guarda **solo** en un arreglo asociativo `CLAVE[...]` en memoria.
- `usuarios_prueba_agregar <usuario> <grupo|->`: crea un usuario de prueba más (lo usa la prueba "alta en caliente").
- `usuarios_prueba_borrar`: `userdel` de cada `so2t_*` creado en esta ejecución; nunca toca otros usuarios.
- `login_json <usuario>`: imprime `{"usuario":"…","password":"…"}` para `curl --data @-` (por stdin, para que la clave no aparezca en `ps`).
- Condición previa: grupos y `/etc/pam.d/so2-monitor` existen; si no, falla indicando `sudo scripts/setup_usuarios.sh`.

### 10.3 `pruebas_auth.sh` (R8.2)

Requiere root y la VM. `trap` que detiene el daemon y llama a `usuarios_prueba_borrar`. Carpeta de trabajo en el home de `SUDO_USER` (`~/so2_logs/pruebas_auth`), con `chown -R` al final. Puerto `8098`.

| # | Verificación | Esperado |
|---|---|---|
| 1 | `GET /` y `GET /login.html` sin cookie | 200 |
| 2 | `GET /api/estado`, `/api/eventos`, `/api/sesion` sin cookie | 401 |
| 3 | Login `so2t_admin` | 200, `rol` = `admin_user`, `Set-Cookie` con `HttpOnly` y `SameSite=Strict` |
| 4 | Login `so2t_ambos` | 200, `admin_user` |
| 5 | Login `so2t_comun` | 200, `common_user` |
| 6 | Login `so2t_nadie` con clave correcta | 403, sin `Set-Cookie` |
| 7 | Clave incorrecta / usuario inexistente | 401 con cuerpo idéntico |
| 8 | Login fallido | `time_total` < 1 s |
| 9 | JSON inválido, sin `password`, usuario de 33 caracteres | 400 |
| 10 | `POST` sin `Content-Length` / cuerpo > búfer | 411 / 413 |
| 11 | Común: `/api/estado` 200, `/api/sesion` = `common_user`, SSE 200, `/api/proceso?pid=1` | 403 |
| 12 | Admin: `/api/proceso?pid=1` | 200 |
| 13 | Cookie con token inventado (64 hex) | 401 |
| 14 | `PUT /api/estado` / `GET /api/login` / `GET /api/noexiste` | 405 / 405 / 404 |
| 15 | Logout: 200; la misma cookie → 401; el SSE abierto con esa cookie termina | ✓ |
| 16 | Alta en caliente: `usuarios_prueba_agregar so2t_nuevo common_user` con el daemon en marcha → login | 200 |
| 17 | La salida del daemon contiene `Login exitoso` y `Login fallido` y **no** contiene ninguna contraseña de prueba | ✓ |
| 18 | El daemon sigue vivo y `SIGTERM` lo detiene con código 0 | ✓ |

### 10.4 `pruebas_integracion.sh` y `demo.sh` (R8.3, R8.4)

- `pruebas_integracion.sh`: exige root (`Ejecuta con sudo`); `DIR` en el home de `SUDO_USER`; `source usuarios_prueba.sh`; inicia sesión con `so2t_admin` en un *cookie jar* temporal y agrega `-b` a todos los `curl` y a `codigo_http`. Conserva las 38 verificaciones de F00 y al final hace `chown -R` de `DIR` al usuario.
- `demo.sh`: al inicio pide usuario (`read -r`) y contraseña (`read -rs`), hace login en un *cookie jar* creado con `mktemp` (borrado en `trap`) y usa `-b` en sus consultas. Si el login falla, muestra el `error` y termina.

## 11. Decisiones

| Decisión | Alternativa descartada | Motivo |
|---|---|---|
| Cookie `HttpOnly` + `SameSite=Strict` | Token en `localStorage` + encabezado | `EventSource` no envía encabezados propios; la cookie llega sola y JS no puede leerla. |
| Rol fijado al iniciar sesión | Recalcular grupos en cada petición | Simple y predecible; el cambio se aplica al volver a entrar (R3.13). |
| PAM en el hilo HTTP con `PAM_FAIL_DELAY` vacío | Hilo aparte por login | Un login dura milisegundos sin la espera; un hilo por login complica el servidor sin ganancia. |
| Archivos estáticos públicos | Proteger `index.html` | No contienen datos; toda la información viaja por la API protegida. |
| Sin bloqueo por intentos | Bloqueo tras N fallos | No lo pide el enunciado; queda anotado como mejora. |
