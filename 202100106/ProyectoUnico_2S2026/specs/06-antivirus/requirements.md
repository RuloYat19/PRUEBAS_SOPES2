# F06 — Antivirus, blacklist y cuarentena · Requisitos

## Introducción

Completar el motor antivirus del enunciado (§2.2.2.2, §2.2.3.3, §2.2.3.4, §2.2.4.3, §2.2.4.4 y la tabla de permisos) sobre el sensor de F05:
- la **blacklist** de firmas SHA-256 en memoria, cargada de `blacklist.json` al arrancar y reemplazada cuando el administrador sube un JSON válido;
- la detección `HIGH` (`Hash malicioso detectado`) seguida de la **cuarentena automática** (permisos `0000`) y su alerta `HIGH`;
- la **cuarentena manual** desde el dashboard y la vista de archivos en cuarentena (solo admin);
- la vista de **amenazas detectadas**: firma, archivo y descripción.

El enunciado no trae un formato oficial para `blacklist.json` (el enlace a la base de datos se perdió). Por decisión del usuario (2026-10-21), el proyecto define un formato propio, descriptivo y pensado para la demostración (R1). Ese formato es el "formato indicado" que se valida.

## Glosario

- **Firma:** entrada de la blacklist: SHA-256, nombre de la amenaza y descripción.
- **Infectado:** archivo cuyo SHA-256 actual coincide con una firma de la blacklist vigente.
- **Cuarentena:** `fchmod(fd, 0000)` sobre el descriptor ya abierto con `O_NOFOLLOW`. Nadie salvo root puede leer, escribir ni ejecutar el archivo. No se mueve ni se borra.
- **Re-evaluación:** volver a comparar el listado de F05 contra una blacklist nueva.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Formato de `blacklist.json`

1. El archivo DEBERÁ ser un objeto JSON con esta forma:

   ```json
   {
     "nombre": "Base local de firmas SO2",
     "version": 1,
     "actualizado": "2026-10-21",
     "firmas": [
       {
         "sha256": "8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660",
         "nombre": "SO2.Demo.Troyano",
         "descripcion": "Muestra de demostración que simula un troyano."
       }
     ]
   }
   ```

2. Reglas de validación (todas obligatorias; si una falla, se rechaza el archivo completo):
   1. la raíz es un objeto; sus únicas claves permitidas son `nombre`, `version`, `actualizado` y `firmas`;
   2. `firmas` es obligatoria y es un arreglo de **1 a 5000** objetos;
   3. `nombre` (opcional) es una cadena de 1 a 64 bytes; `version` (opcional) es un entero ≥ 1; `actualizado` (opcional) es una cadena de 1 a 32 bytes;
   4. cada firma tiene **exactamente** las claves `sha256`, `nombre` y `descripcion`;
   5. `sha256` son 64 caracteres hexadecimales (se aceptan mayúsculas y se guardan en minúsculas);
   6. el `nombre` de la firma es una cadena de 1 a 64 bytes y la `descripcion`, de 1 a 256 bytes (UTF-8);
   7. no se repite ningún `sha256` (comparado ya en minúsculas).
3. CUANDO una regla falle, el error DEBERÁ decir cuál, con estos textos exactos (`<n>` es el índice de la firma, empezando en 1):
   - `JSON mal formado`
   - `La raiz debe ser un objeto`
   - `Clave desconocida: <clave>`
   - `Falta el arreglo firmas`
   - `firmas debe tener entre 1 y 5000 elementos`
   - `Campo invalido: <nombre|version|actualizado>`
   - `Firma <n>: clave desconocida <clave>`
   - `Firma <n>: falta <sha256|nombre|descripcion>`
   - `Firma <n>: sha256 invalido`
   - `Firma <n>: nombre invalido`
   - `Firma <n>: descripcion invalida`
   - `Firma <n>: sha256 duplicado`
4. EL proyecto DEBERÁ incluir `ProgramaIntermedio/blacklist.json` con las 4 firmas de demostración de la tabla siguiente.

   | `nombre` | Contenido que la produce | `sha256` |
   |---|---|---|
   | `SO2.Demo.Troyano` | `SO2-MUESTRA-MALICIOSA-1` + salto de línea | `8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660` |
   | `SO2.Demo.Ransomware` | `SO2-MUESTRA-MALICIOSA-2` + salto de línea | `501e1e69c486c462ff11499f2086dcde083a3743ded7bdc178137b12ad39845f` |
   | `SO2.Demo.Keylogger` | `SO2-MUESTRA-MALICIOSA-3` + salto de línea | `431caf694d5a52919d212d6f5c90091a8ce01a9b3d89cb927b81d65030788b4f` |
   | `EICAR-Test-File` | archivo de prueba estándar EICAR (68 bytes) | `275a021bbfb6489e54d471899f7db9d1663fc695ec2fe2a2c4538aabf651fd0f` |

   Cada firma lleva una descripción en español. El repositorio guarda solo el hash de EICAR, nunca su contenido.

## R2 — Blacklist en memoria

1. AL arrancar, EL daemon DEBERÁ cargar la blacklist del archivo de la clave `blacklist` de `daemon.conf`, antes de activar el sensor. SI no existe o no es válida, DEBERÁ registrar el motivo, arrancar con la blacklist vacía y exponer el motivo en `GET /api/blacklist`.
2. La blacklist DEBERÁ estar en una tabla hash en C indexada por el SHA-256, protegida por su propio mutex. El reemplazo es atómico: la tabla nueva se arma fuera del mutex y solo el intercambio de punteros ocurre dentro.
3. `POST /api/blacklist` (rol `admin_user`) DEBERÁ recibir como cuerpo el contenido del JSON (`Content-Type: application/json`, hasta **1 MiB**) y:
   - con un JSON inválido según R1 → `400` con `{"error":"Blacklist invalida: <detalle de R1.3>"}`, sin cambiar la blacklist ni el archivo en disco;
   - con un cuerpo de más de 1 MiB → `413` con `{"error":"Peticion demasiado grande"}`;
   - con un JSON válido → guardarlo en la ruta configurada (archivo temporal en el mismo directorio y `rename`, conservando el dueño y los permisos del archivo anterior), reemplazar la blacklist en memoria y responder `200` con `{"firmas":<n>,"nombre":"<nombre o vacío>"}`;
   - SI no se puede guardar → `500` con `{"error":"No se pudo guardar la blacklist"}`, sin cambiar la blacklist en memoria.
4. CUANDO se reemplace la blacklist, EL daemon DEBERÁ emitir `Actualización base de datos local` (`MEDIUM`, tipo `archivos`) con `Blacklist actualizada por <usuario>: <n> firmas` y **re-evaluar** el listado (R3.5).
5. El búfer grande solo DEBERÁ reservarse para `POST /api/blacklist` con una sesión válida de `admin_user`. Sin cookie, la respuesta es `401`, y con otro rol, `403`, en ambos casos sin leer el cuerpo. Las demás rutas conservan el límite de F01 (`413` por encima del búfer normal).
6. `GET /api/blacklist` (rol `admin_user`) DEBERÁ responder con `nombre`, `version`, `actualizado`, `firmas` (cantidad), `origen` (`archivo` o `subida`), `actualizada_por`, `cargada` (fecha), `error` (vacío si no hay) y `lista` (las firmas, cada una con `sha256`, `nombre` y `descripcion`).

## R3 — Detección y cuarentena automática

1. CUANDO el análisis de F05 (evento o análisis inicial) calcule un SHA-256 que esté en la blacklist, EL daemon DEBERÁ, en este orden:
   1. marcar la entrada del listado como `infectado`, con el nombre de la firma;
   2. emitir `Hash malicioso detectado` (`HIGH`, tipo `archivos`) con `Firma <firma> detectada en <ruta> (PID <pid>, <proceso>)`; en el análisis inicial, `(análisis inicial)` en lugar del paréntesis del PID, y en la re-evaluación, `(actualización de blacklist)`;
   3. aplicar la cuarentena automática con `fchmod` sobre el **mismo descriptor** con el que calculó el hash;
   4. si funcionó, emitir `Archivo puesto en cuarentena` (`HIGH`) con `Archivo <ruta> puesto en cuarentena automática (firma <firma>)`.
2. Un archivo infectado NO DEBERÁ generar además `Archivo modificado` (`LOW`). Esa alerta queda solo para archivos que no coinciden con la blacklist (enunciado §2.2.2.2.6).
3. SI `fchmod` falla, EL daemon DEBERÁ registrar el error, dejar la entrada `infectado` con `cuarentena` false y no emitir la alerta de cuarentena.
4. SI el archivo ya tiene permisos `0000`, no se vuelve a aplicar la cuarentena ni se repite su alerta.
5. **Re-evaluación:** CUANDO se reemplace la blacklist y el escaneo esté activo, el hilo `archivos` DEBERÁ:
   - volver a abrir (con `O_NOFOLLOW`) y recalcular el hash de cada entrada del listado;
   - aplicar R3.1 a las que ahora coinciden;
   - marcar `benigno` las que ya no coinciden, **sin** quitarles la cuarentena (no hay restauración).

   La re-evaluación no emite `Archivo modificado`: solo actualiza el hash, el estado, la firma y la cuarentena.
6. Cada detección DEBERÁ registrarse en la lista de **amenazas**:
   - campos: `id`, `firma`, `descripcion`, `ruta`, `sha256`, `pid`, `proceso`, `fecha` y `cuarentena` (bool);
   - máximo 100, las más recientes primero;
   - la lista **no** se vacía al cambiar la ruta.

## R4 — Cuarentena manual y vista de cuarentena

1. `POST /api/cuarentena` (rol `admin_user`) DEBERÁ recibir `{"ruta":"<ruta>"}` y responder:
   - `400` con `{"error":"Ruta invalida"}` si el cuerpo es inválido o `ruta` no es una cadena;
   - `404` con `{"error":"El archivo no esta en el listado de analizados"}` si la ruta no coincide **exactamente** con una entrada del listado actual, si ya no existe, si es un enlace o si no es un archivo regular directamente dentro del directorio vigilado;
   - `409` con `{"error":"El archivo ya esta en cuarentena"}` si ya tiene permisos `0000`;
   - `500` con `{"error":"No se pudo poner en cuarentena"}` si `fchmod` falla;
   - `200` con `{"ruta":"<ruta>","cuarentena":true}` si se aplicó.
2. La cuarentena manual DEBERÁ hacerse con `openat2(O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC, RESOLVE_NO_SYMLINKS|RESOLVE_NO_MAGICLINKS)`, `fstat` y `fchmod(fd, 0000)`; ningún componente de la ruta puede ser un enlace (revisión B1). La misma apertura vale para el análisis por evento, el inicial y la re-evaluación (R3). Nunca con `chmod` sobre la ruta ni fuera del directorio vigilado: el daemon corre como root y es la única defensa contra que un admin del dashboard deje sin permisos archivos del sistema.
3. CUANDO se aplique, EL daemon DEBERÁ emitir `Archivo puesto en cuarentena` (`HIGH`) con `Archivo <ruta> puesto en cuarentena manual por <usuario>` y actualizar la entrada del listado (`cuarentena` true).
4. Cada cuarentena, automática o manual, DEBERÁ registrarse en una lista:
   - campos: `ruta`, `sha256`, `tipo` (`automatica`/`manual`), `motivo` (nombre de la firma o `manual`), `usuario` (vacío si es automática), `fecha` y `permisos_anteriores` (octal, por ejemplo `644`);
   - máximo 256, las más recientes primero;
   - la lista no se vacía al cambiar la ruta.
5. `GET /api/cuarentena` (rol `admin_user`) DEBERÁ responder `{"cuarentena":[…]}` con esa lista.

## R5 — API y tiempo real

1. Cada entrada de `/api/archivos`, `event: archivo` y `event: archivos` DEBERÁ incluir además `"firma"` (vacío si es benigno) y `"cuarentena"` (true si sus permisos son `0000` según el último `fstat`).
2. `GET /api/amenazas` (rol `common_user`) DEBERÁ responder `{"amenazas":[…]}` con la lista de R3.6.
3. CUANDO se registre una amenaza, EL daemon DEBERÁ enviar `event: amenaza` con esa entrada.
4. Los contratos anteriores no cambian salvo por las claves nuevas.

## R6 — Dashboard

1. Un panel **Amenazas detectadas**, visible para todos, DEBERÁ mostrar Firma, Archivo, Descripción, Fecha y Cuarentena (sí/no). Se carga con `GET /api/amenazas` al abrir o reabrir el SSE y se actualiza con `event: amenaza`.
2. En **Archivos analizados**, el estado `infectado` DEBERÁ verse en rojo junto al nombre de la firma, y los archivos en cuarentena llevarán una marca visible.
3. EL administrador DEBERÁ ver en cada fila un botón **Cuarentena**. Está deshabilitado si el archivo ya está en cuarentena y pide confirmación con el texto exacto `¿Poner en cuarentena <nombre>? El archivo quedará con permisos 0000.`
4. EL administrador DEBERÁ ver un panel **Cuarentena** con Archivo, Tipo, Motivo, Usuario, Fecha y Permisos anteriores. Se recarga con `GET /api/cuarentena` al abrir el SSE, después de una cuarentena manual y al llegar un `event: alerta` con motivo `Archivo puesto en cuarentena`.
5. EL administrador DEBERÁ ver un bloque **Base de firmas** con el nombre, la versión, la fecha y la cantidad de firmas (y `error` si lo hay), un selector de archivo `.json` y el botón **Actualizar blacklist**. El resultado o el error se muestran en un aviso.
6. EL usuario común NO DEBERÁ ver el botón **Cuarentena**, el panel **Cuarentena** ni **Base de firmas**.
7. Solo `textContent`; nada de `innerHTML`.

## R7 — Pruebas, demostración y paridad

1. `make -C ProgramaIntermedio pruebas` DEBERÁ incluir `pruebas/test_blacklist.c` (validación y tabla) y nuevos casos en `test_analisis.c` (detección, cuarentena, re-evaluación, cuarentena manual y listas). Todo sin root y con código 0.
2. EL proyecto DEBERÁ incluir `scripts/pruebas_antivirus.sh` (`sudo`, VM), que verifique R1–R5 contra el daemon real, sumado a `correr_pruebas.sh`. Usa una copia de la blacklist en `~/so2_logs/pruebas_antivirus/` y **nunca** escribe `ProgramaIntermedio/blacklist.json`.
3. EL proyecto DEBERÁ incluir `scripts/demo_antivirus.sh` (sin root), que crea en `~/so2_vigilado` las tres muestras de R1.4 y un archivo benigno, para la demostración.
4. Las suites anteriores DEBERÁN seguir con `0 FALLIDAS`.
5. `make -C ProgramaIntermedio clean all` DEBERÁ terminar sin warnings, con `daemon_monitor` versionado.
