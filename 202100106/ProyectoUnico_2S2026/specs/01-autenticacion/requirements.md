# F01 — Autenticación y roles (PAM) · Requisitos

## Introducción

Agregar al daemon el inicio de sesión contra PAM, los roles `admin_user` / `common_user` y el control de permisos por endpoint, aplicado **en C** (enunciado §2.2.3.9 y §2.2.4.7). Desde esta feature ningún dato del daemon se entrega sin una sesión válida.

Fuera de alcance (otras features):
- Alertas `LOW` "Inicio de sesión exitoso" y `MEDIUM` "Inicio de sesión fallido": las genera el motor de alertas (F02), que se engancha en el punto que deja esta feature (R3.8).
- Endpoints de administración nuevos (kill, monitoreo, ruta, blacklist, cuarentena, logwatch): cada uno llega con su feature y se registra en la tabla de permisos de R4.
- Bloqueo por intentos fallidos repetidos.

## Glosario

- **Sesión:** registro en memoria del daemon con usuario, rol y último uso, identificado por un token aleatorio.
- **Token:** 32 bytes de `getrandom()` en hexadecimal (64 caracteres).
- **Cookie de sesión:** `so2_sesion=<token>`.
- **Rol mínimo:** `publico` (sin sesión), `common_user` o `admin_user`.
- **Usuario de prueba:** cuenta temporal con prefijo `so2t_` que crean y borran los scripts de prueba.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Preparación del sistema

**Historia:** Como administrador, quiero un script que deje los grupos y el servicio PAM listos, para no configurarlo a mano en cada VM.

1. EL proyecto DEBERÁ incluir `scripts/setup_usuarios.sh`, que al ejecutarse con `sudo` y sin argumentos:
   - instale `libpam0g-dev` si falta `/usr/include/security/pam_appl.h`;
   - cree los grupos `admin_user` y `common_user` si no existen;
   - escriba `/etc/pam.d/so2-monitor` con `auth required pam_unix.so` y `account required pam_unix.so`.
2. EL script DEBERÁ ser idempotente: una segunda ejecución no falla ni duplica nada.
3. CUANDO se ejecute `sudo scripts/setup_usuarios.sh agregar <usuario> <admin|comun>`, EL script DEBERÁ crear el usuario si no existe (`useradd -m`), agregarlo al grupo correspondiente y pedir la contraseña con `passwd` de forma interactiva.
4. SI el script no corre como root, DEBERÁ terminar con código 1 y el mensaje `Ejecuta con sudo`.

## R2 — Verificación con PAM

**Historia:** Como usuario, quiero entrar con mi usuario y contraseña de Linux, para no tener credenciales aparte.

1. EL daemon DEBERÁ verificar las credenciales con `pam_start("so2-monitor", …)`, `pam_authenticate` y `pam_acct_mgmt`, y una función de conversación que entregue la contraseña recibida.
2. EL daemon NO DEBERÁ aceptar contraseñas vacías (`PAM_DISALLOW_NULL_AUTHTOK`).
3. EL daemon DEBERÁ borrar la contraseña de su memoria (`explicit_bzero`) apenas termine la verificación, y NUNCA DEBERÁ escribirla en logs, respuestas ni Engram.
4. Un login fallido NO DEBERÁ bloquear el servidor HTTP: `curl` con credenciales incorrectas DEBERÁ recibir respuesta en menos de **1 s**, y los clientes SSE conectados no se detienen.
5. CUANDO arranque, SI el daemon no corre como root, DEBERÁ registrar la advertencia `La autenticacion PAM requiere ejecutar el daemon como root` y seguir funcionando.
6. CUANDO arranque, SI no existe `/etc/pam.d/so2-monitor` o falta alguno de los dos grupos, DEBERÁ registrar una advertencia que indique ejecutar `scripts/setup_usuarios.sh`, sin terminar.

## R3 — Login, sesión y logout

**Historia:** Como usuario, quiero iniciar y cerrar sesión desde el dashboard, y que el sistema recuerde mi rol mientras la uso.

1. `POST /api/login` DEBERÁ recibir en el cuerpo `{"usuario":"<nombre>","password":"<clave>"}`. El encabezado `Content-Type` no se valida.
2. SI el cuerpo no es JSON válido, falta alguno de los dos campos, no son cadenas, `usuario` está vacío, mide más de 32 caracteres o tiene caracteres fuera del ASCII imprimible sin espacios (`0x21`–`0x7e`), o `password` está vacío o mide más de 128, EL daemon DEBERÁ responder `400` con `{"error":"Solicitud de login invalida"}` sin llamar a PAM.
3. SI PAM rechaza las credenciales (usuario inexistente o contraseña incorrecta), EL daemon DEBERÁ responder `401` con `{"error":"Credenciales invalidas"}`. La respuesta DEBERÁ ser idéntica en ambos casos.
4. SI PAM acepta las credenciales pero el usuario no pertenece a `admin_user` ni a `common_user`, EL daemon DEBERÁ responder `403` con `{"error":"El usuario no pertenece a admin_user ni common_user"}` y NO DEBERÁ crear sesión.
5. SI el usuario pertenece a `admin_user` (esté o no en `common_user`), su rol DEBERÁ ser `admin_user`; SI pertenece solo a `common_user`, su rol DEBERÁ ser `common_user`.
6. CUANDO el login sea exitoso, EL daemon DEBERÁ responder `200` con `{"usuario":"<nombre>","rol":"<rol>"}` y el encabezado `Set-Cookie: so2_sesion=<token>; Path=/; HttpOnly; SameSite=Strict`.
7. La pertenencia a grupos DEBERÁ consultarse en cada login, de modo que un usuario creado o agregado a un grupo desde la consola con el daemon en marcha pueda entrar **sin reiniciar el daemon**.
8. EL daemon DEBERÁ registrar cada intento con `registrar()`: `Login exitoso: <usuario> (<rol>)`, `Login fallido: <usuario>` o `Login sin grupo: <usuario>`. Este punto es donde F02 generará las alertas de sesión.
9. `GET /api/sesion` con sesión válida DEBERÁ responder `200` con `{"usuario":"<nombre>","rol":"<rol>"}`.
10. `POST /api/logout` DEBERÁ eliminar la sesión de la cookie (si existe), cerrar las conexiones SSE abiertas con esa sesión y responder `200` con `{"ok":true}` y `Set-Cookie: so2_sesion=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict`. Sin sesión, también responde `200`.
11. Una sesión DEBERÁ expirar tras **30 minutos sin peticiones**. Cada petición autenticada renueva el último uso. CUANDO una sesión expire, EL daemon DEBERÁ cerrar sus conexiones SSE.
12. EL daemon DEBERÁ admitir hasta **64** sesiones simultáneas; CUANDO se cree una sesión con la tabla llena, DEBERÁ descartar la de uso más antiguo.
13. El rol de una sesión se fija al iniciarla; un cambio de grupos se aplica en el siguiente login.

## R4 — Control de permisos en el daemon

**Historia:** Como evaluador, quiero que el daemon rechace cualquier acción que el rol no permita, aunque se llame a la API directamente.

1. Cada ruta de la API DEBERÁ declarar método y rol mínimo en **una sola tabla** en C. Para esta feature:

   | Método | Ruta | Rol mínimo |
   |---|---|---|
   | `POST` | `/api/login` | `publico` |
   | `POST` | `/api/logout` | `publico` |
   | `GET` | `/api/sesion` | `common_user` |
   | `GET` | `/api/estado` | `common_user` |
   | `GET` | `/api/eventos` | `common_user` |
   | `GET` | `/api/proceso` | `admin_user` |

2. SI una ruta requiere sesión y la petición no trae cookie, trae un token desconocido o la sesión expiró, EL daemon DEBERÁ responder `401` con `{"error":"Sesion requerida"}` sin ejecutar la acción.
3. SI la sesión es válida pero su rol no alcanza el rol mínimo, EL daemon DEBERÁ responder `403` con `{"error":"Permiso denegado"}` sin ejecutar la acción.
4. SI la ruta empieza con `/api/` y no está en la tabla, EL daemon DEBERÁ responder `404` con `{"error":"Recurso no encontrado"}`.
5. SI la ruta existe pero el método no coincide, EL daemon DEBERÁ responder `405` con `{"error":"Metodo no permitido"}`.
6. Los archivos estáticos del dashboard (`/`, `/login.html`, CSS, JS) DEBERÁN servirse sin sesión y solo con `GET`; no contienen datos del sistema.
7. `admin_user` DEBERÁ tener acceso a todo lo que tiene `common_user`.

## R5 — HTTP

**Historia:** Como desarrollador, quiero que el servidor HTTP de P4 soporte lo necesario para sesiones, sin romper lo existente.

1. EL servidor DEBERÁ aceptar `GET` y `POST`. CUANDO llegue un `POST`, DEBERÁ esperar el cuerpo completo según `Content-Length` antes de atenderlo.
2. SI un `POST` no trae `Content-Length` o su valor no es un entero ≥ 0, EL servidor DEBERÁ responder `411` con `{"error":"Content-Length requerido"}`.
3. SI encabezados y cuerpo no caben en el búfer de petición (4096 bytes), EL servidor DEBERÁ responder `413` con `{"error":"Peticion demasiado grande"}`.
4. Los nombres de encabezado (`Cookie`, `Content-Length`) DEBERÁN reconocerse sin distinguir mayúsculas.
5. EL servidor DEBERÁ dejar de enviar `Access-Control-Allow-Origin: *`; el dashboard se sirve del mismo origen.
6. Ningún encabezado, cookie o cuerpo malformado DEBERÁ terminar el daemon.

## R6 — Dashboard

**Historia:** Como usuario, quiero una pantalla de login y que el dashboard muestre solo lo que mi rol puede usar.

1. `Dashboard/login.html` DEBERÁ pedir usuario y contraseña, enviar `POST /api/login` y, si responde `200`, redirigir a `/`.
2. SI el login falla, la página DEBERÁ mostrar el campo `error` de la respuesta sin recargar y sin borrar el usuario escrito.
3. CUANDO cargue `/`, el dashboard DEBERÁ consultar `GET /api/sesion`; SI responde `401`, DEBERÁ redirigir a `/login.html`.
4. CUANDO cualquier `fetch` a la API responda `401`, el dashboard DEBERÁ redirigir a `/login.html`.
5. El encabezado del dashboard DEBERÁ mostrar el usuario, su rol y un botón **Cerrar sesión** que llame a `POST /api/logout` y redirija a `/login.html`.
6. Los controles exclusivos de administrador DEBERÁN ocultarse para `common_user`. En esta feature: el panel **Consultar proceso por PID** (`#panel-pid`).
7. El dashboard DEBERÁ usar rutas relativas del mismo origen; se elimina el parámetro `?api=` de P4.
8. Ocultar controles es solo presentación: la protección real es R4.

## R7 — Ajuste de `obtener_home` (hallazgo de la revisión F00)

1. `obtener_home()` DEBERÁ usar `SUDO_USER` solo cuando `geteuid() == 0`; en otro caso DEBERÁ usar `HOME`.

## R8 — Pruebas y paridad

**Historia:** Como evaluador, quiero pruebas que demuestren los permisos con usuarios reales, y que lo de F00 siga funcionando.

1. `make -C ProgramaIntermedio pruebas` DEBERÁ correr, además de `test_config`, las pruebas unitarias nuevas de sesiones, roles, tabla de permisos y análisis de encabezados, sin necesitar root ni PAM real, y terminar con código 0.
2. EL proyecto DEBERÁ incluir `scripts/pruebas_auth.sh`, que se ejecuta con `sudo` en la VM, crea usuarios de prueba temporales, verifica R2–R5 contra el daemon real y borra los usuarios al terminar, incluso si falla.
3. `scripts/pruebas_integracion.sh` DEBERÁ adaptarse para iniciar sesión y conservar todas sus verificaciones de F00; DEBERÁ seguir terminando con `0 FALLIDAS`.
4. `scripts/demo.sh` DEBERÁ pedir usuario y contraseña una vez y usar la sesión en sus consultas a la API.
5. Los scripts de prueba NUNCA DEBERÁN imprimir ni guardar las contraseñas de los usuarios de prueba.
6. `make -C ProgramaIntermedio clean all` DEBERÁ compilar sin warnings y el ejecutable `daemon_monitor` actualizado DEBERÁ versionarse.
7. `README.md` DEBERÁ explicar cómo preparar PAM y los grupos, crear usuarios, ejecutar el daemon con `sudo` e iniciar sesión.
