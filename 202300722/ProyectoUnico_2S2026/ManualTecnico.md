# Manual Técnico — Sistema Integral de Monitoreo y Seguridad

## Índice

1. [Propósito](#1-propósito)
2. [Arquitectura del sistema](#2-arquitectura-del-sistema)
3. [Estructura del proyecto](#3-estructura-del-proyecto)
4. [Dependencias e instalación](#4-dependencias-e-instalación)
5. [Integración con el kernel Linux](#5-integración-con-el-kernel-linux)
6. [Compilación del proyecto](#6-compilación-del-proyecto)
7. [Ejecución y administración del daemon](#7-ejecución-y-administración-del-daemon)
8. [API HTTP y dashboard](#8-api-http-y-dashboard)
9. [Autenticación PAM y grupos de usuarios](#9-autenticación-pam-y-grupos-de-usuarios)
10. [Detección mediante SHA-256 y blacklist](#10-detección-mediante-sha-256-y-blacklist)
11. [Pruebas de funcionamiento](#11-pruebas-de-funcionamiento)
12. [Solución de problemas](#12-solución-de-problemas)
13. [Limitaciones y aspectos pendientes](#13-limitaciones-y-aspectos-pendientes)
14. [Conclusiones](#14-conclusiones)

---

## 1. Propósito

Implementar un sistema integral de monitoreo y seguridad sobre Linux que combine llamadas al sistema (*system calls* o syscalls), un daemon desarrollado en C, un sensor eBPF y un dashboard web para visualizar información del sistema y detectar eventos relevantes.

La versión del kernel autorizada por el tutor para este proyecto es **Linux 6.18.48**.

Los objetivos principales son:

- Obtener información sobre procesos y estados del sistema.
- Monitorear eventos de archivos mediante eBPF.
- Detectar archivos que coincidan con firmas SHA-256 registradas en una blacklist.
- Generar alertas de seguridad.
- Aplicar una cuarentena básica mediante cambios de permisos.
- Registrar eventos y estadísticas en un archivo de log.
- Exponer información mediante una API HTTP.
- Permitir la consulta del estado del sistema desde un dashboard web.
- Finalizar el daemon de manera controlada, liberando los recursos utilizados por sus hilos.

## 2. Arquitectura del sistema

El sistema está compuesto por los siguientes elementos.

### 2.1. Kernel Linux

El kernel modificado contiene las siguientes syscalls personalizadas:

| Número | Nombre | Función general |
|---:|---|---|
| 470 | `get_process_info` | Obtener información de procesos. |
| 471 | `get_processes_state` | Obtener información de los estados de los procesos. |
| 472 | `ipc_channel_send` | Enviar mensajes mediante el canal IPC. |
| 473 | `ipc_channel_receive` | Recibir mensajes mediante el canal IPC. |
| 474 | `start_log_watch` | Iniciar el monitoreo de logs. |
| 475 | `stop_log_watch` | Detener un monitoreo de logs. |

Los números indicados corresponden a la integración utilizada en el proyecto y deben coincidir con las definiciones del kernel compilado y con las constantes utilizadas por el daemon.

### 2.2. Sensor eBPF

El sensor utiliza un tracepoint asociado a `sys_enter_openat` para observar operaciones de apertura de archivos que cumplan los filtros configurados.

Sus responsabilidades son:

- Identificar operaciones de apertura con indicadores de escritura o creación.
- Filtrar las rutas que pertenecen al directorio monitoreado.
- Excluir eventos del propio daemon cuando corresponda a la implementación.
- Publicar metadatos del evento mediante un Ring Buffer.
- Comunicar al daemon la ruta y el proceso que generó el evento.

El evento observado contiene, como mínimo, el PID, el tipo de acción y la ruta del archivo.

**Importante:** observar una apertura no garantiza que la escritura haya terminado. Por tanto, el evento eBPF debe interpretarse de acuerdo con el momento en que se genera.

### 2.3. Daemon en C

El daemon funciona como intermediario entre el kernel, el sensor eBPF y el dashboard.

Entre sus responsabilidades se encuentran:

- Recopilar información de procesos y métricas del sistema.
- Leer estadísticas de archivos del sistema, como `/proc/meminfo` y `/proc/vmstat`, según la implementación.
- Consumir eventos de archivos enviados por el sensor eBPF.
- Utilizar las syscalls IPC para intercambiar mensajes.
- Calcular hashes SHA-256 mediante OpenSSL EVP.
- Consultar las firmas registradas en `blacklist.json`.
- Generar alertas y registrar eventos en el log.
- Ejecutar un servidor HTTP local.
- Gestionar los hilos y los recursos utilizados durante la ejecución.

El daemon utiliza varios hilos para separar las tareas de monitoreo, consumo de eventos, IPC y atención HTTP.

### 2.4. Dashboard web

El dashboard está desarrollado con HTML, CSS y JavaScript.

Sus archivos principales son:

- `index.html`: estructura de la interfaz.
- `styles.css`: estilos visuales.
- `app.js`: lógica del cliente y comunicación con la API.

La interfaz consulta el estado del daemon y presenta la información que este expone. Las funciones administrativas disponibles dependen de las acciones implementadas en la versión actual.

### 2.5. Flujo general de funcionamiento

1. El kernel proporciona las syscalls personalizadas.
2. El sensor eBPF observa las operaciones que cumplen sus filtros.
3. Los eventos llegan al daemon mediante el Ring Buffer.
4. El daemon analiza la ruta del archivo y calcula su hash SHA-256.
5. El hash se compara con las firmas de la blacklist.
6. Si existe una coincidencia, el daemon genera una alerta y aplica la cuarentena configurada.
7. Los eventos y las alertas se registran en el log.
8. El dashboard consulta la API para mostrar el estado del sistema.

## 3. Estructura del proyecto

La estructura general del proyecto es la siguiente:

```text
ProyectoUnico_2S2026/
├── Kernel/
│   └── Archivos de integración del kernel
├── ProgramaIntermedio/
│   ├── daemon.c
│   ├── Makefile
│   ├── config.json
│   ├── blacklist.json
│   └── ebpf/
│       ├── Código fuente eBPF
│       ├── vmlinux.h
│       └── Skeleton generado
└── Dashboard/
    ├── index.html
    ├── styles.css
    └── app.js
```

La ubicación real de los archivos puede variar según la organización utilizada para la entrega.

En el entorno de desarrollo utilizado para las pruebas, el directorio del programa intermedio es:

```bash
cd ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/202300722/ProyectoUnico_2S2026/ProgramaIntermedio
```

El código fuente del kernel utilizado se encuentra en:

```bash
~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/202300722/ProyectoUnico_2S2026/linux-6.18.48
```

Los archivos de prueba proporcionados para el proyecto se encuentran en:

```bash
~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/BD_ProyectoUnico/test_files/
```

Entre ellos se encuentra `Simulated.Adware.Popup.txt`, utilizado para comprobar la detección de una firma maliciosa simulada.

## 4. Dependencias e instalación

Las siguientes dependencias se utilizan para compilar el programa y trabajar con eBPF, OpenSSL, PAM y JSON-C.

En Ubuntu, ejecutar:

```bash
sudo apt update

sudo apt install -y \
  build-essential \
  gcc \
  make \
  clang \
  llvm \
  bpftool \
  libbpf-dev \
  libelf-dev \
  libssl-dev \
  libpam0g-dev \
  libjson-c-dev \
  pkg-config
```

La disponibilidad de algunas dependencias puede variar según la versión de Ubuntu y la configuración de los repositorios.

### 4.1. Verificación del kernel

El kernel debe disponer de las opciones necesarias para trabajar con BPF, los tracepoints y la información de depuración requerida por la compilación del sensor.

Entre las opciones que deben verificarse se encuentran:

```text
CONFIG_BPF
CONFIG_BPF_SYSCALL
CONFIG_BPF_EVENTS
CONFIG_DEBUG_INFO_BTF
CONFIG_FTRACE_SYSCALLS
```

Si el archivo de configuración está disponible en `/boot`, puede consultarse mediante:

```bash
grep -E \
'CONFIG_BPF=|CONFIG_BPF_SYSCALL=|CONFIG_BPF_EVENTS=|CONFIG_DEBUG_INFO_BTF=|CONFIG_FTRACE_SYSCALLS=' \
/boot/config-$(uname -r)
```

También puede consultarse la versión del kernel que está ejecutándose:

```bash
uname -r
```

El resultado debe corresponder a la versión del kernel modificado que se utiliza para las pruebas.

## 5. Integración con el kernel Linux

La integración de las syscalls requiere que sus declaraciones, implementaciones y números coincidan con los utilizados por el daemon.

### 5.1. Archivos relevantes

Los archivos que deben revisarse durante la integración son:

- `include/linux/syscalls.h`: declaraciones de las syscalls.
- `arch/x86/entry/syscalls/syscall_64.tbl`: números y entradas de las syscalls para x86-64.
- `kernel/Makefile`: inclusión de los archivos fuente correspondientes.
- Archivos `.c` que implementan las syscalls.
- Código del daemon que invoca las llamadas mediante `syscall()`.

### 5.2. Verificación de las entradas

Desde el directorio del kernel:

```bash
cd ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/202300722/ProyectoUnico_2S2026/linux-6.18.48
```

Consultar las entradas correspondientes:

```bash
grep -E 'get_process_info|get_processes_state|ipc_channel_send|ipc_channel_receive|start_log_watch|stop_log_watch' \
  arch/x86/entry/syscalls/syscall_64.tbl
```

Este comando permite comprobar los nombres y números registrados en la tabla.

**Precaución:** antes de modificar el kernel, realizar una copia de seguridad de los archivos afectados. No reemplazar archivos de una versión que ya funciona sin revisar las diferencias y las dependencias.

## 6. Compilación del proyecto

Para compilar el daemon, ubicarse en el directorio `ProgramaIntermedio`:

```bash
cd ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/202300722/ProyectoUnico_2S2026/ProgramaIntermedio
```

Limpiar los archivos generados anteriormente:

```bash
make clean
```

Compilar el proyecto:

```bash
make
```

Según las reglas definidas en el `Makefile`, el proceso genera o utiliza los siguientes elementos:

1. `vmlinux.h`, cuando se genera a partir de la información BTF del kernel.
2. El objeto compilado del programa eBPF.
3. El skeleton de libbpf.
4. El ejecutable `daemon`.

Si la compilación finaliza correctamente, comprobar que el ejecutable exista:

```bash
ls -l daemon
```

### 6.1. Advertencias de compilación

Durante la compilación se pueden mostrar advertencias relacionadas con:

- Indentación engañosa.
- Variables no utilizadas.
- Valores de retorno ignorados.
- Posible truncamiento de cadenas.

Una advertencia no equivale necesariamente a un error de compilación. Sin embargo, conviene corregirlas, especialmente cuando involucran operaciones de archivos, cadenas, memoria o llamadas al sistema.

Si `make` termina con un error, se debe corregir primero el problema indicado antes de ejecutar el daemon.

## 7. Ejecución y administración del daemon

El daemon puede ejecutarse en primer plano para depuración o en modo desacoplado de la terminal.

### 7.1. Ejecución en primer plano

Desde `ProgramaIntermedio`:

```bash
sudo ./daemon --foreground
```

Este comando debe utilizarse si la opción `--foreground` está implementada en la versión compilada.

La ejecución en primer plano permite observar directamente la salida del programa y facilita el diagnóstico de errores.

### 7.2. Ejecución normal

Para iniciar el daemon mediante el modo normal:

```bash
sudo ./start.sh
```

Si el proyecto no utiliza el script de inicio, se puede ejecutar directamente el binario:

```bash
sudo ./daemon
```

El modo normal del daemon implementado se desacopla de la terminal y registra los eventos en:

```text
/tmp/proyecto_unico.log
```

El PID del proceso se guarda en:

```text
/tmp/proyecto_unico.pid
```

Consultar el PID:

```bash
sudo cat /tmp/proyecto_unico.pid
```

Consultar las últimas líneas del log:

```bash
sudo tail -n 50 /tmp/proyecto_unico.log
```

Seguir el log en tiempo real:

```bash
sudo tail -f /tmp/proyecto_unico.log
```

Para salir de la visualización en tiempo real, presionar `Ctrl+C`. Esto no detiene el daemon.

### 7.3. Verificar si el daemon está activo

Ejecutar:

```bash
pgrep -af '(^|/)daemon$'
```

También se puede consultar el PID almacenado:

```bash
sudo cat /tmp/proyecto_unico.pid
```

La existencia del archivo PID, por sí sola, no garantiza que el proceso siga activo. Se debe contrastar con la lista de procesos y el log.

### 7.4. Detención controlada

La detención se realiza enviando `SIGTERM` al proceso:

```bash
sudo kill -TERM "$(sudo cat /tmp/proyecto_unico.pid)"
```

Esperar unos segundos:

```bash
sleep 2
```

Comprobar que el proceso haya finalizado:

```bash
pgrep -af '(^|/)daemon$'
```

Consultar el registro:

```bash
sudo tail -n 50 /tmp/proyecto_unico.log
```

Si la detención se realizó correctamente, debe aparecer un mensaje similar a:

```text
Daemon detenido limpiamente
```

En la implementación probada se corrigió un bloqueo durante el cierre relacionado con el hilo que esperaba mensajes mediante la syscall `ipc_channel_receive`. Después del ajuste y de recompilar el programa, la prueba de `SIGTERM` terminó correctamente y el daemon pudo finalizar sin quedar bloqueado en la espera del hilo IPC.

Este comportamiento debe conservarse al realizar cambios posteriores en `daemon.c`.

## 8. API HTTP y dashboard

El servidor HTTP del daemon escucha en la dirección local:

```text
127.0.0.1:8080
```

### 8.1. Consultar el estado

Ejecutar:

```bash
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool
```

El endpoint `GET /api/status` proporciona los datos que la implementación expone, como métricas, procesos, alertas y archivos observados.

Si el comando devuelve un error de conexión, verificar primero que el daemon esté activo y que el log indique que el servidor HTTP está escuchando.

### 8.2. Dashboard

Abrir en el navegador:

```text
http://127.0.0.1:8080/
```

La interfaz debe consultar la API y presentar los datos disponibles.

### 8.3. Autenticación

La ruta prevista para el inicio de sesión es:

```text
POST /api/login
```

El cuerpo JSON esperado por el diseño es:

```json
{
  "username": "usuario",
  "password": "contraseña"
}
```

La autenticación está planteada para utilizar PAM. Las credenciales reales deben proporcionarse de manera segura y no almacenarse en el código ni en archivos de configuración.

### 8.4. Controles administrativos

La ruta prevista para los controles es:

```text
POST /api/control
```

El prototipo utiliza una cabecera `X-Role` y un cuerpo JSON, por ejemplo:

```json
{
  "action": "metrics_off"
}
```

La acción es ilustrativa y solo funcionará si está implementada en el daemon.

**Advertencia de seguridad:** una cabecera `X-Role` enviada directamente por el cliente no es suficiente para garantizar autorización. En una versión de producción, el servidor debe validar una sesión autenticada o un token firmado y comprobar los permisos en cada solicitud.

## 9. Autenticación PAM y grupos de usuarios

El diseño contempla dos grupos:

- `admin_user`: usuarios con permisos administrativos previstos por la aplicación.
- `common_user`: usuarios estándar.

Crear los grupos si todavía no existen:

```bash
sudo groupadd -f admin_user
sudo groupadd -f common_user
```

Agregar el usuario actual al grupo administrativo:

```bash
sudo usermod -aG admin_user "$USER"
```

Para agregar otra cuenta al grupo estándar, sustituir `OTRO_USUARIO` por el nombre real:

```bash
sudo usermod -aG common_user OTRO_USUARIO
```

Los cambios de grupo requieren cerrar y volver a iniciar sesión para que la nueva sesión los refleje.

La autenticación PAM utiliza el servicio `login` según el diseño descrito. La configuración efectiva debe comprobarse en el sistema de pruebas, ya que la política PAM puede variar entre distribuciones.

No debe suponerse que pertenecer a un grupo concede automáticamente permisos dentro del dashboard: el daemon debe comprobar los grupos y aplicar las reglas de autorización correspondientes.

## 10. Detección mediante SHA-256 y blacklist

El sistema utiliza hashes SHA-256 para comparar archivos observados con firmas registradas en `blacklist.json`.

### 10.1. Formato de la blacklist

La estructura contempla un arreglo denominado `signatures`, cuyos elementos incluyen los campos `sha256`, `name` y `description`.

Ejemplo ilustrativo:

```json
{
  "signatures": [
    {
      "sha256": "HASH_SHA256_DE_PRUEBA",
      "name": "Simulated.Adware.Popup",
      "description": "Firma de prueba para validar la detección"
    }
  ]
}
```

El contenido real debe conservar el hash de la firma de prueba configurada para el proyecto.

### 10.2. Firma maliciosa simulada

El archivo utilizado en las pruebas es:

```text
Simulated.Adware.Popup.txt
```

Su hash SHA-256 esperado es:

```text
470eed5b6f546e420020e5b6e0bc2813022334eef3c0ed4794fe030eb7822adb
```

Este hash debe coincidir con la firma registrada en `blacklist.json`.

**Importante:** el archivo es una simulación académica para probar el mecanismo de detección. No debe interpretarse como malware real ni como una evaluación completa de un antivirus.

### 10.3. Calcular el hash de un archivo

Para calcular el hash de cualquier archivo:

```bash
sha256sum archivo_de_prueba.txt
```

Para calcular el hash del archivo malicioso simulado original, utilizar su ruta de origen:

```bash
sha256sum ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/BD_ProyectoUnico/test_files/Simulated.Adware.Popup.txt
```

La salida debe compararse con el hash almacenado en la blacklist.

### 10.4. Cuarentena

En la implementación probada, cuando el hash coincide con una firma maliciosa, el daemon intenta cambiar los permisos del archivo a `0000` mediante `chmod()`.

Esto impide el acceso ordinario al archivo para los usuarios sin privilegios suficientes, pero **no elimina el archivo, no lo mueve a un repositorio aislado y no garantiza una cuarentena completa**.

Para esta práctica se considera una simulación básica de cuarentena basada en permisos. El archivo debe ser de prueba y debe existir un procedimiento seguro para restaurar sus permisos cuando sea necesario.

## 11. Pruebas de funcionamiento

Las siguientes pruebas permiten verificar la compilación, la ejecución del daemon, la API, la detección eBPF y el comportamiento de la cuarentena.

### 11.1. Preparar el entorno

Abrir una terminal y ubicarse en el directorio del programa:

```bash
cd ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/202300722/ProyectoUnico_2S2026/ProgramaIntermedio
```

Compilar nuevamente:

```bash
make clean
make
```

Iniciar el daemon:

```bash
sudo ./start.sh
```

Si no se utiliza `start.sh`, iniciar el binario directamente:

```bash
sudo ./daemon
```

Comprobar el log:

```bash
sudo tail -n 30 /tmp/proyecto_unico.log
```

Se esperan mensajes que indiquen el inicio del daemon, la activación del monitoreo y el inicio del servidor HTTP. Los mensajes exactos dependen de la implementación.

### 11.2. Verificar la API HTTP

Ejecutar:

```bash
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool
```

Verificar que se reciba una respuesta JSON válida.

Para comprobar el acceso al dashboard, abrir:

```text
http://127.0.0.1:8080/
```

### 11.3. Preparar el directorio monitoreado

El directorio utilizado durante las pruebas es:

```text
/tmp/proyecto_monitoreado
```

Crear el directorio si no existe:

```bash
sudo mkdir -p /tmp/proyecto_monitoreado
```

Crear un archivo benigno:

```bash
echo "Archivo de prueba benigno del proyecto" \
  | sudo tee /tmp/proyecto_monitoreado/prueba_benigna.txt
```

Comprobar que exista:

```bash
sudo ls -l /tmp/proyecto_monitoreado/
```

Consultar el log:

```bash
sudo tail -n 30 /tmp/proyecto_unico.log
```

En las pruebas realizadas se observó un evento eBPF para `prueba_benigna.txt`, registrado con una línea similar a:

```text
eBPF evento pid=7995 action=2 path=/tmp/proyecto_monitoreado/prueba_benigna.txt
```

El PID y el valor de `action` pueden ser diferentes en cada ejecución.

### 11.4. Comprobar que el archivo benigno se conserva

Consultar sus permisos:

```bash
sudo stat -c '%a %n' \
  /tmp/proyecto_monitoreado/prueba_benigna.txt
```

Si los permisos iniciales son los habituales para un archivo creado con `tee` bajo una configuración estándar, se espera normalmente `644`; este valor puede variar según la configuración de permisos del sistema.

La comprobación también puede hacerse mediante:

```bash
sudo cat /tmp/proyecto_monitoreado/prueba_benigna.txt
```

El contenido debe seguir siendo legible y no debe aparecer una alerta de coincidencia maliciosa para ese archivo.

### 11.5. Copiar el archivo malicioso simulado al directorio monitoreado

Copiar el archivo original desde la carpeta de pruebas:

```bash
sudo cp \
  ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/BD_ProyectoUnico/test_files/Simulated.Adware.Popup.txt \
  /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

**Nota:** el sensor utiliza el evento de apertura para detectar operaciones de escritura o creación. Por ello, la prueba debe comprobar el resultado final y no depender exclusivamente de que el evento se genere después de que `cp` haya terminado.

### 11.6. Verificar el evento eBPF

Consultar las últimas líneas del log:

```bash
sudo tail -n 50 /tmp/proyecto_unico.log
```

Durante las pruebas realizadas se observó un evento con una ruta similar a:

```text
eBPF evento pid=8015 action=1 path=/tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

El PID y el valor de `action` son ejemplos del registro obtenido y pueden cambiar en otras ejecuciones.

### 11.7. Verificar la alerta de detección

Buscar los mensajes relevantes:

```bash
sudo grep -E \
'eBPF evento|Hash malicioso|archivo puesto en cuarentena' \
/tmp/proyecto_unico.log
```

En la prueba realizada, el log mostró mensajes equivalentes a:

```text
ALERTA LOW archivo: Hash malicioso 'Simulated.Adware.Popup' detectado en /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt; archivo puesto en cuarentena
Hash malicioso Simulated.Adware.Popup: /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

La severidad `LOW` corresponde a la firma de prueba configurada. No representa una clasificación universal de amenazas.

### 11.8. Comprobar el hash del archivo monitoreado

Ejecutar:

```bash
sudo sha256sum \
  /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

El hash esperado es:

```text
470eed5b6f546e420020e5b6e0bc2813022334eef3c0ed4794fe030eb7822adb
```

Si el hash no coincide, se debe comprobar que el archivo se haya copiado correctamente y que su contenido sea idéntico al archivo de origen.

Si el archivo ya fue puesto en cuarentena con permisos `0000`, `sudo sha256sum` debería poder leerlo cuando se ejecuta con privilegios suficientes, siempre que el archivo siga existiendo y no haya otros controles que impidan el acceso.

### 11.9. Comprobar los permisos de cuarentena

Consultar los permisos de ambos archivos:

```bash
sudo stat -c '%a %n' \
  /tmp/proyecto_monitoreado/prueba_benigna.txt \
  /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

El archivo malicioso debería mostrar permisos `0` si la llamada `chmod(path, 0000)` se ejecutó correctamente.

El archivo benigno debería conservar sus permisos originales.

También se puede comprobar automáticamente:

```bash
if [ "$(sudo stat -c '%a' /tmp/proyecto_monitoreado/prueba_benigna.txt)" = "0" ]; then
    echo "REVISAR: el archivo benigno tiene permisos 0000"
else
    echo "OK: el archivo benigno no tiene permisos 0000"
fi

if [ "$(sudo stat -c '%a' /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt)" = "0" ]; then
    echo "OK: archivo malicioso en cuarentena por permisos"
else
    echo "REVISAR: no se confirmó la cuarentena por permisos"
fi
```

La primera comprobación solo descarta que el archivo benigno tenga permisos `0000`; no garantiza que conserve exactamente sus permisos iniciales. Para una validación estricta, comparar el resultado con el valor original registrado antes de la prueba.

**Criterio de aprobación:** la detección se considera confirmada cuando coinciden el hash, la firma de la blacklist y la alerta del daemon. La cuarentena por permisos solo se considera verificada cuando `stat` muestra `0` para el archivo malicioso.

### 11.10. Verificar el cierre del daemon

Detener el daemon:

```bash
sudo kill -TERM "$(sudo cat /tmp/proyecto_unico.pid)"
```

Esperar a que finalice:

```bash
sleep 2
```

Consultar el log:

```bash
sudo tail -n 50 /tmp/proyecto_unico.log
```

Comprobar que ya no se encuentre activo:

```bash
pgrep -af '(^|/)daemon$'
```

Se espera que el daemon finalice limpiamente, que desaparezca el proceso y que se elimine el archivo PID conforme a la implementación.

### 11.11. Tabla de resultados esperados

| Prueba | Resultado esperado |
|---|---|
| Compilación | `make` finaliza correctamente y genera `daemon`. |
| Inicio del daemon | El proceso se inicia y registra mensajes en el log. |
| API HTTP | `/api/status` devuelve JSON válido. |
| Archivo benigno | Se observa el evento si cumple los filtros del sensor y no coincide con la blacklist. |
| Archivo malicioso simulado | Se observa el evento y se calcula el SHA-256. |
| Comparación de firmas | El hash coincide con la firma registrada. |
| Alerta | El log registra la detección del archivo simulado. |
| Cuarentena | `stat` confirma permisos `0000` para el archivo malicioso. |
| Cierre | `SIGTERM` detiene el daemon sin dejar bloqueado el hilo IPC. |

## 12. Solución de problemas

### 12.1. El daemon no inicia

Comprobar que el ejecutable exista:

```bash
ls -l daemon
```

Recompilar si es necesario:

```bash
make clean
make
```

Revisar el log:

```bash
sudo tail -n 100 /tmp/proyecto_unico.log
```

Verificar que no exista otra instancia utilizando los mismos recursos.

### 12.2. No aparecen eventos eBPF

Comprobar que el daemon esté activo y que el log muestre el inicio del sensor.

Verificar la ruta monitoreada:

```bash
sudo ls -ld /tmp/proyecto_monitoreado
```

Confirmar que el archivo se cree directamente dentro del directorio configurado. La implementación del sensor utilizada para estas pruebas filtra rutas y operaciones específicas; no debe suponerse que observa todos los archivos del sistema.

### 12.3. El archivo no se detecta como malicioso

Calcular el hash:

```bash
sudo sha256sum /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

Revisar la firma en `blacklist.json`:

```bash
cat blacklist.json
```

Comprobar que:

- El hash coincida exactamente.
- El archivo de blacklist sea el utilizado por el daemon.
- La ruta corresponda al directorio monitoreado.
- El sensor esté activo.
- La alerta se busque en el log correcto.

### 12.4. El log indica cuarentena, pero los permisos no son `0000`

Consultar los permisos y el estado del archivo:

```bash
sudo stat /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

Buscar el mensaje de detección:

```bash
sudo grep -n 'Simulated.Adware.Popup' /tmp/proyecto_unico.log
```

La línea del log indica que el programa registró la acción, pero la verificación definitiva de los permisos debe realizarse con `stat`. Si el resultado no coincide, revisar el retorno de `chmod()` y el momento en que se analiza el archivo.

### 12.5. El daemon no se detiene

Enviar `SIGTERM` y comprobar el PID:

```bash
sudo kill -TERM "$(sudo cat /tmp/proyecto_unico.pid)"
sleep 2
pgrep -af '(^|/)daemon$'
```

Si permanece activo, consultar los últimos mensajes:

```bash
sudo tail -n 100 /tmp/proyecto_unico.log
```

La versión probada solucionó el bloqueo del hilo IPC durante el cierre. Si vuelve a ocurrir, revisar los cambios en el manejo de señales, la espera de `ipc_channel_receive` y la secuencia de `pthread_join()`.

## 13. Limitaciones y aspectos pendientes

Las siguientes limitaciones deben tenerse presentes al interpretar los resultados y preparar la entrega.

1. **Eventos eBPF:** el sensor basado en `sys_enter_openat` observa aperturas que cumplen los filtros, pero no confirma por sí solo que la escritura haya finalizado. Para cubrir completamente el ciclo de escritura se necesita correlacionar eventos adicionales o utilizar un mecanismo que detecte la modificación completada.

2. **Alcance del monitoreo:** el filtro de rutas utilizado en las pruebas se limita al directorio configurado y a las condiciones implementadas en el programa eBPF. No debe interpretarse como monitoreo global de todos los archivos del sistema.

3. **Cuarentena:** cambiar los permisos a `0000` es una medida básica. No elimina el archivo ni constituye un aislamiento equivalente al de una solución antivirus de producción.

4. **Seguridad de la API:** el uso de una cabecera `X-Role` como mecanismo de autorización es únicamente un prototipo. Debe sustituirse por autenticación y autorización robustas asociadas a una sesión o token validado por el servidor.

5. **Procesos y estadísticas:** la disponibilidad de información detallada y de un listado completo de procesos depende de la integración efectiva de las syscalls y de la implementación del endpoint. No se debe considerar validado el ranking de procesos por RSS sin comprobar los datos reales.

6. **Acciones administrativas:** la terminación de procesos, la carga de blacklist desde el dashboard, la cuarentena manual, la administración de múltiples instancias de `log_watch` y la tabla completa de procesos deben probarse por separado si forman parte de los requisitos de evaluación.

7. **Pruebas de seguridad:** la firma `Simulated.Adware.Popup` sirve para comprobar el flujo de detección, pero no permite afirmar que el sistema detecta todas las amenazas ni que reemplaza un antivirus.

## 14. Conclusiones

1. La integración de las syscalls personalizadas del kernel Linux 6.18.48 con el daemon en C proporciona una base para consultar información del sistema, intercambiar mensajes mediante IPC y monitorear eventos relevantes.

2. La integración del sensor eBPF permitió observar eventos asociados a archivos creados dentro del directorio monitoreado. En las pruebas se registraron tanto un archivo benigno como el archivo de prueba `Simulated.Adware.Popup.txt`.

3. La comparación mediante SHA-256 permite identificar coincidencias exactas con las firmas almacenadas en la blacklist. Para considerar válida la detección, es necesario comprobar que el hash del archivo coincida con el valor configurado y que el daemon registre la alerta correspondiente.

4. El mecanismo de cuarentena implementado mediante permisos `0000` constituye una demostración del tratamiento de un archivo detectado. Su efectividad debe confirmarse consultando los permisos reales con `stat`; el mensaje del log, por sí solo, no demuestra que el cambio de permisos se haya completado.

5. La API HTTP y el dashboard proporcionan una interfaz para consultar el estado del sistema. Sin embargo, la autenticación y la autorización deben reforzarse antes de considerar el sistema apropiado para un entorno de producción.

6. La corrección del cierre del daemon demuestra la importancia de coordinar adecuadamente las señales, los hilos y las llamadas bloqueantes. La prueba de detención controlada debe mantenerse como parte de la validación del proyecto.

7. Finalmente, el sistema constituye una base funcional para el monitoreo y la seguridad a nivel de sistema operativo. Para aumentar su confiabilidad, es necesario completar las funciones pendientes, ampliar la validación del ciclo de escritura de archivos, verificar los resultados mediante evidencias reproducibles y reforzar los mecanismos de seguridad de la API.

---

**Nota final:** las pruebas deben realizarse en un entorno controlado y con archivos de prueba. Antes de entregar el proyecto, se recomienda guardar evidencias de la compilación, el estado de la API, los eventos del log, el hash SHA-256 y los permisos finales del archivo malicioso simulado.
