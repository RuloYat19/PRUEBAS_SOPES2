# Universidad de San Carlos de Guatemala

## Facultad de Ingeniería

## Ingeniería en Ciencias y Sistemas

## Sistemas Operativos 2

![Logo USAC](imagenes/LogoUSAC.jpeg)

# PRÁCTICA #4

## Integración de monitoreo de procesos, memoria, IPC y eventos de seguridad

### Informe de Desarrollo

  Nombre                      Carnet
  --------------------------- -----------
  Raúl Emanuel Yat Cancinos   202300722

**Kernel utilizado:** Linux 6.18.48\
**Entorno:** Ubuntu 24.04 LTS en VirtualBox\
**Lenguaje:** C\
**Fecha de elaboración:** 07/10/2026

------------------------------------------------------------------------

# Tabla de Contenido

1.  [Introducción](#1-introducción)
2.  [Objetivos](#2-objetivos)
3.  [Descripción general](#3-descripción-general)
4.  [Entorno y estructura](#4-entorno-y-estructura)
5.  [Syscalls utilizadas](#5-syscalls-utilizadas)
6.  [Arquitectura de la solución](#6-arquitectura-de-la-solución)
7.  [Daemon](#7-daemon)
8.  [Dashboard](#8-dashboard)
9.  [Compilación y ejecución](#9-compilación-y-ejecución)
10. [Pruebas](#10-pruebas)
11. [Problemas encontrados y
    soluciones](#11-problemas-encontrados-y-soluciones)
12. [Verificación final](#12-verificación-final)
13. [Conclusiones](#13-conclusiones)
14. [Lecciones aprendidas](#14-lecciones-aprendidas)
15. [Referencias](#15-referencias)
16. [Anexos](#16-anexos)

------------------------------------------------------------------------

# 1. Introducción

La presente práctica integra las funcionalidades desarrolladas en el
kernel de Linux con un daemon de espacio de usuario y un Dashboard web.

El sistema permite consultar información de procesos, obtener
estadísticas de memoria, recibir eventos generados por `log_watch`
mediante IPC y presentar la información en una interfaz web.

El flujo principal es:

``` text
Kernel Linux 6.18.48
        |
        +--> Process Monitor (470-471)
        |
        +--> IPC (472-473)
        |
        +--> Log Watch (474-475)
                    |
                    v
                  Daemon
                    |
             +------+------+
             |             |
             v             v
          API HTTP     Eventos
             |
             v
         Dashboard
```

------------------------------------------------------------------------

# 2. Objetivos

## 2.1 Objetivo general

Integrar el monitoreo de procesos, memoria, comunicación IPC y detección
de eventos de seguridad en un daemon, proporcionando un Dashboard para
visualizar la información.

## 2.2 Objetivos específicos

-   Utilizar las syscalls de monitoreo de procesos.
-   Consultar el estado global de los procesos.
-   Obtener información individual de procesos.
-   Obtener estadísticas de memoria.
-   Integrar `ipc_channel_receive()` con el daemon.
-   Integrar `log_watch` con IPC.
-   Detectar la palabra clave `ERROR`.
-   Registrar eventos de seguridad.
-   Exponer la información mediante una API HTTP.
-   Visualizar la información mediante un Dashboard.
-   Validar el sistema mediante pruebas reproducibles.

------------------------------------------------------------------------

# 3. Descripción general

La práctica integra las funcionalidades de las prácticas anteriores en
una aplicación daemon.

Cuando `log_watch` detecta `ERROR` en `/tmp/practica4.log`, genera un
evento que se coloca en la cola IPC del kernel. El daemon recibe el
mensaje, lo procesa y lo almacena en `security_events[]`.

Finalmente, `/api/status` expone la información y el Dashboard la
muestra al usuario.

``` text
echo "ERROR ..." >> /tmp/practica4.log
                |
                v
          log_watch.c
                |
                v
        Cola IPC del kernel
                |
                v
   ipc_channel_receive() [473]
                |
                v
             daemon
                |
                v
       security_events[]
                |
                v
          /api/status
                |
                v
           Dashboard
```

------------------------------------------------------------------------

# 4. Entorno y estructura

## 4.1 Kernel

Código fuente:

``` bash
~/Escritorio/SistemasOperativos2/mapa/Practica_3_2S2026/linux-6.18.48
```

Verificación:

``` bash
cd ~/Escritorio/SistemasOperativos2/mapa/Practica_3_2S2026/linux-6.18.48
make kernelversion
uname -r
```

Resultado esperado:

``` text
6.18.48
```

## 4.2 Proyecto

``` text
Practica_4_2S2026/
├── daemon/
│   ├── daemon.c
│   ├── Makefile
│   └── daemon
└── dashboard/
    ├── index.html
    ├── style.css
    └── app.js
```

------------------------------------------------------------------------

# 5. Syscalls utilizadas

Las entradas utilizadas en `arch/x86/entry/syscalls/syscall_64.tbl` son:

``` text
470    common    get_process_info        sys_get_process_info
471    common    get_processes_state     sys_get_processes_state
472    common    ipc_channel_send        sys_ipc_channel_send
473    common    ipc_channel_receive     sys_ipc_channel_receive
474    common    start_log_watch         sys_start_log_watch
475    common    stop_log_watch          sys_stop_log_watch
```

Verificación:

``` bash
cd ~/Escritorio/SistemasOperativos2/mapa/Practica_3_2S2026/linux-6.18.48

grep -nE '470|471|472|473|474|475' arch/x86/entry/syscalls/syscall_64.tbl
```

## 5.1 Process Monitor

`get_process_info` (470) obtiene información de un proceso.

`get_processes_state` (471) obtiene:

``` text
total
running
sleeping
zombie
```

## 5.2 IPC

`ipc_channel_send` (472) agrega mensajes a la cola.

`ipc_channel_receive` (473) recibe mensajes y bloquea al receptor
mientras la cola está vacía.

## 5.3 Log Watch

`start_log_watch` (474) inicia el monitor.

`stop_log_watch` (475) detiene un monitor mediante su ID.

En esta práctica el daemon monitorea:

``` text
/tmp/practica4.log
```

Palabra clave:

``` text
ERROR
```

Log central:

``` text
/tmp/practica4_central.log
```

------------------------------------------------------------------------

# 6. Arquitectura de la solución

``` text
                    KERNEL
                      |
       +--------------+--------------+
       |              |              |
       v              v              v
   Process           IPC         Log Watch
   Monitor        472-473         474-475
   470-471             ^              |
       |               |              |
       +---------------+--------------+
                       |
                       v
                    DAEMON
                       |
              +--------+--------+
              |                 |
              v                 v
          API HTTP        Security Events
              |
              v
          DASHBOARD
```

El daemon:

-   consulta procesos;
-   obtiene memoria;
-   inicia `log_watch`;
-   recibe eventos IPC;
-   almacena eventos;
-   proporciona `/api/status`;
-   sirve el Dashboard.

------------------------------------------------------------------------

# 7. Daemon

## 7.1 Procesos

El daemon utiliza:

``` text
SYS_GET_PROCESS_INFO = 470
SYS_GET_PROCESSES_STATE = 471
```

También recorre `/proc` para construir la tabla de procesos.

Los datos incluyen:

``` text
PID
Nombre
Runtime
Memoria
```

## 7.2 Memoria

La información se obtiene desde:

``` text
/proc/meminfo
```

Se presentan:

``` text
Memoria total
Memoria utilizada
Memoria libre
Memoria disponible
Cache
Swap total
Swap utilizada
```

## 7.3 Eventos IPC

El daemon posee un hilo dedicado a:

``` c
syscall(SYS_IPC_CHANNEL_RECEIVE, message);
```

El evento recibido tiene la forma:

``` text
archivo_origen=/tmp/practica4.log|palabra_detectada=ERROR|contenido=ERROR prueba IPC
```

Luego se ejecuta el procesamiento del evento y se agrega a:

``` text
security_events[]
```

## 7.4 API

El servidor HTTP utiliza:

``` text
127.0.0.1:8080
```

Endpoints:

``` text
/api/status
/api/health
```

------------------------------------------------------------------------

# 8. Dashboard

Los archivos son:

``` text
dashboard/
├── index.html
├── style.css
└── app.js
```

El Dashboard muestra:

-   versión del kernel;
-   memoria;
-   procesos;
-   tabla de procesos;
-   búsqueda de procesos;
-   eventos de seguridad.

La información se actualiza consultando:

``` text
http://127.0.0.1:8080/api/status
```

------------------------------------------------------------------------

# 9. Compilación y ejecución

## 9.1 Compilar

``` bash
cd ~/Escritorio/SistemasOperativos2/mapa/Practica_4_2S2026/daemon

make clean
make
```

Verificar:

``` bash
ls -lh daemon
```

## 9.2 Ejecutar

Es importante ejecutar el daemon desde `daemon/` porque el Dashboard se
encuentra en `../dashboard`.

``` bash
cd ~/Escritorio/SistemasOperativos2/mapa/Practica_4_2S2026/daemon
./daemon
```

Debe aparecer información similar a:

``` text
========================================
       DAEMON PRACTICA 4 - SO2
       Kernel 6.18.48
========================================

[P2] Procesos:
  Total: ...
  Running: ...
  Sleeping: ...
  Zombie: ...

[MEMORIA]
  Usada: ...
  Libre: ...
  Cache: ...
  Swap usada: ...

[DAEMON] Log monitor iniciado. ID=...

[DAEMON] Sistema listo.
[DAEMON] Hilo IPC iniciado.
[DAEMON] Esperando eventos de seguridad...
```

Mantener esta terminal abierta durante las pruebas.

------------------------------------------------------------------------

# 10. Pruebas

## 10.1 Verificar kernel

``` bash
uname -r
```

Esperado:

``` text
6.18.48
```

También:

``` bash
cd ~/Escritorio/SistemasOperativos2/mapa/Practica_3_2S2026/linux-6.18.48
make kernelversion
```

Esperado:

``` text
6.18.48
```

**Evidencia:**

``` text
[PEGAR AQUÍ LA SALIDA REAL]
```

## 10.2 Verificar syscalls

``` bash
grep -nE '470|471|472|473|474|475' arch/x86/entry/syscalls/syscall_64.tbl
```

## 10.3 Probar API

Con el daemon ejecutándose:

``` bash
curl -s http://127.0.0.1:8080/api/status
```

Para observar procesos:

``` bash
curl -s http://127.0.0.1:8080/api/status | grep -o '"processes_state":{[^}]*}'
```

Para observar memoria:

``` bash
curl -s http://127.0.0.1:8080/api/status | grep -o '"memory":{[^}]*}'
```

## 10.4 Preparar archivos

``` bash
touch /tmp/practica4.log
touch /tmp/practica4_central.log
```

Limpiar el log central:

``` bash
truncate -s 0 /tmp/practica4_central.log
```

## 10.5 Prueba 1: log_watch

``` bash
echo "ERROR prueba 1 de seguridad P4" >> /tmp/practica4.log
sleep 5
cat /tmp/practica4_central.log
```

Debe aparecer una línea similar a:

``` text
[archivo=/tmp/practica4.log] [palabra_clave=ERROR] ERROR prueba 1 de seguridad P4
```

## 10.6 Prueba 2: IPC

``` bash
echo "ERROR prueba IPC P4" >> /tmp/practica4.log
sleep 5
```

En la terminal del daemon debe aparecer:

``` text
[DAEMON] Evento IPC recibido:
archivo_origen=/tmp/practica4.log|palabra_detectada=ERROR|contenido=ERROR prueba IPC P4
```

## 10.7 Prueba 3: evento de seguridad

``` bash
echo "ERROR prueba IPC corregido" >> /tmp/practica4.log
sleep 5
```

Consultar:

``` bash
curl -s http://127.0.0.1:8080/api/status | grep -o '"security_events":\[[^]]*\]'
```

Resultado esperado:

``` json
"security_events":[{
    "archivo":"/tmp/practica4.log",
    "palabra":"ERROR",
    "contenido":"ERROR prueba IPC corregido",
    "timestamp":"2026-10-07 19:22:18"
}]
```

El timestamp variará según la hora de ejecución.

## 10.8 Verificar log central

``` bash
cat /tmp/practica4_central.log
```

## 10.9 Verificar Dashboard

Abrir:

``` text
http://127.0.0.1:8080
```

Generar un evento:

``` bash
echo "ERROR prueba final dashboard P4" >> /tmp/practica4.log
sleep 5
```

Actualizar el Dashboard.

Debe aparecer el evento en la sección de seguridad.

**Evidencia:**

``` text
[AGREGAR AQUÍ CAPTURA DEL DASHBOARD]
```

## 10.10 Prueba de reinicio

Detener:

``` text
Ctrl+C
```

Volver a iniciar:

``` bash
cd ~/Escritorio/SistemasOperativos2/mapa/Practica_4_2S2026/daemon
./daemon
```

Debe iniciar nuevamente sin errores.

------------------------------------------------------------------------

# 11. Problemas encontrados y soluciones

## 11.1 Los eventos no aparecían en `security_events`

`log_watch` detectaba correctamente `ERROR` y escribía en:

``` text
/tmp/practica4_central.log
```

Sin embargo, los eventos no aparecían en el Dashboard.

La causa estaba en el daemon:

``` c
if (result > 0) {
    process_ipc_event(message);
}
```

La syscall `ipc_channel_receive()` retorna `0` cuando la recepción es
exitosa. Por lo tanto, el mensaje era recibido pero no procesado.

La solución fue procesar el mensaje siempre que `result >= 0`:

``` c
long result = syscall(
    SYS_IPC_CHANNEL_RECEIVE,
    message
);

if (result < 0) {
    if (!running)
        break;

    if (errno == EINTR)
        continue;

    usleep(100000);
    continue;
}

message[sizeof(message) - 1] = '