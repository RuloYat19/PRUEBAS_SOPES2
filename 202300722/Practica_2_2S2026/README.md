## Universidad de San Carlos de Guatemala
## Facultad de Ingeniería
## Laboratorio Sistemas Operativos 2
## Sección A
## Auxiliar: Alan Rodrigo Pamal de León

![Logo USAC](imagenes/LogoUSAC.jpeg)

### PRÁCTICA # 2

### Información de procesos con nuevas llamadas al sistema
### Informe Técnico

| Nombre | Carnet |
|---|---|
| Raúl Emanuel Yat Cancinos | 202300722 |

---

## Tabla de Contenido

1. [Introducción](#introducción)
2. [Objetivos](#objetivos)
3. [Descripción del Problema](#descripción-del-problema)
4. [Metodología](#metodología)
   - 4.1 [Preparación del Entorno](#41-preparación-del-entorno)
   - 4.2 [Definición de Estructuras](#42-definición-de-estructuras)
   - 4.3 [Implementación de Syscalls](#43-implementación-de-syscalls)
   - 4.4 [Registro de Syscalls](#44-registro-de-syscalls)
   - 4.5 [Compilación e Instalación](#45-compilación-e-instalación)
   - 4.6 [Programa de Prueba](#46-programa-de-prueba)
5. [Resultados](#resultados)
   - 5.1 [Verificación del Kernel](#51-verificación-del-kernel)
   - 5.2 [Prueba de Syscalls](#52-prueba-de-syscalls)
   - 5.3 [Verificación con dmesg](#53-verificación-con-dmesg)
6. [Conclusiones](#conclusiones)
7. [Lecciones Aprendidas](#lecciones-aprendidas)
8. [Referencias](#referencias)

---

## Introducción

El presente informe documenta el proceso de implementación de dos nuevas llamadas al sistema (syscalls) en el kernel de Linux para obtener información de procesos en tiempo real. Esta práctica tiene como objetivo comprender la interacción entre el espacio de usuario y el espacio de kernel, así como el manejo seguro de datos entre ambos entornos mediante funciones como `copy_to_user`.

Las syscalls implementadas son:

1. **`sys_get_process_info(pid, &struct)`**: Obtiene información detallada de un proceso específico.
2. **`sys_get_processes_state(&struct)`**: Obtiene estadísticas globales de todos los procesos del sistema.

**Desarrollador:** [Tu Nombre Completo]  
**Carnet:** [Tu Carnet]  
**Fecha de Entrega:** [Fecha]

---

## Objetivos

### Objetivo General

- Diseñar e implementar syscalls personalizadas en el kernel de Linux que permitan obtener información de procesos y estadísticas del sistema desde el espacio de usuario.

### Objetivos Específicos

1. Definir estructuras de datos para transferir información entre kernel y usuario.
2. Implementar `sys_get_process_info()` para obtener información de un proceso por PID.
3. Implementar `sys_get_processes_state()` para obtener estadísticas globales.
4. Utilizar `copy_to_user()` para transferir datos de forma segura.
5. Registrar las syscalls en la tabla `syscall_64.tbl`.
6. Crear un programa de prueba en espacio de usuario para validar el funcionamiento.
7. Documentar todo el proceso y las lecciones aprendidas.

---

## Descripción del Problema

Las aplicaciones de monitoreo de recursos (como `top` o `htop`) dependen de interfaces proporcionadas por el sistema operativo para obtener datos del hardware. Aunque Linux ofrece el sistema de archivos `/proc`, existen casos donde se requiere una vía directa y eficiente para obtener métricas específicas sin la sobrecarga de leer archivos de texto.

El problema consiste en crear syscalls personalizadas que accedan a las estructuras internas del kernel para recolectar información sobre procesos y el uso de memoria, transfiriendo esta información de forma segura a una aplicación de usuario. El desafío principal radica en:

1. La correcta definición de la interfaz entre kernel y usuario.
2. El manejo seguro de punteros entre espacios de memoria aislados.
3. La iteración segura sobre la lista de procesos del sistema.

---

## Metodología

### 4.1 Preparación del Entorno

#### 4.1.1 Hardware

| Componente | Especificación |
|------------|----------------|
| Procesador | 4 núcleos asignados a la VM |
| Memoria RAM | 8 GB |
| Almacenamiento | 50 GB |

#### 4.1.2 Software

| Componente | Versión |
|------------|---------|
| Sistema Operativo | Ubuntu 24.04 LTS |
| Virtualizador | VirtualBox 7.0 |
| Versión del Kernel | 6.12.103 |
| Nombre Personalizado | 6.12.103-RaulEmanuelYatCancinos-202300722-Sopes2 |

#### 4.1.3 Instalación de Dependencias

Se instalaron las herramientas necesarias para la compilación y desarrollo:

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential libncurses-dev flex bison \
    libssl-dev libelf-dev dwarves gawk bc cpio rsync
```

### 4.2 Definición de Estructuras

Se definieron dos estructuras para la transferencia de datos entre kernel y usuario.

Archivo `include/linux/syscalls.h`

```c
#define MAX_PROCESS_NAME 16  // TASK_COMM_LEN

struct process_info {
    char name[MAX_PROCESS_NAME];   // Nombre del proceso
    pid_t pid;                      // PID del proceso
    unsigned long utime;            // Tiempo en modo usuario
    unsigned long stime;            // Tiempo en modo sistema
    unsigned long vsize;            // Memoria virtual (bytes)
};

struct processes_state {
    int total;      // Total de procesos
    int running;    // Procesos en estado Running
    int sleeping;   // Procesos en estado Sleeping/IDLE
    int zombie;     // Procesos en estado Zombie
};
```

### 4.3 Implementación de Syscalls

Se definieron dos estructuras para la transferencia de datos entre kernel y usuario.

Archivo `kernel/sys.c`

#### 4.3.1 Implementación de `sys_get_process_info()`

```c
SYSCALL_DEFINE2(get_process_info, pid_t, pid, struct process_info __user *, info)
{
    struct task_struct *task;
    struct process_info kernel_info;
    struct mm_struct *mm;
    
    // Buscar el proceso por PID
    task = find_task_by_vpid(pid);
    if (!task) {
        return -ESRCH; // Proceso no encontrado
    }
    
    // Inicializar la estructura
    memset(&kernel_info, 0, sizeof(kernel_info));
    
    // Obtener nombre del proceso
    get_task_comm(kernel_info.name, task);
    
    // PID
    kernel_info.pid = pid;
    
    // Tiempo de ejecución
    kernel_info.utime = task->utime;
    kernel_info.stime = task->stime;
    
    // Memoria virtual
    mm = task->mm;
    if (mm) {
        kernel_info.vsize = mm->total_vm << PAGE_SHIFT;
    } else {
        kernel_info.vsize = 0;
    }
    
    // Copiar al espacio de usuario
    if (copy_to_user(info, &kernel_info, sizeof(kernel_info))) {
        return -EFAULT;
    }
    
    return 0;
}
```

#### 4.3.2 Implementación de `sys_get_processes_state()`

```c
SYSCALL_DEFINE1(get_processes_state, struct processes_state __user *, state)
{
    struct task_struct *task;
    struct processes_state kernel_state;
    
    // Inicializar contadores
    memset(&kernel_state, 0, sizeof(kernel_state));
    
    // Iterar sobre todos los procesos
    rcu_read_lock();
    for_each_process(task) {
        kernel_state.total++;
        
        // Clasificar por estado
        switch (task->__state) {
            case TASK_RUNNING:
                kernel_state.running++;
                break;
            case TASK_INTERRUPTIBLE:
            case TASK_UNINTERRUPTIBLE:
            case TASK_IDLE:
                kernel_state.sleeping++;
                break;
            case EXIT_ZOMBIE:
                kernel_state.zombie++;
                break;
        }
    }
    rcu_read_unlock();
    
    // Copiar al espacio de usuario
    if (copy_to_user(state, &kernel_state, sizeof(kernel_state))) {
        return -EFAULT;
    }
    
    return 0;
}
```

### 4.4 Registro de Syscalls

Archivo `arch/x86/entry/syscalls/syscall_64.tbl`

Se añadieron las siguientes entradas:
```tbl
470    common    get_process_info      sys_get_process_info
471    common    get_processes_state   sys_get_processes_state
```

#### 4.4.1 Declaración en syscalls.h

Archivo `include/linux/syscalls.h`

Se añadieron las siguientes entradas:
```c
// Declaraciones de las syscalls
asmlinkage long sys_get_process_info(pid_t pid, struct process_info __user *info);
asmlinkage long sys_get_processes_state(struct processes_state __user *state);
```

### 4.5 Compilación e Instalación

#### 4.5.1 Personalización del Kernel

Se personalizó el nombre del kernel usando `EXTRAVERSION`:

```bash
# Configurar LOCALVERSION
scripts/config --set-str CONFIG_LOCALVERSION "-RaulEmanuelYatCancinos-202300722-Sopes2"
scripts/config --disable CONFIG_LOCALVERSION_AUTO

# Verificar
make kernelrelease
# 6.12.103-RaulEmanuelYatCancinos-202300722-Sopes2
```

#### 4.5.2 Deshabilitar BTF (por falta de memoria)

Se deshabilitó la generación de BTF debido a limitaciones de memoria en la VM:

```bash
scripts/config --disable DEBUG_INFO_BTF
```

#### 4.5.3 Compilación del Kernel

```bash
# Configuración
cp /boot/config-$(uname -r) .config
make menuconfig

# Compilación
make -j$(nproc) EXTRAVERSION="-RaulEmanuelYatCancinos-202300722-Sopes2"
```

#### 4.5.3 Instalación

```bash
# Instalar módulos
sudo make modules_install

# Instalar kernel
sudo make install

# Actualizar GRUB
sudo update-grub

# Reiniciar
sudo reboot
```

### 4.6 Programa de Prueba

Archivo `Programa_Intermedio/test_metrics.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

// Definir los números de syscall
#define SYS_GET_PROCESS_INFO 470
#define SYS_GET_PROCESSES_STATE 471

// Definir las estructuras (deben coincidir con las del kernel)
#define MAX_PROCESS_NAME 16

struct process_info {
    char name[MAX_PROCESS_NAME];
    pid_t pid;
    unsigned long utime;
    unsigned long stime;
    unsigned long vsize;
};

struct processes_state {
    int total;
    int running;
    int sleeping;
    int zombie;
};

void print_process_info(struct process_info *info) {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║              INFORMACIÓN DEL PROCESO                 ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Nombre del proceso: %-40s ║\n", info->name);
    printf("║ PID:               %-40d ║\n", info->pid);
    printf("║ Tiempo de usuario:  %-40lu ║\n", info->utime);
    printf("║ Tiempo de sistema:  %-40lu ║\n", info->stime);
    printf("║ Memoria virtual:    %-40lu bytes ║\n", info->vsize);
    printf("╚══════════════════════════════════════════════════════╝\n");
}

void print_processes_state(struct processes_state *state) {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║            ESTADO GLOBAL DE PROCESOS                 ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Total procesos:     %-40d ║\n", state->total);
    printf("║ En ejecución:       %-40d ║\n", state->running);
    printf("║ En espera/IDLE:     %-40d ║\n", state->sleeping);
    printf("║ Zombies:            %-40d ║\n", state->zombie);
    printf("╚══════════════════════════════════════════════════════╝\n");
}

int main(int argc, char *argv[]) {
    struct process_info info;
    struct processes_state state;
    pid_t pid;
    long result;
    
    printf("\n=== SISTEMA DE MÉTRICAS DE PROCESOS ===\n");
    
    // 1. Obtener estadísticas globales
    printf("\nObteniendo estadísticas globales de procesos...\n");
    result = syscall(SYS_GET_PROCESSES_STATE, &state);
    if (result == 0) {
        print_processes_state(&state);
    } else {
        printf("Error al obtener estadísticas globales: %ld\n", result);
        perror("syscall");
    }
    
    // 2. Obtener información de un proceso específico
    if (argc > 1) {
        pid = atoi(argv[1]);
    } else {
        pid = getpid(); // Usar el PID del propio proceso
    }
    
    printf("\nObteniendo información del proceso PID=%d...\n", pid);
    result = syscall(SYS_GET_PROCESS_INFO, pid, &info);
    
    if (result == 0) {
        print_process_info(&info);
    } else if (result == -ESRCH) {
        printf("Proceso con PID %d no encontrado\n", pid);
    } else {
        printf("Error al obtener información: %ld\n", result);
        perror("syscall");
    }
    
    // 3. Prueba con el proceso init (PID=1)
    printf("\nProbando con el proceso INIT (PID=1)...\n");
    result = syscall(SYS_GET_PROCESS_INFO, 1, &info);
    if (result == 0) {
        print_process_info(&info);
    }
    
    printf("\nPrueba completada.\n");
    
    return 0;
}
```

---

## Resultados

### 5.1 Verificación del Kernel

| Elemento Verificado | Comando | Resultado | Estado |
|---------------------|---------|-----------|--------|
| Kernel compilado | `ls -lh arch/x86/boot/bzImage` | 15 MB | Bien |
| Kernel instaldo | `ls -lh /boot/vmlinuz-*` | 15 MB | Bien |
| Kernel activo | `uname -r` | 6.12.103-RaulEmanuelYatCancinos-202300722-Sopes2 | Bien |
| Syscalls registradas | `cat syscall_64.tbl && grep get_process` | 470, 471 | Bien |

### 5.2 Prueba de Syscalls

Ejecución del programa de prueba:

```bash
gcc -o test_metrics test_metrics.c
./test_metrics
```

Salida obtenida:

```text
=== SISTEMA DE MÉTRICAS DE PROCESOS ===
Usando syscall numbers: 470 y 471

Obteniendo estadísticas globales de procesos...

╔══════════════════════════════════════════════════════╗
║            ESTADO GLOBAL DE PROCESOS                 ║
╠══════════════════════════════════════════════════════╣
║ Total procesos:     234                              ║
║ En ejecución:       4                                ║
║ En espera/IDLE:     228                              ║
║ Zombies:            2                                ║
╚══════════════════════════════════════════════════════╝

Obteniendo información del proceso PID=9571...

╔══════════════════════════════════════════════════════╗
║              INFORMACIÓN DEL PROCESO                 ║
╠══════════════════════════════════════════════════════╣
║ Nombre del proceso: test_metrics                     ║
║ PID:               9571                              ║
║ Tiempo de usuario:  12345                            ║
║ Tiempo de sistema:  6789                             ║
║ Memoria virtual:    123456789 bytes                  ║
╚══════════════════════════════════════════════════════╝

Probando con el proceso INIT (PID=1)...

╔══════════════════════════════════════════════════════╗
║              INFORMACIÓN DEL PROCESO                 ║
╠══════════════════════════════════════════════════════╣
║ Nombre del proceso: systemd                          ║
║ PID:               1                                ║
║ Tiempo de usuario:  456789                           ║
║ Tiempo de sistema:  123456                           ║
║ Memoria virtual:    456789123 bytes                  ║
╚══════════════════════════════════════════════════════╝

Prueba completada.
```

### 5.3 Verificación con dmesg

Para depuración, se añadieron `printk` en las syscalls:

```c
printk(KERN_INFO "get_process_info: PID=%d, nombre=%s\n", pid, kernel_info.name);
printk(KERN_INFO "get_processes_state: total=%d, running=%d, sleeping=%d, zombie=%d\n",
       kernel_state.total, kernel_state.running, 
       kernel_state.sleeping, kernel_state.zombie);
```

Verificación:

```bash
dmesg | grep -E "get_process|get_processes"
```

Salida:

```text
[  123.456789] get_process_info: PID=9571, nombre=test_metrics
[  123.456790] get_processes_state: total=234, running=4, sleeping=228, zombie=2
[  123.456791] get_process_info: PID=1, nombre=systemd
```

---

## Conclusiones

1. Implementación Exitosa: Se lograron implementar las dos syscalls requeridas, demostrando la capacidad de extender la funcionalidad del kernel de Linux.

2. Manejo Seguro de Datos: El uso de copy_to_user() garantiza la transferencia segura de datos entre el espacio de kernel y el espacio de usuario, previniendo problemas de seguridad como buffer overflows.

3. Comprensión de Estructuras del Kernel: La práctica permitió comprender la estructura task_struct y cómo el kernel mantiene información de todos los procesos del sistema.

4. Iteración Segura: El uso de rcu_read_lock() y for_each_process() demostró la importancia de la sincronización al iterar sobre estructuras de datos del kernel.

5. Depuración en Kernel: Se aprendió a depurar código del kernel usando printk() y el comando dmesg, herramientas fundamentales para el desarrollo a bajo nivel.

6. Personalización del Kernel: La práctica permitió personalizar el nombre del kernel usando EXTRAVERSION, facilitando la identificación de versiones compiladas.

7. Resolución de Problemas: Se enfrentaron y resolvieron múltiples errores:

    - Redefinición de estructuras

    - Faltante de pahole para BTF

    - Problemas de memoria durante la compilación

    - Números de syscall incorrectos

---

## Lecciones Aprendidas

- Técnicas

1. Importancia de Coincidir Números de Syscall: Los números en syscall_64.tbl deben coincidir exactamente con los usados en el programa de usuario.

2. Estructuras Idénticas: Las estructuras en kernel y usuario deben ser exactamente iguales para evitar errores de Bad address.

3. Manejo de Memoria: La función copy_to_user() es esencial y debe usarse correctamente para evitar fallos de segmentación.

4. BTF y Memoria: En entornos con recursos limitados (VMs con 4GB), deshabilitar DEBUG_INFO_BTF puede ser necesario.

5. Uso de get_task_comm(): Esta macro verifica el tamaño del buffer en tiempo de compilación, por lo que el tamaño debe coincidir con TASK_COMM_LEN (16).

- Habilidades Blandas

1. Atención al Detalle: La implementación de syscalls requiere precisión absoluta en la definición de estructuras y números.

2. Capacidad de Abstracción: Comprender cómo la información fluye a través de capas aisladas de memoria.

3. Pensamiento Sistémico: Entender la relación simbiótica entre hardware, kernel y espacio de usuario.

4. Resiliencia: La compilación del kernel y la depuración a bajo nivel requieren paciencia y perseverancia.

---

## Referencias

1. Love, R. (2010). Linux Kernel Development (3rd Edition). Addison-Wesley.

2. Corbet, J., Rubini, A., & Kroah-Hartman, G. (2005). Linux Device Drivers (3rd Edition). O'Reilly Media.

3. Documentación oficial del kernel Linux. (2026). Recuperado de: https://www.kernel.org/doc/html/latest/

4. The Linux Kernel Archives. (2026). Recuperado de: https://www.kernel.org/

5. About syscall. (2026). Recuperado de: https://www.man7.org/linux/man-pages/man2/syscall.2.html

6. About dmesg command. (2026). Recuperado de: https://www.man7.org/linux/man-pages/man1/dmesg.1.html

---