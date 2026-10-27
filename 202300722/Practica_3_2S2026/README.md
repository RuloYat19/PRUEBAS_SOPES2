# Universidad de San Carlos de Guatemala
## Facultad de Ingeniería
## Ingeniería en Ciencias y Sistemas
## Sistemas Operativos 2

![Logo USAC](imagenes/LogoUSAC.jpeg)

# PRÁCTICA #3
## Sincronización e Instrumentación: Comunicación entre Procesos y Monitoreo de Eventos

### Informe de Desarrollo

| Nombre | Carnet |
|---|---|
| Raúl Emanuel Yat Cancinos | 202300722 |

**Kernel requerido:** Linux 6.12.69 LTS  
**Entorno:** Ubuntu 24.04 LTS en VirtualBox  
**Lenguaje:** C  
**Fecha de asignación:** 04/09/2026  
**Fecha de elaboración:** 04/09/2026 – 17/09/2026

---

# Tabla de Contenido

1. [Introducción](#1-introducción)
2. [Objetivos](#2-objetivos)
3. [Descripción del problema](#3-descripción-del-problema)
4. [Preparación del entorno](#4-preparación-del-entorno)
   - [4.1 Crear respaldo y directorios](#41-crear-respaldo-y-directorios)
   - [4.2 Instalar dependencias](#42-instalar-dependencias)
   - [4.3 Obtener Linux 6.12.69](#43-obtener-linux-61269)
   - [4.4 Configurar el kernel](#44-configurar-el-kernel)
5. [Implementación del canal IPC](#5-implementación-del-canal-ipc)
   - [5.1 Diseño](#51-diseño)
   - [5.2 Estructura de datos](#52-estructura-de-datos)
   - [5.3 Syscall send](#53-syscall-send)
   - [5.4 Syscall receive](#54-syscall-receive)
   - [5.5 Makefile](#55-makefile)
6. [Implementación de log_watch](#6-implementación-de-log_watch)
   - [6.1 Diseño](#61-diseño)
   - [6.2 Inicio del monitor](#62-inicio-del-monitor)
   - [6.3 Hilo de kernel](#63-hilo-de-kernel)
   - [6.4 Detención del monitor](#64-detención-del-monitor)
7. [Registro de las syscalls](#7-registro-de-las-syscalls)
8. [Compilación e instalación](#8-compilación-e-instalación)
9. [Programas de usuario](#9-programas-de-usuario)
   - [9.1 Enviar IPC](#91-enviar-ipc)
   - [9.2 Recibir IPC](#92-recibir-ipc)
   - [9.3 Iniciar log_watch](#93-iniciar-log_watch)
   - [9.4 Detener log_watch](#94-detener-log_watch)
10. [Pruebas](#10-pruebas)
11. [Errores encontrados y soluciones](#11-errores-encontrados-y-soluciones)
12. [Conclusiones](#12-conclusiones)
13. [Lecciones aprendidas](#13-lecciones-aprendidas)
14. [Referencias](#14-referencias)

---

# 1. Introducción

La presente práctica implementa dos mecanismos dentro del kernel de Linux:

1. Un canal de comunicación entre procesos mediante una cola FIFO bloqueante.
2. Un sistema de monitoreo de archivos de log ejecutado mediante un hilo de kernel.

El objetivo es comprender cómo el kernel sincroniza el acceso concurrente a recursos compartidos, cómo un proceso puede quedar bloqueado mediante una *wait queue* sin realizar *polling* y cómo se puede crear y finalizar de forma segura un hilo de kernel.

La práctica exige que las nuevas funcionalidades puedan ser utilizadas desde programas escritos en C en espacio de usuario mediante llamadas al sistema personalizadas.

---

# 2. Objetivos

## 2.1 Objetivo general

Diseñar e implementar dos mecanismos en el kernel de Linux 6.12.69 LTS: un canal IPC basado en una cola bloqueante y un sistema de monitoreo de archivos de log ejecutado por un hilo de kernel independiente.

## 2.2 Objetivos específicos

- Implementar `sys_ipc_channel_send()`.
- Implementar `sys_ipc_channel_receive()`.
- Utilizar `copy_from_user()` y `copy_to_user()` para transferencias seguras.
- Proteger la cola compartida mediante `mutex`.
- Utilizar una `wait_queue` para bloquear al receptor.
- Implementar `sys_start_log_watch()`.
- Crear hilos mediante `kthread_run()`.
- Detenerlos mediante `kthread_stop()`.
- Mantener una lista de monitores activos protegida mediante `mutex`.
- Implementar `sys_stop_log_watch()`.
- Crear programas de usuario para probar las cuatro operaciones.
- Validar el funcionamiento mediante al menos tres pruebas consecutivas de monitoreo.

---

# 3. Descripción del problema

La práctica plantea dos problemas.

## 3.1 Comunicación entre procesos

Debe existir una cola FIFO en el kernel donde un proceso pueda enviar un mensaje y otro proceso pueda recibirlo.

El receptor no debe utilizar un ciclo de espera activa como:

```c
while (cola_vacia()) {
    // esperar
}
```

En su lugar, debe utilizar una *wait queue* para que el proceso quede bloqueado hasta que exista un mensaje.

La cola debe estar protegida ante accesos concurrentes.

## 3.2 Monitoreo de logs

Debe existir una syscall que permita iniciar un monitor que:

- Reciba varias rutas.
- Reciba una palabra clave.
- Cree un hilo de kernel.
- Revise periódicamente los archivos.
- Detecte la palabra clave.
- Registre los hallazgos en un log centralizado.
- Pueda ser detenido posteriormente mediante un identificador único.

Según el enunciado, el registro debe incluir:

- Identificador o ruta del archivo origen.
- Palabra que activó la detección.
- Contenido de la línea detectada.

---

# 4. Preparación del entorno

## 4.1 Crear respaldo y directorios

Antes de modificar el kernel se recomienda crear una copia de seguridad.

```bash
cd ~
cp -a linux-6.12.69 linux-6.12.69-backup
```

Crear una carpeta para los programas de prueba:

```bash
mkdir -p ~/practica3
cd ~/practica3
```

Verificar el sistema:

```bash
cat /etc/os-release
uname -a
gcc --version
make --version
git --version
```

---

## 4.2 Instalar dependencias

Actualizar los repositorios:

```bash
sudo apt update
```

Instalar las herramientas necesarias:

```bash
sudo apt install -y \
build-essential \
bc \
bison \
flex \
libssl-dev \
libelf-dev \
libdw-dev \
dwarves \
gawk \
cpio \
rsync \
kmod \
libncurses-dev \
git \
gdb \
strace
```

Verificar algunas herramientas:

```bash
gcc --version
make --version
gawk --version
git --version
```

---

## 4.3 Obtener Linux 6.12.69

Si todavía no se tiene el código fuente:

```bash
cd ~
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.69.tar.xz
```

Extraer:

```bash
tar -xf linux-6.12.69.tar.xz
```

Entrar al directorio:

```bash
cd ~/linux-6.12.69
```

Verificar la versión del código:

```bash
make kernelversion
```

Debe mostrar:

```text
6.12.69
```

---

## 4.4 Configurar el kernel

Se puede iniciar utilizando la configuración actual del sistema:

```bash
cd ~/linux-6.12.69
cp /boot/config-$(uname -r) .config
```

Actualizar las opciones para la nueva versión:

```bash
make olddefconfig
```

También puede utilizarse:

```bash
make menuconfig
```

Verificar que el kernel utilizado para la práctica sea 6.12.69:

```bash
make kernelversion
```

Resultado esperado:

```text
6.12.69
```

---

# 5. Implementación del canal IPC

## 5.1 Diseño

El canal IPC utiliza:

```text
Programa de usuario
       |
       v
sys_ipc_channel_send()
       |
       v
copy_from_user()
       |
       v
Cola FIFO del kernel
       |
       +---- mutex
       |
       +---- wait queue
       |
       v
sys_ipc_channel_receive()
       |
       v
copy_to_user()
       |
       v
Programa receptor
```

El envío agrega un mensaje a la cola y despierta a los procesos que estén esperando.

El receptor se bloquea mediante `wait_event_interruptible()` mientras la cola esté vacía.

---

## 5.2 Estructura de datos

Se puede crear el archivo:

```bash
cd ~/linux-6.12.69
nano kernel/ipc_channel.c
```

Una implementación base es:

```c
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/errno.h>

#define IPC_MAX_MESSAGE 256

struct ipc_message {
    struct list_head list;
    char message[IPC_MAX_MESSAGE];
};

static LIST_HEAD(ipc_queue);
static DEFINE_MUTEX(ipc_mutex);
static DECLARE_WAIT_QUEUE_HEAD(ipc_wait_queue);

SYSCALL_DEFINE1(ipc_channel_send, char __user *, mensaje)
{
    struct ipc_message *new_message;

    new_message = kmalloc(sizeof(*new_message), GFP_KERNEL);
    if (!new_message)
        return -ENOMEM;

    if (copy_from_user(new_message->message,
                       mensaje,
                       IPC_MAX_MESSAGE - 1)) {
        kfree(new_message);
        return -EFAULT;
    }

    new_message->message[IPC_MAX_MESSAGE - 1] = '\0';

    mutex_lock(&ipc_mutex);
    list_add_tail(&new_message->list, &ipc_queue);
    mutex_unlock(&ipc_mutex);

    wake_up_interruptible(&ipc_wait_queue);

    pr_info("IPC: mensaje agregado a la cola\n");

    return 0;
}

SYSCALL_DEFINE1(ipc_channel_receive, char __user *, buffer)
{
    struct ipc_message *msg;
    int ret;

    ret = wait_event_interruptible(
        ipc_wait_queue,
        !list_empty(&ipc_queue)
    );

    if (ret)
        return ret;

    mutex_lock(&ipc_mutex);

    if (list_empty(&ipc_queue)) {
        mutex_unlock(&ipc_mutex);
        return -EAGAIN;
    }

    msg = list_first_entry(
        &ipc_queue,
        struct ipc_message,
        list
    );

    list_del(&msg->list);

    mutex_unlock(&ipc_mutex);

    if (copy_to_user(buffer,
                     msg->message,
                     IPC_MAX_MESSAGE)) {
        kfree(msg);
        return -EFAULT;
    }

    kfree(msg);

    pr_info("IPC: mensaje recibido desde la cola\n");

    return 0;
}
```

### Importante

El código anterior es una base de implementación. Antes de compilar se debe revisar la interfaz exacta de la práctica y adaptar los archivos al árbol del kernel 6.12.69 utilizado.

---

## 5.3 Syscall `sys_ipc_channel_send`

La función:

```c
SYSCALL_DEFINE1(ipc_channel_send, char __user *, mensaje)
```

recibe un puntero desde espacio de usuario.

La transferencia se realiza mediante:

```c
copy_from_user()
```

Después se inserta el mensaje en la cola:

```c
list_add_tail(&new_message->list, &ipc_queue);
```

El acceso está protegido:

```c
mutex_lock(&ipc_mutex);
...
mutex_unlock(&ipc_mutex);
```

Finalmente se despierta al receptor:

```c
wake_up_interruptible(&ipc_wait_queue);
```

---

## 5.4 Syscall `sys_ipc_channel_receive`

El receptor espera mediante:

```c
wait_event_interruptible(
    ipc_wait_queue,
    !list_empty(&ipc_queue)
);
```

Esto evita el *busy waiting*.

No se utiliza:

```c
while (list_empty(&ipc_queue)) {
    ...
}
```

Una vez disponible el mensaje:

```c
msg = list_first_entry(
    &ipc_queue,
    struct ipc_message,
    list
);
```

se elimina:

```c
list_del(&msg->list);
```

y se copia hacia espacio de usuario:

```c
copy_to_user()
```

---

## 5.5 Makefile

Editar:

```bash
nano kernel/Makefile
```

Agregar el objeto:

```make
obj-y += ipc_channel.o
```

Verificar:

```bash
grep -n "ipc_channel" kernel/Makefile
```

Debe aparecer:

```text
obj-y += ipc_channel.o
```

---

# 6. Implementación de log_watch

## 6.1 Diseño

La arquitectura propuesta es:

```text
Programa de usuario
       |
       v
sys_start_log_watch()
       |
       v
Copia de parámetros
       |
       v
Creación del monitor
       |
       v
kthread_run()
       |
       v
Hilo de kernel
       |
       +---- revisa archivos
       |
       +---- busca palabra clave
       |
       +---- escribe log central
       |
       v
sys_stop_log_watch()
       |
       v
kthread_stop()
```

Cada monitor posee un identificador único.

---

## 6.2 Inicio del monitor

Crear:

```bash
nano kernel/log_watch.c
```

Estructura base:

```c
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/errno.h>

#define MAX_WATCH_PATH 256
#define MAX_KEYWORD 128
#define MAX_WATCH_FILES 16

struct log_watch {
    struct list_head list;
    int id;
    struct task_struct *thread;

    int cantidad;

    char rutas[MAX_WATCH_FILES][MAX_WATCH_PATH];
    char palabra[MAX_KEYWORD];
    char log_central[MAX_WATCH_PATH];
};

static LIST_HEAD(log_watch_list);
static DEFINE_MUTEX(log_watch_mutex);
static int next_watch_id = 1;
```

---

## 6.3 Hilo de kernel

El hilo debe ejecutar periódicamente su trabajo.

La estructura general es:

```c
static int log_watch_thread(void *data)
{
    struct log_watch *watch = data;

    while (!kthread_should_stop()) {

        /*
         * Revisar los archivos indicados.
         * Buscar la palabra clave.
         * Registrar los hallazgos.
         */

        ssleep(2);
    }

    return 0;
}
```

La condición:

```c
kthread_should_stop()
```

permite que `kthread_stop()` solicite la terminación.

El hilo puede dormir:

```c
ssleep(2);
```

para realizar la revisión periódicamente sin consumir CPU continuamente.

---

## 6.4 Inicio mediante `kthread_run`

La creación del hilo se realiza conceptualmente mediante:

```c
watch->thread = kthread_run(
    log_watch_thread,
    watch,
    "log_watch_%d",
    watch->id
);
```

Se debe verificar el resultado:

```c
if (IS_ERR(watch->thread)) {
    ret = PTR_ERR(watch->thread);
    kfree(watch);
    return ret;
}
```

La lista de monitores debe protegerse:

```c
mutex_lock(&log_watch_mutex);

list_add_tail(&watch->list, &log_watch_list);

mutex_unlock(&log_watch_mutex);
```

---

## 6.5 Syscall `sys_start_log_watch`

La interfaz requerida por la práctica es:

```c
sys_start_log_watch(
    char **rutas,
    int cantidad,
    char *ruta_log_central,
    char *palabra_clave
)
```

Debe realizar, como mínimo, estas operaciones:

1. Validar `cantidad`.
2. Reservar memoria.
3. Copiar las rutas desde espacio de usuario.
4. Copiar la palabra clave.
5. Copiar la ruta del log central.
6. Generar un ID único.
7. Crear el hilo.
8. Agregar el monitor a la lista protegida.
9. Retornar el ID.

La copia desde usuario debe realizarse mediante funciones seguras como:

```c
copy_from_user()
```

---

## 6.6 Syscall `sys_stop_log_watch`

La interfaz requerida es:

```c
sys_stop_log_watch(int id_monitoreo)
```

El procedimiento general es:

```text
Recibir ID
   |
   v
Buscar monitor en la lista
   |
   +---- No existe --> error
   |
   v
Retirar/proteger estructura
   |
   v
kthread_stop()
   |
   v
Liberar memoria
   |
   v
return 0
```

La detención debe utilizar:

```c
kthread_stop(watch->thread);
```

Si el ID no existe, debe devolverse un código de error, por ejemplo:

```c
return -ENOENT;
```

---

# 7. Registro de las syscalls

## 7.1 Revisar la tabla de syscalls

En arquitectura x86_64:

```bash
cd ~/linux-6.12.69
nano arch/x86/entry/syscalls/syscall_64.tbl
```

Buscar números disponibles:

```bash
grep -nE '^[0-9]+.*syscall' arch/x86/entry/syscalls/syscall_64.tbl | tail
```

También puede revisarse el final del archivo:

```bash
tail -30 arch/x86/entry/syscalls/syscall_64.tbl
```

Se deben escoger números que no estén utilizados.

Ejemplo conceptual:

```text
XXX    common    ipc_channel_send       sys_ipc_channel_send
YYY    common    ipc_channel_receive     sys_ipc_channel_receive
ZZZ    common    start_log_watch         sys_start_log_watch
WWW    common    stop_log_watch          sys_stop_log_watch
```

**Los números `XXX`, `YYY`, `ZZZ` y `WWW` son solamente ejemplos. Se deben sustituir por números realmente disponibles en la tabla del kernel utilizado.**

---

## 7.2 Verificar los símbolos

Después de implementar las funciones:

```bash
grep -R "SYSCALL_DEFINE.*ipc_channel" kernel/
grep -R "SYSCALL_DEFINE.*log_watch" kernel/
```

También:

```bash
grep -R "sys_ipc_channel_send" .
grep -R "sys_ipc_channel_receive" .
grep -R "sys_start_log_watch" .
grep -R "sys_stop_log_watch" .
```

---

# 8. Compilación e instalación

## 8.1 Limpiar archivos temporales

Desde el directorio del kernel:

```bash
cd ~/linux-6.12.69
```

No utilizar `make clean` si existen cambios que se quieran conservar sin respaldo.

Para una recompilación normal:

```bash
make -j$(nproc)
```

Si se desea limpiar objetos generados:

```bash
make clean
```

---

## 8.2 Compilar

Primero:

```bash
make -j$(nproc)
```

Si aparecen errores, detenerse y corregirlos antes de instalar.

Verificar que se genere:

```bash
ls -lh arch/x86/boot/bzImage
```

Una compilación correcta termina con un mensaje similar a:

```text
Kernel: arch/x86/boot/bzImage is ready
```

---

## 8.3 Instalar módulos

```bash
sudo make modules_install
```

---

## 8.4 Instalar kernel

```bash
sudo make install
```

Actualizar GRUB:

```bash
sudo update-grub
```

Reiniciar:

```bash
sudo reboot
```

---

## 8.5 Verificar el kernel

Después de reiniciar:

```bash
uname -r
```

El resultado debe corresponder al kernel 6.12.69 y a la personalización utilizada.

También:

```bash
uname -a
```

Y:

```bash
cat /proc/version
```

---

# 9. Programas de usuario

Los programas deben utilizar `syscall()` y el número correspondiente registrado en `syscall_64.tbl`.

> Los números utilizados abajo son ejemplos. Sustituirlos por los números reales asignados en la tabla del kernel.

---

## 9.1 Enviar IPC

Crear:

```bash
cd ~/practica3
nano ipc_send.c
```

Código:

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_IPC_CHANNEL_SEND XXX

int main(void)
{
    char mensaje[256];

    printf("Ingrese mensaje: ");
    fflush(stdout);

    if (!fgets(mensaje, sizeof(mensaje), stdin)) {
        return 1;
    }

    long ret = syscall(SYS_IPC_CHANNEL_SEND, mensaje);

    if (ret < 0) {
        perror("sys_ipc_channel_send");
        return 1;
    }

    printf("Mensaje enviado correctamente.\n");

    return 0;
}
```

Compilar:

```bash
gcc -Wall -Wextra ipc_send.c -o ipc_send
```

Ejecutar:

```bash
./ipc_send
```

---

## 9.2 Recibir IPC

Crear:

```bash
nano ipc_receive.c
```

Código:

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_IPC_CHANNEL_RECEIVE YYY

int main(void)
{
    char buffer[256] = {0};

    printf("Esperando mensaje...\n");
    fflush(stdout);

    long ret = syscall(
        SYS_IPC_CHANNEL_RECEIVE,
        buffer
    );

    if (ret < 0) {
        perror("sys_ipc_channel_receive");
        return 1;
    }

    printf("Mensaje recibido: %s\n", buffer);

    return 0;
}
```

Compilar:

```bash
gcc -Wall -Wextra ipc_receive.c -o ipc_receive
```

Ejecutar:

```bash
./ipc_receive
```

Debe quedar bloqueado:

```text
Esperando mensaje...
```

Mientras permanece bloqueado se puede verificar que no esté consumiendo CPU de forma innecesaria.

En otra terminal:

```bash
./ipc_send
```

Al enviar un mensaje, el receptor debe continuar y mostrarlo.

---

## 9.3 Iniciar log_watch

Crear:

```bash
nano start_log_watch.c
```

La llamada debe seguir la firma requerida:

```c
syscall(
    SYS_START_LOG_WATCH,
    rutas,
    cantidad,
    log_central,
    palabra_clave
);
```

Ejemplo de estructura:

```c
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_START_LOG_WATCH ZZZ

int main(void)
{
    char *rutas[] = {
        "/tmp/practica3_log1.log",
        "/tmp/practica3_log2.log"
    };

    int cantidad = 2;

    char log_central[] =
        "/tmp/practica3_central.log";

    char palabra_clave[] =
        "ERROR";

    long ret = syscall(
        SYS_START_LOG_WATCH,
        rutas,
        cantidad,
        log_central,
        palabra_clave
    );

    if (ret < 0) {
        perror("sys_start_log_watch");
        return 1;
    }

    printf("Monitor iniciado correctamente.\n");
    printf("ID del monitor: %ld\n", ret);

    return 0;
}
```

Compilar:

```bash
gcc -Wall -Wextra start_log_watch.c -o start_log_watch
```

---

## 9.4 Detener log_watch

Crear:

```bash
nano stop_log_watch.c
```

Código:

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_STOP_LOG_WATCH WWW

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <id_monitor>\n", argv[0]);
        return 1;
    }

    int id = atoi(argv[1]);

    long ret = syscall(
        SYS_STOP_LOG_WATCH,
        id
    );

    if (ret < 0) {
        perror("sys_stop_log_watch");
        return 1;
    }

    printf("Monitor %d detenido correctamente.\n", id);

    return 0;
}
```

Compilar:

```bash
gcc -Wall -Wextra stop_log_watch.c -o stop_log_watch
```

Ejecutar:

```bash
./stop_log_watch ID
```

---

# 10. Pruebas

## 10.1 Verificación del kernel

Ejecutar:

```bash
uname -r
```

Evidencia textual:

```text
[PEGAR AQUÍ LA SALIDA REAL DE uname -r]
```

---

## 10.2 Prueba de bloqueo IPC

Terminal 1:

```bash
./ipc_receive
```

Salida esperada:

```text
Esperando mensaje...
```

El programa debe permanecer esperando.

Terminal 2:

```bash
./ipc_send
```

Ejemplo:

```text
Ingrese mensaje: Hola desde el proceso emisor
Mensaje enviado correctamente.
```

La terminal 1 debe mostrar:

```text
Mensaje recibido: Hola desde el proceso emisor
```

### Verificación con `ps`

Mientras el receptor espera:

```bash
ps aux | grep ipc_receive
```

También se puede utilizar:

```bash
top
```

para observar que el receptor no está ejecutando un ciclo de espera activa.

---

## 10.3 Preparar archivos de prueba

Crear:

```bash
echo "Inicio del archivo 1" > /tmp/practica3_log1.log
echo "Inicio del archivo 2" > /tmp/practica3_log2.log
```

Verificar:

```bash
cat /tmp/practica3_log1.log
cat /tmp/practica3_log2.log
```

Crear el log central:

```bash
touch /tmp/practica3_central.log
```

Limpiarlo antes de una prueba:

```bash
truncate -s 0 /tmp/practica3_central.log
```

---

# 10.4 Prueba 1 de log_watch

Iniciar:

```bash
./start_log_watch
```

Guardar el ID que devuelve.

Ejemplo:

```text
Monitor iniciado correctamente.
ID del monitor: 1
```

Agregar una línea con la palabra clave:

```bash
echo "ERROR prueba 1: evento detectado" >> /tmp/practica3_log1.log
```

Esperar unos segundos:

```bash
sleep 3
```

Consultar el log central:

```bash
cat /tmp/practica3_central.log
```

Evidencia:

```text
[PEGAR AQUÍ LA SALIDA REAL]
```

---

# 10.5 Prueba 2 de log_watch

Agregar:

```bash
echo "ERROR prueba 2: segundo evento detectado" >> /tmp/practica3_log2.log
```

Esperar:

```bash
sleep 3
```

Consultar:

```bash
cat /tmp/practica3_central.log
```

Evidencia:

```text
[PEGAR AQUÍ LA SALIDA REAL]
```

---

# 10.6 Prueba 3 de log_watch

Agregar:

```bash
echo "ERROR prueba 3: tercer evento detectado" >> /tmp/practica3_log1.log
```

Esperar:

```bash
sleep 3
```

Consultar:

```bash
cat /tmp/practica3_central.log
```

Evidencia:

```text
[PEGAR AQUÍ LA SALIDA REAL]
```

La práctica solicita demostrar al menos tres pruebas consecutivas de detección.

---

# 10.7 Detener el monitor

Si el programa devolvió:

```text
ID del monitor: 1
```

ejecutar:

```bash
./stop_log_watch 1
```

Salida esperada:

```text
Monitor 1 detenido correctamente.
```

---

## 10.8 Probar un ID inexistente

Por ejemplo:

```bash
./stop_log_watch 9999
```

Debe producir un error porque el ID no existe.

Evidencia:

```text
[PEGAR AQUÍ LA SALIDA REAL]
```

---

# 10.9 Revisar mensajes del kernel

Se puede utilizar:

```bash
sudo dmesg | tail -50
```

Buscar específicamente:

```bash
sudo dmesg | grep -i IPC
```

Y:

```bash
sudo dmesg | grep -i watch
```

Estas evidencias pueden utilizarse para demostrar que las syscalls fueron ejecutadas dentro del kernel.

---

# 11. Verificación final

Antes de entregar se recomienda ejecutar:

```bash
cd ~/linux-6.12.69
make kernelversion
```

Debe ser:

```text
6.12.69
```

Verificar el kernel actualmente ejecutándose:

```bash
uname -r
```

Verificar los programas:

```bash
cd ~/practica3
ls -lh
```

Se deberían encontrar los ejecutables:

```text
ipc_send
ipc_receive
start_log_watch
stop_log_watch
```

Verificar el repositorio:

```bash
cd ~/linux-6.12.69
git status
```

---

# 12. Repositorio GitLab

Configurar Git:

```bash
git config --global user.name "Raúl Emanuel Yat Cancinos"
git config --global user.email "TU_CORREO"
```

Inicializar si todavía no existe:

```bash
cd ~/linux-6.12.69
git status
```

Agregar cambios:

```bash
git add kernel/ipc_channel.c
git add kernel/log_watch.c
git add kernel/Makefile
git add arch/x86/entry/syscalls/syscall_64.tbl
```

Agregar programas de usuario desde su repositorio correspondiente:

```bash
git add .
```

Crear commit:

```bash
git commit -m "Implementación práctica 3 IPC y log watch"
```

Crear la rama de trabajo:

```bash
git checkout -b practica3
```

Verificar:

```bash
git branch
```

Agregar el repositorio remoto:

```bash
git remote add origin URL_DEL_REPOSITORIO
```

Enviar la rama:

```bash
git push -u origin practica3
```

Finalmente verificar:

```bash
git remote -v
git status
```

En este documento debe colocarse el enlace real:

```text
Repositorio GitLab:
[PEGAR AQUÍ EL LINK REAL DEL REPOSITORIO]
```

---

# 13. Conclusiones

La práctica permitió estudiar la implementación de mecanismos de sincronización y comunicación directamente dentro del kernel de Linux.

La utilización de una cola FIFO junto con un `mutex` permite proteger el acceso concurrente a los mensajes. Por otro lado, la utilización de una *wait queue* permite que el proceso receptor permanezca bloqueado hasta que exista información disponible, evitando un ciclo de espera activa.

El mecanismo de `log_watch` permite estudiar el ciclo de vida de un hilo de kernel: creación, ejecución periódica y terminación mediante `kthread_stop()`.

Finalmente, las pruebas desde espacio de usuario permiten comprobar la interacción entre programas convencionales y las nuevas llamadas al sistema implementadas en el kernel.

---

# 14. Lecciones aprendidas

- Diferencia entre espacio de usuario y espacio de kernel.
- Funcionamiento de las llamadas al sistema.
- Uso de `copy_from_user()` y `copy_to_user()`.
- Protección de estructuras compartidas mediante `mutex`.
- Funcionamiento de las *wait queues*.
- Diferencia entre bloqueo y *polling*.
- Creación de hilos mediante `kthread_run()`.
- Terminación segura mediante `kthread_stop()`.
- Manejo de listas enlazadas del kernel.
- Compilación e instalación de un kernel personalizado.
- Pruebas de funcionalidades del kernel desde programas escritos en C.
- Uso de `dmesg`, `ps`, `top` y herramientas de depuración para validar el comportamiento.

---

# 15. Referencias

1. Love, R., & Bovet, D. (2005). *Understanding the Linux Kernel*. O'Reilly Media.
2. Kerrisk, M. (2010). *The Linux Programming Interface*. No Starch Press.
3. Linux Kernel Documentation: https://www.kernel.org/doc/html/latest/
4. GNU C Library / GCC Documentation: https://gcc.gnu.org/onlinedocs/

---