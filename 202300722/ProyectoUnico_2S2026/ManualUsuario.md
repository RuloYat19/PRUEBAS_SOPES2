# Manual de Usuario — Monitor Integral de Seguridad

## Índice

1. [Introducción](#1-introducción)
2. [Requisitos previos](#2-requisitos-previos)
3. [Iniciar el sistema](#3-iniciar-el-sistema)
4. [Acceder al dashboard](#4-acceder-al-dashboard)
5. [Iniciar sesión](#5-iniciar-sesión)
6. [Funciones del usuario común](#6-funciones-del-usuario-común)
7. [Funciones del administrador](#7-funciones-del-administrador)
8. [Interpretación de alertas](#8-interpretación-de-alertas)
9. [Monitoreo y análisis de archivos](#9-monitoreo-y-análisis-de-archivos)
10. [Prueba de detección con archivos de laboratorio](#10-prueba-de-detección-con-archivos-de-laboratorio)
11. [Cerrar sesión y detener el daemon](#11-cerrar-sesión-y-detener-el-daemon)
12. [Diagnóstico de problemas](#12-diagnóstico-de-problemas)
13. [Recomendaciones de seguridad](#13-recomendaciones-de-seguridad)
14. [Evidencias de funcionamiento](#14-evidencias-de-funcionamiento)

---

## 1. Introducción

El Monitor Integral de Seguridad es una aplicación desarrollada para observar eventos del sistema Linux, consultar información de procesos, registrar alertas y detectar archivos que coincidan con firmas SHA-256 almacenadas en una blacklist.

El sistema está compuesto por un kernel Linux modificado, un daemon desarrollado en C, un sensor eBPF y una interfaz web.

El dashboard permite consultar la información que proporciona el daemon a través de una API HTTP local.

La versión del kernel utilizada en el proyecto es **Linux 6.18.48**.

Este manual explica cómo iniciar el sistema, acceder a la interfaz, consultar la información disponible, realizar pruebas con archivos de laboratorio y solucionar problemas básicos.

## 2. Requisitos previos

Antes de utilizar el sistema, se debe contar con:

- Una máquina virtual o equipo con Linux.
- El kernel Linux 6.18.48 con las modificaciones del proyecto.
- El proyecto compilado correctamente.
- Las dependencias necesarias para ejecutar el daemon y cargar el sensor eBPF.
- Permisos de administrador para iniciar o detener el daemon y realizar las operaciones que lo requieran.
- Un navegador web para acceder al dashboard.

La aplicación utiliza los siguientes recursos locales:

| Recurso | Ubicación |
|---|---|
| Programa intermedio | `ProgramaIntermedio/` |
| Registro de eventos | `/tmp/proyecto_unico.log` |
| Archivo PID del daemon | `/tmp/proyecto_unico.pid` |
| Directorio de pruebas monitoreado | `/tmp/proyecto_monitoreado` |
| Dashboard web | `http://127.0.0.1:8080/` |
| API de estado | `http://127.0.0.1:8080/api/status` |

El directorio monitoreado indicado corresponde al utilizado durante las pruebas. Si la configuración del proyecto establece otra ruta, se debe utilizar la ruta configurada realmente.

## 3. Iniciar el sistema

### 3.1. Iniciar el kernel correcto

Arrancar la máquina utilizando el kernel Linux 6.18.48 que contiene las syscalls integradas para el proyecto.

Una vez iniciada la sesión, comprobar la versión:

```bash
uname -r
```

El resultado debe corresponder al kernel modificado que se utilizará para la evaluación.

### 3.2. Compilar el programa

Abrir una terminal y acceder al directorio `ProgramaIntermedio`:

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

Si la compilación termina correctamente, debe generarse el ejecutable `daemon`.

Comprobar su existencia:

```bash
ls -l daemon
```

Si aparecen errores durante la compilación, deben corregirse antes de continuar.

### 3.3. Iniciar el daemon

Para iniciar el daemon mediante el script utilizado en las pruebas:

```bash
sudo ./start.sh
```

Si se desea ejecutar directamente el binario en modo normal:

```bash
sudo ./daemon
```

Para depuración en primer plano, si la opción está implementada en la versión compilada:

```bash
sudo ./daemon --foreground
```

El modo normal está diseñado para desacoplar el daemon de la terminal y registrar sus mensajes en `/tmp/proyecto_unico.log`.

### 3.4. Comprobar que el sistema se inició

Consultar el proceso:

```bash
pgrep -af '(^|/)daemon$'
```

Consultar el PID:

```bash
sudo cat /tmp/proyecto_unico.pid
```

Consultar los mensajes de inicio:

```bash
sudo tail -n 30 /tmp/proyecto_unico.log
```

Se deben revisar los mensajes relacionados con el inicio del daemon, la activación del monitoreo, el sensor eBPF y el servidor HTTP.

Entre los mensajes observados durante las pruebas se encuentran:

```text
Daemon iniciado PID=...
log_watch iniciado ID=...
HTTP escuchando en http://127.0.0.1:8080
Sensor eBPF activo en /tmp/proyecto_monitoreado
```

Los identificadores y las horas cambiarán en cada ejecución.

## 4. Acceder al dashboard

Con el daemon activo, abrir el siguiente enlace en el navegador:

**http://127.0.0.1:8080/**

La interfaz permite consultar la información que el daemon expone a través de la API.

Si la página no carga, comprobar primero que el proceso siga activo y revisar el registro de eventos.

También se puede verificar la API directamente desde la terminal:

```bash
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool
```

Si la API responde con un JSON válido, significa que el endpoint está respondiendo. Esto no garantiza por sí solo que todas las funciones del dashboard estén operativas.

## 5. Iniciar sesión

El sistema contempla autenticación mediante PAM utilizando cuentas locales de Linux.

Para iniciar sesión:

1. Abrir el dashboard.
2. Introducir el nombre de usuario de la cuenta local.
3. Introducir la contraseña correspondiente.
4. Enviar el formulario de inicio de sesión.
5. Verificar que la aplicación permita el acceso y asigne el rol correspondiente, según la implementación.

### 5.1. Grupos de usuarios

El diseño contempla dos grupos:

- `admin_user`: grupo destinado a usuarios administradores.
- `common_user`: grupo destinado a usuarios comunes.

Para consultar los grupos de una cuenta:

```bash
groups NOMBRE_USUARIO
```

Para consultar la pertenencia a los grupos desde la terminal:

```bash
getent group admin_user
getent group common_user
```

Si es necesario crear los grupos, un administrador puede ejecutar:

```bash
sudo groupadd -f admin_user
sudo groupadd -f common_user
```

Agregar una cuenta al grupo administrativo:

```bash
sudo usermod -aG admin_user NOMBRE_USUARIO
```

Agregar una cuenta al grupo común:

```bash
sudo usermod -aG common_user NOMBRE_USUARIO
```

Se debe reemplazar `NOMBRE_USUARIO` por la cuenta local correspondiente.

Después de modificar los grupos, cerrar y volver a iniciar la sesión del usuario para actualizar sus grupos efectivos.

**Nota:** el comportamiento cuando una cuenta pertenece a ambos grupos depende de las reglas de asignación de roles implementadas en el daemon. La pertenencia a un grupo no debe considerarse una prueba suficiente de autorización hasta verificar el comportamiento real de la aplicación.

## 6. Funciones del usuario común

El usuario común está destinado a consultar información del sistema sin realizar operaciones administrativas.

Según las funciones habilitadas en la versión instalada, puede consultar:

- Métricas disponibles del sistema.
- Información sobre procesos y sus estados.
- Alertas registradas.
- Archivos observados y sus hashes SHA-256.
- Estado de los servicios de monitoreo.

El usuario común no debería tener acceso a controles administrativos, como modificar la configuración del monitoreo o ejecutar operaciones privilegiadas.

La disponibilidad exacta de cada apartado depende de la implementación del dashboard y de los datos que proporcione la API.

## 7. Funciones del administrador

El administrador está destinado a gestionar las funciones administrativas que estén implementadas y habilitadas en el sistema.

Entre las operaciones previstas para el proyecto se encuentran:

- Activar o desactivar las métricas.
- Activar o desactivar el escaneo de archivos.
- Recargar la blacklist.
- Configurar las rutas de monitoreo.
- Administrar el monitoreo de logs.
- Consultar información de un proceso por PID.
- Terminar procesos, cuando esta función esté habilitada.
- Consultar archivos puestos en cuarentena.
- Poner archivos en cuarentena manualmente, cuando la función esté implementada.

**Importante:** las operaciones enumeradas en los últimos puntos son funciones previstas del proyecto y no deben darse por disponibles únicamente porque aparezcan en este manual. Deben comprobarse en la interfaz y validarse mediante una prueba antes de considerarlas terminadas.

Las operaciones administrativas deben contar con autorización adecuada y, cuando corresponda, solicitar confirmación antes de ejecutar una acción que afecte al sistema.

## 8. Interpretación de alertas

Las alertas permiten identificar eventos que requieren atención o que documentan las actividades del sistema.

Las severidades previstas en el proyecto son:

| Severidad | Significado | Ejemplos |
|---|---|---|
| `LOW` | Evento informativo o de baja prioridad. | Activación del monitoreo o inicio de sesión exitoso. |
| `MEDIUM` | Advertencia que requiere revisión. | Coincidencia con una palabra clave en un log, inicio de sesión fallido o desactivación del monitoreo. |
| `HIGH` | Evento crítico según las reglas de la aplicación. | Detección de un hash malicioso o aplicación de una medida de cuarentena. |

La severidad real depende de la configuración de cada evento. Por ejemplo, en la prueba realizada, la firma `Simulated.Adware.Popup` estaba configurada con severidad `LOW`, por lo que el registro correspondiente no debe interpretarse como una alerta `HIGH` si no se cambia esa configuración.

Las alertas pueden incluir:

- Fecha y hora del evento.
- Tipo de alerta.
- Severidad.
- Descripción del evento.

Para consultar las alertas registradas en el log:

```bash
sudo grep -i 'ALERTA' /tmp/proyecto_unico.log
```

Para revisar el registro completo:

```bash
sudo less /tmp/proyecto_unico.log
```

Presionar `q` para salir de `less`.

## 9. Monitoreo y análisis de archivos

El sistema utiliza un sensor eBPF para observar determinadas operaciones sobre archivos que cumplen las condiciones configuradas.

Durante las pruebas se utilizó el directorio:

```text
/tmp/proyecto_monitoreado
```

El sensor de esta versión observa operaciones de apertura mediante `openat` con indicadores de escritura o creación y filtra las rutas configuradas. Por ello, no se debe asumir que monitorea todas las modificaciones de archivos en el sistema.

### 9.1. Archivos benignos

Un archivo benigno es aquel cuyo hash no coincide con una firma maliciosa registrada en la blacklist.

Para crear un archivo de prueba:

```bash
echo "Archivo benigno de prueba" \
  | sudo tee /tmp/proyecto_monitoreado/prueba_benigna.txt
```

Después, consultar el log:

```bash
sudo tail -n 30 /tmp/proyecto_unico.log
```

Si la operación cumple los filtros del sensor, se espera observar un evento asociado a la ruta del archivo.

### 9.2. Archivos maliciosos simulados

El proyecto utiliza el archivo de laboratorio `Simulated.Adware.Popup.txt` para probar la comparación de hashes.

Cuando el archivo coincide con una firma registrada, el daemon puede generar una alerta y aplicar la acción de cuarentena configurada.

En la versión probada, la cuarentena básica consiste en cambiar los permisos del archivo a `0000`.

Esto no elimina el archivo ni lo mueve a otro directorio. Se recomienda utilizar exclusivamente archivos descartables creados para las pruebas.

### 9.3. Consultar los archivos y sus permisos

Para listar el contenido del directorio:

```bash
sudo ls -la /tmp/proyecto_monitoreado
```

Para consultar la ruta, los permisos y la información del archivo:

```bash
sudo stat /tmp/proyecto_monitoreado/prueba_benigna.txt
```

Si un archivo se detectó como malicioso, consultar también su registro en el log y verificar sus permisos reales. No basta con que el log indique que se intentó ponerlo en cuarentena.

## 10. Prueba de detección con archivos de laboratorio

Esta sección permite reproducir la prueba de detección realizada durante el desarrollo.

**Advertencia:** realizar estas pruebas únicamente en el entorno de laboratorio. No copiar archivos sospechosos a directorios personales o del sistema, ni modificar permisos de archivos importantes.

### 10.1. Verificar el archivo original

El archivo de prueba original se encuentra en:

```text
~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/BD_ProyectoUnico/test_files/Simulated.Adware.Popup.txt
```

Calcular su hash SHA-256:

```bash
sha256sum \
  ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/BD_ProyectoUnico/test_files/Simulated.Adware.Popup.txt
```

El hash esperado es:

```text
470eed5b6f546e420020e5b6e0bc2813022334eef3c0ed4794fe030eb7822adb
```

Si el resultado es diferente, comprobar que el archivo original no haya sido modificado.

### 10.2. Copiar el archivo al directorio monitoreado

Ejecutar:

```bash
sudo cp \
  ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/BD_ProyectoUnico/test_files/Simulated.Adware.Popup.txt \
  /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

### 10.3. Revisar los eventos

Consultar el log:

```bash
sudo tail -n 50 /tmp/proyecto_unico.log
```

También se pueden filtrar los mensajes relevantes:

```bash
sudo grep -E \
'eBPF evento|Hash malicioso|archivo puesto en cuarentena' \
/tmp/proyecto_unico.log
```

En las pruebas realizadas se observaron eventos eBPF para el archivo benigno y para `Simulated.Adware.Popup.txt`. Además, el daemon registró una coincidencia con la firma configurada y un mensaje indicando que el archivo había sido puesto en cuarentena.

### 10.4. Verificar el hash de la copia

Ejecutar:

```bash
sudo sha256sum \
  /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

El resultado debe coincidir con el hash esperado de la sección 10.1.

### 10.5. Verificar la cuarentena

Ejecutar:

```bash
sudo stat -c '%a %n' \
  /tmp/proyecto_monitoreado/prueba_benigna.txt \
  /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

Interpretación:

- El archivo benigno debe conservar los permisos que tenía antes de la prueba.
- El archivo malicioso simulado debe mostrar `0` si el cambio de permisos a `0000` se completó correctamente.

Si el archivo malicioso no muestra permisos `0`, la cuarentena no se ha confirmado y se debe revisar el log y la implementación.

La verificación del hash, la alerta y los permisos permite documentar por separado la detección y la aplicación de la medida de cuarentena.

## 11. Cerrar sesión y detener el daemon

### 11.1. Cerrar sesión en el dashboard

El botón **Cerrar sesión**, si está implementado, cierra la sesión de la interfaz web.

Cerrar la sesión del dashboard no implica detener el daemon. El servicio puede seguir ejecutándose y monitoreando el sistema.

### 11.2. Detener el daemon desde la terminal

Para enviar una señal de terminación:

```bash
sudo kill -TERM "$(sudo cat /tmp/proyecto_unico.pid)"
```

Esperar unos segundos:

```bash
sleep 2
```

Comprobar que ya no esté activo:

```bash
pgrep -af '(^|/)daemon$'
```

Consultar el final del log:

```bash
sudo tail -n 30 /tmp/proyecto_unico.log
```

Se espera que el daemon registre un mensaje similar a:

```text
Daemon detenido limpiamente
```

Durante las pruebas se corrigió el bloqueo que ocurría al detener el hilo IPC. La prueba posterior de `SIGTERM` finalizó correctamente, por lo que se recomienda conservar esa implementación y verificar el cierre después de cualquier modificación del código.

## 12. Diagnóstico de problemas

### 12.1. El dashboard no responde

Comprobar si el daemon está activo:

```bash
pgrep -af '(^|/)daemon$'
```

Revisar el log:

```bash
sudo tail -n 50 /tmp/proyecto_unico.log
```

Consultar la API:

```bash
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool
```

Si la conexión falla, verificar que el servidor HTTP haya iniciado y que esté utilizando el puerto configurado.

### 12.2. No aparecen archivos en el monitoreo

Comprobar el directorio:

```bash
sudo ls -la /tmp/proyecto_monitoreado
```

Revisar los mensajes del sensor:

```bash
sudo grep -E 'Sensor eBPF|eBPF evento' /tmp/proyecto_unico.log
```

Confirmar que:

- El daemon esté activo.
- El sensor eBPF se haya cargado correctamente.
- La ruta corresponda al directorio monitoreado.
- La operación realizada cumpla los filtros configurados.
- Se estén consultando los mensajes del log correcto.

### 12.3. El archivo no se detecta como malicioso

Calcular su hash:

```bash
sudo sha256sum /tmp/proyecto_monitoreado/Simulated.Adware.Popup.txt
```

Revisar la blacklist desde el directorio del proyecto:

```bash
cd ~/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/202300722/ProyectoUnico_2S2026/ProgramaIntermedio
cat blacklist.json
```

El hash debe coincidir exactamente con la firma configurada. También se debe verificar que el daemon esté utilizando esa blacklist y que el evento del archivo llegue al proceso.

### 12.4. El usuario no puede iniciar sesión

Comprobar:

- Que el nombre de usuario sea correcto.
- Que la contraseña sea válida.
- Que la cuenta local exista.
- Que la pertenencia a los grupos corresponda a la configuración del proyecto.
- Que la política PAM permita la autenticación.

Consultar los grupos de la cuenta:

```bash
groups NOMBRE_USUARIO
```

Si se modificaron los grupos recientemente, cerrar y volver a iniciar la sesión.

### 12.5. El daemon no termina

Comprobar el PID y el proceso:

```bash
sudo cat /tmp/proyecto_unico.pid
pgrep -af '(^|/)daemon$'
```

Enviar la señal de terminación:

```bash
sudo kill -TERM "$(sudo cat /tmp/proyecto_unico.pid)"
```

Revisar los últimos mensajes del log:

```bash
sudo tail -n 50 /tmp/proyecto_unico.log
```

Si el proceso continúa activo, no eliminar manualmente el archivo PID como primera medida. Primero se debe determinar si el proceso sigue ejecutándose y revisar si algún hilo está bloqueado.

## 13. Recomendaciones de seguridad

1. Utilizar únicamente cuentas autorizadas para acceder al dashboard.
2. No compartir contraseñas ni almacenarlas en archivos de configuración.
3. Probar la detección con archivos descartables y creados para el laboratorio.
4. No borrar archivos importantes ni cambiar sus permisos para probar la cuarentena.
5. Verificar los permisos reales del archivo después de cada detección.
6. Revisar periódicamente `/tmp/proyecto_unico.log`.
7. No asumir que una alerta demuestra que el sistema detecta cualquier tipo de malware.
8. No considerar la cabecera `X-Role` una protección suficiente para una aplicación de producción.
9. Confirmar que las acciones administrativas estén autorizadas y que su resultado se verifique.
10. Detener el daemon mediante `SIGTERM` para permitir que finalice sus hilos y libere los recursos.
11. Mantener una copia de seguridad de los archivos del kernel y de la configuración del proyecto antes de modificarlos.

## 14. Evidencias de funcionamiento

Para documentar el funcionamiento del proyecto, se recomienda guardar capturas de pantalla de las siguientes pruebas:

| N.º | Evidencia | Qué debe mostrar |
|---:|---|---|
| 1 | Versión del kernel | La salida de `uname -r` con la versión utilizada. |
| 2 | Compilación | La ejecución de `make` finalizada correctamente. |
| 3 | Inicio del daemon | El proceso activo y los mensajes de inicio en el log. |
| 4 | Dashboard | La interfaz abierta en el navegador. |
| 5 | Inicio de sesión | El resultado de la autenticación de una cuenta autorizada. |
| 6 | Usuario común | La información de consulta disponible para ese rol. |
| 7 | Administrador | Los controles administrativos que estén realmente implementados. |
| 8 | Archivo benigno | El evento del sensor y el estado del archivo de prueba. |
| 9 | Archivo malicioso simulado | La coincidencia con la firma y la alerta del daemon. |
| 10 | Hash SHA-256 | La salida de `sha256sum` con el hash esperado. |
| 11 | Cuarentena | La salida de `stat` que confirme los permisos `0000`. |
| 12 | Detención | El mensaje de cierre limpio y la ausencia del proceso. |

Las capturas deben obtenerse después de ejecutar las pruebas en la máquina virtual. No deben sustituirse los resultados reales por imágenes de ejemplo.

---