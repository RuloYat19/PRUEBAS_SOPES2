# F06 — Antivirus, blacklist y cuarentena · Diseño

Requisitos: [`requirements.md`](requirements.md). Base: F05 (`analisis.c`, `sensor.c`, hilo `archivos`, listado y generación), F02 (`alerta_emitir` y sus motivos `MOT_HASH_MALICIOSO`, `MOT_ARCHIVO_CUARENTENA` y `MOT_BLACKLIST_ACTUALIZADA`), F01 (permisos, sesiones, límite de petición).

---

## 1. Visión general

```text
arranque: blacklist_iniciar(conf.blacklist) → sensor_activar() → hilo archivos
hilo archivos (por evento, análisis inicial o re-evaluación):
    abrir_sin_enlaces (openat2 RESOLVE_NO_SYMLINKS) → fstat → SHA-256 → blacklist_buscar(sha)
        ├─ no está → F05 (Archivo modificado LOW si cambió)
        └─ está    → infectado + "Hash malicioso detectado" HIGH + amenaza
                     → fchmod(fd, 0000) → "Archivo puesto en cuarentena" HIGH + registro de cuarentena
    si blacklist_version() cambió y el escaneo está activo → analisis_reevaluar
hilo HTTP:
    POST /api/blacklist  → (búfer de 1 MiB solo para admin) → blacklist_actualizar → alerta MEDIUM → archivos_despertar()
    POST /api/cuarentena → analisis_cuarentena_manual → alerta HIGH
    GET  /api/blacklist | /api/amenazas | /api/cuarentena
    fd_aviso → difundir_amenazas ("event: amenaza")
```

| Pieza | Archivo | Requisito |
|---|---|---|
| Validación, tabla hash, persistencia | `ProgramaIntermedio/blacklist.c` (nuevo) | R1, R2 |
| Firmas de demostración | `ProgramaIntermedio/blacklist.json` (nuevo) | R1.4 |
| Detección, cuarentena, amenazas, re-evaluación | `ProgramaIntermedio/analisis.c` | R3, R4 |
| Disparo de la re-evaluación | `ProgramaIntermedio/sensor.c` (`hilo_archivos`) | R3.5 |
| Búfer grande, endpoints, difusión | `ProgramaIntermedio/http.c`, filas en `auth.c` | R2.3–R2.6, R4, R5 |
| JSON | `ProgramaIntermedio/json.c` | R2.6, R4.5, R5 |
| Arranque y apagado | `ProgramaIntermedio/main.c`, `Makefile` | R2.1 |
| Dashboard | `Dashboard/index.html`, `js/antivirus.js` (nuevo), `js/archivos.js`, `js/app.js`, `css/styles.css` | R6 |
| Pruebas y demo | `pruebas/test_blacklist.c`, `pruebas/test_analisis.c`, `scripts/pruebas_antivirus.sh`, `scripts/demo_antivirus.sh`, `scripts/correr_pruebas.sh` | R7 |

## 2. `blacklist.c`

```c
#define MAX_FIRMAS		5000
#define MAX_CUERPO_BLACKLIST	(1024 * 1024)

struct firma {
	char sha256[65];
	char nombre[65];
	char descripcion[257];
};

struct info_blacklist {
	char nombre[65];
	int version;
	char actualizado[33];
	size_t firmas;
	char origen[16];
	char actualizada_por[MAX_USUARIO + 1];
	char cargada[32];
	char error[160];
};

int blacklist_iniciar(const char *ruta);
int blacklist_validar(const char *json, size_t largo, struct tabla_firmas **tabla,
		      struct info_blacklist *info, char *error, size_t tam_error);
int blacklist_actualizar(const char *json, size_t largo, const char *usuario,
			 size_t *firmas, char *error, size_t tam_error);
int blacklist_buscar(const char *sha256, struct firma *destino);
unsigned long blacklist_version(void);
void blacklist_info(struct info_blacklist *destino);
size_t blacklist_lista(struct firma *destino, size_t max);
void tabla_firmas_liberar(struct tabla_firmas *t);
void blacklist_finalizar(void);
```

- **Tabla** (`struct tabla_firmas`, opaca): arreglo `struct firma *entradas` de `n` elementos e índice de direccionamiento abierto `unsigned int *ranuras`. La capacidad es la primera potencia de 2 ≥ 2n; cada ranura guarda índice + 1 (0 = vacía). Hash: los primeros 16 caracteres hex del SHA-256 como `uint64_t`, `& (capacidad - 1)`, con sondeo lineal.
- **Estado global** bajo `lock_blacklist`: `struct tabla_firmas *vigente`, `struct info_blacklist info`, `unsigned long version`, `char ruta[PATH_MAX]`. `lock_blacklist` es una **hoja**: dentro de él no se toma ningún otro lock ni se hace E/S.
- **`blacklist_validar`**: `cJSON_ParseWithLength` y luego las reglas de R1.2, en orden, con los textos exactos de R1.3.
  - Las claves se recorren con `cJSON_ArrayForEach` sobre los hijos (`hijo->string`). cJSON acepta claves repetidas: una repetida cuenta como desconocida (`Clave desconocida: <clave>` / `Firma <n>: clave desconocida <clave>`).
  - `sha256`: `strlen == 64` y `isxdigit` en todos; se copia en minúsculas.
  - Las longitudes son en bytes.
  - Los duplicados se detectan al insertar en la tabla nueva.
  - Si todo es válido, devuelve la tabla y la `info` (`origen` lo fija quien llama).
- **`blacklist_iniciar(ruta)`**: guarda la ruta. Lee el archivo (máximo `MAX_CUERPO_BLACKLIST`; si es más grande → error `Archivo demasiado grande`) y valida.
  - Si es válido: `vigente`, `origen = "archivo"`, `version = 1`.
  - Si no: tabla vacía, `info.error = "<ruta>: <detalle>"`, `registrar("blacklist no cargada: …")` y `version = 1`.
  - Siempre devuelve 0: el daemon nunca termina por esto.
- **`blacklist_actualizar(json, largo, usuario, …)`**:
  1. `blacklist_validar` (fuera del lock) → si falla, -1 con el detalle;
  2. `guardar_atomico(ruta, json, largo)` → si falla, -2;
  3. bajo el lock: intercambiar `vigente`, actualizar `info` (`origen = "subida"`, `actualizada_por`, `cargada = ahora`, `error = ""`) y hacer `version++`;
  4. liberar la tabla anterior **fuera** del lock;
  5. devolver 0 y `*firmas`.
- **`guardar_atomico(ruta, datos, largo)`**:
  - `mkstemp("<dir>/.blacklist.XXXXXX")` en el mismo directorio;
  - `write` completo (reintenta `EINTR` y escrituras parciales) y `fsync`;
  - si `stat(ruta)` existe → `fchown(fd, st_uid, st_gid)` y `fchmod(fd, st_mode & 07777)`; si no → `fchmod(fd, 0644)`;
  - `close` y `rename(tmp, ruta)`;
  - ante cualquier error, `unlink(tmp)` y -1. Así un `sudo` no deja el archivo del repositorio con dueño root.
- **`blacklist_buscar`**: bajo el lock, busca y copia la firma a `destino`; devuelve 1 si la encontró y 0 si no.

`blacklist.json` del repositorio: las 4 firmas de R1.4, con `"nombre": "Base local de firmas SO2"`, `"version": 1` y `"actualizado": "2026-10-21"`. Ejemplo de descripción: `"Muestra de demostración que simula un troyano: texto plano inofensivo."`.

## 3. `analisis.c`

### 3.1 Tipos nuevos (`comun.h`)

```c
#define MAX_AMENAZAS		100
#define MAX_CUARENTENA		256

enum contexto_analisis { CTX_EVENTO, CTX_INICIAL, CTX_REEVALUACION };
enum resultado_cuarentena { CUARENTENA_OK = 0, CUARENTENA_NO_LISTADO = -1,
			    CUARENTENA_YA = -2, CUARENTENA_ERROR = -3 };

struct amenaza {
	unsigned long id;
	char firma[65];
	char descripcion[257];
	char ruta[SENSOR_RUTA_MAX];
	char sha256[65];
	unsigned int pid;
	char proceso[SENSOR_COMM_MAX];
	char fecha[32];
	int cuarentena;
};

struct registro_cuarentena {
	char ruta[SENSOR_RUTA_MAX];
	char sha256[65];
	char tipo[12];
	char motivo[65];
	char usuario[MAX_USUARIO + 1];
	char fecha[32];
	char permisos_anteriores[8];
};
```

`struct archivo_analizado` suma `char firma[65]` e `int cuarentena`.

### 3.2 Pipeline común

`static void analizar_abierto(int fd, const struct stat *st, const char *ruta, const char *sha, enum contexto_analisis ctx, unsigned int pid, const char *proceso, struct resultado *r)`, llamado con el descriptor **todavía abierto** desde `analisis_procesar` (`CTX_EVENTO`), `analisis_linea_base` (`CTX_INICIAL`) y `analisis_reevaluar` (`CTX_REEVALUACION`):

1. `infectado = blacklist_buscar(sha, &firma)`.
2. `ya_cuarentena = (st->st_mode & 07777) == 0`.
3. Si `infectado && !ya_cuarentena`: `fchmod(fd, 0)`.
   - Si funciona: `cuarentena = 1` y se guardan `permisos_anteriores` (`%o` de `st_mode & 07777`).
   - Si falla: `registrar` y `cuarentena = 0`.
4. Si `ya_cuarentena`: `cuarentena = 1`, sin `fchmod` ni registro nuevo.
5. El resultado se aplica al listado bajo `lock_archivos` (misma comprobación de generación de F05). Campos: `estado`, `firma`, `cuarentena`, `sha256`, `mtime`, `tamano`, y `accion`/`pid`/`proceso` según el contexto.
6. Alertas y registros, **fuera** de `lock_archivos` y en este orden:
   - si `infectado` y la entrada **no** estaba ya infectada con el mismo SHA-256: amenaza (`id` creciente) + `MOT_HASH_MALICIOSO`, con el sufijo según el contexto (R3.1.2);
   - si `fchmod` se aplicó en este paso: registro de cuarentena (`tipo = "automatica"`, `motivo = firma.nombre`) + `MOT_ARCHIVO_CUARENTENA` con `Archivo <ruta> puesto en cuarentena automática (firma <firma>)`;
   - si **no** `infectado`, en `CTX_EVENTO` y el hash o el `mtime` cambiaron: `MOT_ARCHIVO_MODIFICADO` como en F05;
   - con `CTX_INICIAL` y `CTX_REEVALUACION` nunca hay `Archivo modificado`;
   - `eventfd_write(fd_aviso, 1)` si cambió algo.
7. Después, quien llamó cierra el descriptor.

El "si cambió" de F05 sigue igual para la entrada. Una detección cuenta como cambio aunque el hash no haya cambiado (por ejemplo, en la re-evaluación).

### 3.3 Re-evaluación y cuarentena manual

- **`analisis_reevaluar(dir, gen)`**: copia las rutas del listado bajo el lock. Por cada una: comprueba el directorio, `abrir_sin_enlaces(ruta, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)`, `fstat` regular, SHA-256 y `analizar_abierto(…, CTX_REEVALUACION, 0, "")`. Si ya no coincide con la blacklist: `estado = benigno` y `firma = ""`; la `cuarentena` sale del `fstat` (no se restaura). Un archivo que ya no existe se deja como está.
- **`analisis_cuarentena_manual(ruta, usuario, …)`**:
  1. con `lock_archivos`, comprueba que `ruta` sea **exactamente** una entrada del listado y copia el directorio publicado del sensor;
  2. comprueba `ruta_en_directorio(ruta, dir + "/", …)`;
  3. `open` con las banderas de F05, `fstat` y `S_ISREG` → si algo falla, `CUARENTENA_NO_LISTADO`;
  4. `(st_mode & 07777) == 0` → `CUARENTENA_YA`;
  5. `fchmod(fd, 0)` → si falla, `CUARENTENA_ERROR`;
  6. registro (`tipo = "manual"`, `motivo = "manual"`, `usuario`) + `MOT_ARCHIVO_CUARENTENA` con `Archivo <ruta> puesto en cuarentena manual por <usuario>`;
  7. con `lock_archivos`: `cuarentena = 1` y `seq = ++ultimo_seq` en la entrada; `eventfd_write(fd_aviso, 1)`.

  Corre en el hilo HTTP: es una sola apertura y un `fchmod`, sin hash.
- **Listas**: `amenazas[MAX_AMENAZAS]` y `cuarentena[MAX_CUARENTENA]`, circulares bajo `lock_archivos`. `analisis_reiniciar` **no** las vacía. API: `analisis_amenazas(destino, max)` (recientes primero), `analisis_amenazas_desde(ultimo_id, destino, max)` (ascendente) y `analisis_cuarentena(destino, max)` (recientes primero).
- **Desalojo del listado** (F05 R4.7): igual que antes; las listas de amenazas y cuarentena son aparte, así que no se pierde nada de lo que pide R4.5.

### 3.4 `sensor.c`

En `hilo_archivos`, junto a `gen_analizada`, va `unsigned long ver_evaluada`:
- antes de `analisis_linea_base`, `ver_evaluada = blacklist_version()` (la línea base ya usa la blacklist vigente);
- después, si `activo && blacklist_version() != ver_evaluada` → `ver = blacklist_version(); analisis_reevaluar(dir, gen); ver_evaluada = ver`.

El HTTP llama a `archivos_despertar()` después de un `blacklist_actualizar` exitoso.

## 4. `http.c`, `auth.c` y JSON

### 4.1 Búfer grande solo para `POST /api/blacklist`

`struct cliente`: `char peticion_fija[TAM_PETICION]; char *peticion; size_t capacidad;`. `peticion` apunta a `peticion_fija` y `capacidad = TAM_PETICION`. Todos los `sizeof(c->peticion)` pasan a `c->capacidad`. `cliente_cerrar` (y cualquier reinicio del cliente) libera `peticion` si no es `peticion_fija` y la vuelve a apuntar a `peticion_fija`.

En `leer`, con los encabezados completos, `POST` y `Content-Length` válido:
1. Si `fin_encabezados + largo` cabe en `capacidad` → sigue igual que en F01.
2. Si no cabe, la ruta de la línea de petición es exactamente `/api/blacklist` y `largo <= MAX_CUERPO_BLACKLIST`:
   - extraer la cookie y llamar a `sesion_buscar`;
   - sin sesión → `401 No autenticado`; con rol menor a admin → `403` (los mismos textos que F01); en ambos casos `cliente_vaciar` sin leer más;
   - si es admin → `malloc(fin_encabezados + largo + 1)`, copiar lo leído, `peticion` = el nuevo búfer y `capacidad` = el nuevo tamaño (si `malloc` falla → `500 Memoria insuficiente`).
3. Si no → `413 Peticion demasiado grande` (como en F01).

Después, el flujo normal (`atender` → `permiso_buscar`) vuelve a validar el permiso.

### 4.2 Filas y endpoints (21 filas en total)

```c
{ "GET",  "/api/blacklist",          ROL_ADMIN },
{ "POST", "/api/blacklist",          ROL_ADMIN },
{ "GET",  "/api/amenazas",           ROL_COMUN },
{ "GET",  "/api/cuarentena",         ROL_ADMIN },
{ "POST", "/api/cuarentena",         ROL_ADMIN },
```

- **`atender_blacklist_post(c, usuario)`**: el cuerpo son los `Content-Length` bytes después de los encabezados; `blacklist_actualizar`.
  - -1 → 400 `Blacklist invalida: <detalle>`; -2 → 500 `No se pudo guardar la blacklist`.
  - Si sale bien: alerta `MOT_BLACKLIST_ACTUALIZADA` con `Blacklist actualizada por %s: %zu firmas`, `archivos_despertar()` y 200 `{"firmas":n,"nombre":…}`.
- **`atender_blacklist_get`**: `json_blacklist` (`blacklist_info` + `blacklist_lista` en un arreglo de `malloc`).
- **`atender_amenazas`**: `{"amenazas":[…]}`.
- **`atender_cuarentena_get`**: `{"cuarentena":[…]}`.
- **`atender_cuarentena_post(c, usuario)`**: `ruta` debe ser `cJSON_IsString` → `analisis_cuarentena_manual` → códigos y textos de R4.1.
- **`difundir_amenazas()`**: patrón `*_desde` con `static unsigned long ultima_amenaza`, una trama `event: amenaza` por entrada.
- **JSON**: `json_archivo` suma `"firma"` y `"cuarentena"` (12 claves). Nuevos: `json_amenaza`, `json_registro_cuarentena` y `json_blacklist` (R2.6).

### 4.3 `main.c`

`blacklist_iniciar(conf.blacklist)` antes de `sensor_iniciar`/`sensor_activar`; `blacklist_finalizar()` después de `sensor_finalizar()`. Makefile: `blacklist.c` en `FUENTES` y `blacklist.o` en las pruebas que enlazan `analisis.o`.

## 5. Dashboard

| Archivo | Cambio |
|---|---|
| `index.html` | `#panel-amenazas` (todos): tabla Firma · Archivo · Descripción · Fecha · Cuarentena. `#panel-cuarentena` (`data-rol="admin_user"`): tabla Archivo · Tipo · Motivo · Usuario · Fecha · Permisos anteriores. Bloque `#base-firmas` (`data-rol="admin_user"`) con nombre, versión, fecha, cantidad, `#blacklist-error`, `<input type="file" accept=".json,application/json">`, botón **Actualizar blacklist** y `<p id="blacklist-aviso" aria-live="polite">`. Carga `js/antivirus.js`. |
| `js/antivirus.js` | Nuevo. `const antivirus = { cargarAmenazas(), agregarAmenaza(a), cargarCuarentena(), cargarBlacklist(), conectarControles(), alAlerta(a) }`. La subida lee el archivo con `file.text()`, rechaza archivos de más de 1 MiB en el cliente (con mensaje) y hace `fetch('/api/blacklist', {method:'POST', headers:{'Content-Type':'application/json'}, body: texto})` **sin** `JSON.parse` previo, para que la validación sea la del daemon. Después de un 200: `cargarBlacklist()`. `alAlerta(a)`: si `a.motivo === 'Archivo puesto en cuarentena'` y es admin → `cargarCuarentena()`. Solo `textContent`. |
| `js/archivos.js` | Estado `infectado` en rojo con la firma; marca "en cuarentena"; botón **Cuarentena** (solo admin, deshabilitado si `cuarentena`), con el `confirm()` exacto de R6.3 y luego `api.post('/api/cuarentena', {ruta})`; muestra el resultado o el `error`. |
| `js/app.js` | `onopen` → `antivirus.cargarAmenazas()` y, si es admin, `cargarCuarentena()` y `cargarBlacklist()`. `addEventListener('amenaza', …)`. El manejador de `alerta` también llama a `antivirus.alAlerta(a)`. |
| `css/styles.css` | `infectado` y la marca de cuarentena con `--critico`; botón **Cuarentena**; tablas nuevas con el estilo de las existentes. |

## 6. Pruebas

### 6.1 `pruebas/test_blacklist.c` (sin root)

1. Válido: las 4 firmas de `blacklist.json` (leído del repositorio) → 4 firmas, `nombre` y `version` correctos.
2. Cada texto de R1.3 con un JSON mínimo que lo provoca (12 casos), comparando el mensaje exacto.
3. Mayúsculas en `sha256` → se busca en minúsculas.
4. Límites: 5000 firmas válidas → OK; 5001 → error; `nombre` de 64 bytes → OK, 65 → error; `descripcion` de 256 → OK, 257 → error.
5. `blacklist_buscar`: acierto con copia de la firma y fallo; tabla vacía.
6. `blacklist_actualizar` con un archivo en `mkdtemp`:
   - inválido → el archivo en disco queda igual (mismo contenido) y `version` no cambia;
   - válido → el contenido en disco es igual al cuerpo, conserva el modo (`0640` antes → `0640` después) y `version` aumenta.
7. `blacklist_iniciar` con un archivo inexistente → 0 firmas, `info.error` no vacío y devuelve 0.

### 6.2 `pruebas/test_analisis.c` (casos nuevos, sin root, sobre archivos propios)

9. Evento con contenido de `SO2.Demo.Troyano` → `infectado`, `firma`, `cuarentena` 1, `stat` modo 0; dos alertas HIGH en orden (`Hash malicioso detectado` y luego `Archivo puesto en cuarentena`) con los textos exactos; ninguna `Archivo modificado`; una amenaza y un registro de cuarentena con `permisos_anteriores` `644`.
10. Línea base con una muestra → sufijo `(análisis inicial)` y cuarentena.
11. Re-evaluación: archivo benigno en el listado; blacklist nueva que incluye su hash → infectado y en cuarentena con `(actualización de blacklist)`, sin `Archivo modificado`. Luego una blacklist sin ese hash → `benigno`, `firma` vacía, `cuarentena` sigue en 1.
12. Archivo ya en `0000` e infectado → sin `fchmod` nuevo ni registro de cuarentena duplicado.
13. Cuarentena manual:
    - ruta del listado → OK, modo 0, alerta `manual por prueba`;
    - otra vez → `YA`;
    - ruta fuera del listado (un archivo del `mkdtemp` padre) → `NO_LISTADO` y su modo **no** cambia;
    - enlace simbólico dentro del directorio → `NO_LISTADO`.
14. `analisis_reiniciar` no vacía las amenazas ni la cuarentena.
15. `json_archivo` tiene 12 claves; `json_amenaza` y `json_registro_cuarentena` se parsean con cJSON.

`test_auth.c`: 21 filas.

### 6.3 `scripts/pruebas_antivirus.sh` (sudo, VM)

Reutiliza `usuarios_prueba.sh` con logins por stdin, en el puerto **8093**. Directorio vigilado: `~/so2_vigilado/pruebas_antivirus`, con el mismo manejo seguro que F05 (aborta si existe antes de crear usuarios y borra solo lo suyo). Blacklist: copia de `ProgramaIntermedio/blacklist.json` en `~/so2_logs/pruebas_antivirus/blacklist.json`, y `daemon.conf` de prueba apuntando a ella. Las escrituras se hacen como `$SUDO_USER`. Al final se comprueba que `ProgramaIntermedio/blacklist.json` tiene el mismo `sha256sum` que al empezar.

| # | Verificación | Esperado |
|---|---|---|
| 1 | Arranque con `muestra_previa.txt` (contenido de la firma 3) ya en el directorio | alertas HIGH con `(análisis inicial)`; `stat -c %a` → `0` |
| 2 | `GET /api/blacklist`: admin / común / sin cookie | 200 con 4 firmas y `origen` `archivo` / 403 / 401 |
| 3 | Muestra 1 creada con `python3 -I` (imprime su PID) | en ≤ 2 s: `Hash malicioso detectado` HIGH con `Firma SO2.Demo.Troyano detectada en <ruta> (PID <pid>, python3)` y después `Archivo puesto en cuarentena` HIGH; ninguna `Archivo modificado` para esa ruta; modo `0`; `sudo -u $SUDO_USER cat` falla |
| 4 | Listado, amenazas y SSE | entrada `infectado`, `firma`, `cuarentena` true; `GET /api/amenazas` (común) la incluye; `event: amenaza` en el SSE del común |
| 5 | `GET /api/cuarentena`: admin / común | 200 con `tipo` `automatica`, `motivo` `SO2.Demo.Troyano`, `permisos_anteriores` `644` / 403 |
| 6 | Archivo benigno | solo `Archivo modificado` LOW; modo intacto |
| 7 | Subidas inválidas: JSON mal formado, sin `firmas`, `sha256` corto, duplicado, clave desconocida, `firmas` vacío | 400 con `Blacklist invalida: <texto exacto>`; `GET /api/blacklist` igual; `sha256sum` del archivo de prueba igual |
| 8 | Límites de la subida | común → 403; sin cookie → 401; cuerpo de 1 MiB + 1 → 413; otra ruta (`/api/ruta`) con 8 KiB → 413 |
| 9 | Subida válida con las 4 firmas + el hash de `benigno.txt` (ya en el listado) | 200 `{"firmas":5}`; alerta MEDIUM `Blacklist actualizada por so2t_admin: 5 firmas`; en ≤ 2 s `benigno.txt` queda en cuarentena con `(actualización de blacklist)`; el archivo de prueba en disco es igual al cuerpo y conserva dueño y modo |
| 10 | Cuarentena manual | común → 403; admin sobre un archivo benigno del listado → 200, modo 0, alerta `manual por so2t_admin`; otra vez → 409; `{"ruta":"/etc/hostname"}` → 404 y `stat -c %a /etc/hostname` sin cambio; enlace dentro del directorio → 404 |
| 11 | Sensor desactivado y muestra 2 creada | sin detección; al reactivar, la línea base la detecta y la pone en cuarentena |
| 12 | Reinicio del daemon con la misma configuración | `GET /api/blacklist` → 5 firmas, `origen` `archivo` |
| 13 | `SIGTERM`, log sin contraseñas, `blacklist.json` del repositorio intacto | código 0 |

### 6.4 `scripts/demo_antivirus.sh` (sin root)

Crea en `~/so2_vigilado` (o en el directorio pasado como argumento) `troyano.txt`, `ransomware.txt` y `keylogger.txt` con los contenidos de R1.4 (con `printf`), y `notas.txt` benigno. Si un archivo ya existe, no lo sobrescribe y lo informa. Imprime el `sha256sum` de cada uno. No crea el archivo EICAR.

## 7. Decisiones

| Decisión | Alternativa descartada | Motivo |
|---|---|---|
| Formato propio y estricto (claves exactas) | Formato tolerante a varias formas | No hay formato oficial (el usuario lo confirmó). Uno estricto hace que "validar que tenga el formato indicado" sea comprobable y da errores precisos en la demo. |
| `fchmod` sobre el descriptor ya abierto con `openat2` + `RESOLVE_NO_SYMLINKS` | `chmod(ruta, 0)`; `open` con solo `O_NOFOLLOW` | Evita la carrera ruta→enlace: el daemon es root y un `chmod` por ruta podría dejar sin permisos un archivo del sistema. `O_NOFOLLOW` solo cubre el último componente: cambiar el directorio vigilado por un enlace lo saltaba (revisión B1). La ruta vigilada es canónica (`realpath`), así que una ruta legítima no contiene enlaces. |
| Cuarentena manual solo para entradas del listado | Cualquier ruta enviada por el admin | Limita a root al directorio vigilado; `/etc/...` → 404. |
| Sin `Archivo modificado` para infectados | Emitir LOW y HIGH | El enunciado reserva LOW para lo que no coincide con la blacklist. |
| Re-evaluación al subir la blacklist | Esperar al próximo evento | La demo muestra en vivo que una firma nueva pone en cuarentena un archivo que ya existía. |
| Persistir el cuerpo exacto con `rename` atómico conservando dueño y modo | No persistir, o reescribir con cJSON | Es una "base de datos local": sobrevive al reinicio, nunca queda a medias y no deja archivos de root en el repositorio. |
| Búfer de 1 MiB reservado solo tras validar una sesión admin | Subir `TAM_PETICION` para todos | 64 clientes × 1 MiB sin autenticar sería un DoS de memoria trivial. |
| Listas de amenazas y cuarentena aparte del listado | Derivarlas de las entradas del listado | El listado se vacía al cambiar la ruta y desaloja entradas; las vistas de R3.6 y R4.4 no deben perder nada. |
