# Guía de Instalación y Despliegue en una Máquina Nueva (Desde Cero)

Esta guía detalla el procedimiento completo paso a paso para preparar, compilar y ejecutar el **Proyecto Único de Sistemas Operativos 2 (2S2026)** en una máquina limpia (máquina virtual o física) desde cero.

---

## 1. Requisitos Previos del Sistema

### Hardware Recomendado
- **Arquitectura:** `x86_64` (Intel o AMD de 64 bits).
- **Procesador:** Mínimo 2 núcleos (recomendado 4 o más para agilizar la compilación del kernel).
- **Memoria RAM:** Mínimo 4 GB (recomendado 8 GB).
- **Almacenamiento:** Mínimo **25 GB libres** en la partición raíz o en el directorio de compilación.

### Sistema Operativo Base Recomendado
- **Ubuntu 22.04 LTS** o **Ubuntu 24.04 LTS** (instalación limpia de servidor o escritorio).
- También compatible con Debian 12 (Bookworm).

---

## 2. Instalación de Paquetes y Herramientas del Sistema

Abre una terminal en la máquina nueva y actualiza los repositorios de paquetes. Luego instala todas las dependencias requeridas para compilar el kernel Linux, compilar programas eBPF con Clang/LLVM y compilar el daemon con soporte PAM y OpenSSL:

```bash
sudo apt-get update && sudo apt-get upgrade -y

sudo apt-get install -y \
    build-essential \
    flex \
    bison \
    libncurses-dev \
    libssl-dev \
    libelf-dev \
    bc \
    zstd \
    cpio \
    kmod \
    rsync \
    gawk \
    wget \
    curl \
    git \
    python3 \
    pciutils \
    dwarves \
    clang \
    llvm \
    libbpf-dev \
    linux-tools-common \
    bpftool \
    libpam0g-dev
```

### Verificación de herramientas críticas
Asegúrate de que las siguientes herramientas clave estén disponibles:

```bash
# 1. Comprobar versión de pahole (debe ser >= 1.16 para generar información BTF)
pahole --version

# 2. Comprobar Clang y bpftool (necesarios para el sensor eBPF)
clang --version
bpftool version
```

---

## 3. Descarga / Transferencia del Repositorio

Clona o copia el repositorio del proyecto en tu máquina nueva (por ejemplo, en `~/Documentos/mapa`):

```bash
mkdir -p ~/Documentos
cd ~/Documentos
git clone <URL_DEL_REPOSITORIO> mapa

cd ~/Documentos/mapa/ProyectoUnico_2S2026
```

> **Estructura esperada:**
> El directorio `ProyectoUnico_2S2026/` contiene las carpetas `Kernel/`, `ProgramaIntermedio/`, `Dashboard/`, `scripts/`, `docs/`, `imgs/` y `archivos_prueba/`.

---

## 4. Compilación e Instalación del Kernel Personalizado

El proyecto utiliza el kernel oficial **Linux 6.12.69 LTS** integrado con las **6 llamadas al sistema personalizadas** (Syscalls 548 a 553: `ipc_channel_send`, `ipc_channel_receive`, `start_log_watch`, `stop_log_watch`, `get_process_info`, `get_processes_state`) y con soporte completo para **eBPF y BTF**.

### Paso 4.1: Definir la carpeta de trabajo para compilar el Kernel
Para evitar problemas de espacio o rutas relativas en la máquina nueva, exporta la variable `SO2_WORKSPACE`:

```bash
export SO2_WORKSPACE=~/so2_build_workspace
mkdir -p "$SO2_WORKSPACE"
```

### Paso 4.2: Descarga, parcheo y compilación automática
Ejecuta el script maestro de preparación:

```bash
cd ~/Documentos/mapa/ProyectoUnico_2S2026
scripts/preparar_kernel.sh --sin-instalar
```

**¿Qué hace este script automáticamente?**
1. Descarga el archivo oficial `linux-6.12.69.tar.xz` desde `cdn.kernel.org` y lo descomprime.
2. Aplica el overlay desde `Kernel/` hacia el árbol de fuentes, agregando las 6 syscalls en `arch/x86/entry/syscalls/syscall_64.tbl`, `include/linux/syscalls.h`, `include/linux/ipc_channel.h` y `kernel/Makefile`.
3. Copia la configuración del hardware local (`/boot/config-$(uname -r)`) y activa los flags requeridos:
   - `CONFIG_BPF=y`
   - `CONFIG_BPF_SYSCALL=y`
   - `CONFIG_BPF_EVENTS=y`
   - `CONFIG_DEBUG_INFO_BTF=y`
   - `CONFIG_FTRACE_SYSCALLS=y`
   - `CONFIG_LOCALVERSION="-so2-202100106"`
4. Compila el kernel y todos sus módulos aprovechando todos los núcleos disponibles (`make -j$(nproc)`).
   *(Este proceso puede tomar entre 15 y 45 minutos dependiendo de la potencia del procesador).*

---

### Paso 4.3: Instalación del Kernel y actualización de GRUB
Una vez terminada la compilación exitosamente, instala los módulos e imagen de arranque:

```bash
cd ~/Documentos/mapa/ProyectoUnico_2S2026
scripts/instalar_kernel.sh
```

**¿Qué hace este script?**
- Instala los módulos en `/lib/modules/6.12.69-so2-202100106`.
- Copia `vmlinuz` y `System.map` a `/boot/`.
- Genera el archivo `initrd.img` correspondiente.
- Si Secure Boot está activo, firma la imagen con clave MOK.
- Ejecuta `update-grub` para agregar el nuevo kernel al menú de inicio.

---

### Paso 4.4: Reiniciar la máquina
Reinicia el sistema operativo para arrancar con el nuevo kernel:

```bash
sudo reboot
```

> [!IMPORTANT]
> Si la máquina no arranca automáticamente con el kernel nuevo, en la pantalla de GRUB selecciona:
> **"Advanced options for Ubuntu"** (Opciones avanzadas) → **"Ubuntu, with Linux 6.12.69-so2-202100106"**.

---

### Paso 4.5: Verificación post-reinicio
Tras iniciar sesión nuevamente, valida que el kernel activo sea el correcto y que las syscalls funcionen:

```bash
# Debe imprimir exactamente: 6.12.69-so2-202100106
uname -r

# Ejecutar el script integral de verificación del kernel:
cd ~/Documentos/mapa/ProyectoUnico_2S2026
sudo scripts/verificar_kernel.sh
```

El script debe mostrar en color verde:
- `✓ Version de kernel: 6.12.69-so2-202100106`
- `✓ BTF vmlinux: /sys/kernel/btf/vmlinux existe y es legible`
- `✓ Configuracion eBPF/BTF en /boot/config-...: todas las opciones =y`
- `✓ Syscall 548-553 validadas exitosamente`

---

## 5. Configuración de Usuarios y Autenticación PAM

El daemon delega la autenticación y permisos en el subsistema PAM de Linux y grupos locales del sistema.

### Paso 5.1: Configurar grupos y servicio PAM
Ejecuta el script de configuración base como superusuario:

```bash
cd ~/Documentos/mapa/ProyectoUnico_2S2026
sudo scripts/setup_usuarios.sh
```
Esto crea los grupos `admin_user`, `common_user` y el servicio `/etc/pam.d/so2-monitor`.

### Paso 5.2: Crear los usuarios de demostración
Crea los usuarios que utilizarás para la presentación:

```bash
# 1. Crear usuario Administrador (ana):
sudo scripts/setup_usuarios.sh agregar ana admin
# (Ingresa la contraseña cuando el sistema la solicite, por ejemplo: 4493)

# 2. Crear usuario Común (luis):
sudo scripts/setup_usuarios.sh agregar luis comun
# (Ingresa la contraseña cuando el sistema la solicite, por ejemplo: 4493)
```

---

## 6. Compilación del Daemon en C y Sensor eBPF

Ahora compila el ejecutable `daemon_monitor` y ejecuta sus pruebas unitarias:

```bash
cd ~/Documentos/mapa/ProyectoUnico_2S2026/ProgramaIntermedio
make clean all pruebas
```

**¿Qué ocurre en este paso?**
1. Se extrae `vmlinux.h` desde `/sys/kernel/btf/vmlinux` mediante `bpftool`.
2. Se compila `ebpf/sensor.bpf.c` con Clang en bytecode BPF (`sensor.bpf.o`).
3. Se genera el esqueleto en C `ebpf/sensor.skel.h`.
4. Se compila el binario `daemon_monitor` vinculando `libpam`, `libbpf` y `libcrypto` (OpenSSL).
5. Se ejecutan automáticamente las 10 baterías de pruebas unitarias (`test_config`, `test_peticion`, `test_auth`, `test_alertas`, `test_metricas`, `test_procesos`, `test_analisis`, `test_blacklist`, `test_logwatch`).

> Todo debe compilar limpiamente con **0 advertencias** y mostrar `Todas las pruebas pasaron exitosamente`.

---

## 7. Preparación de Directorios de Demostración

Crea los directorios de trabajo estándar en el home del usuario:

```bash
mkdir -p ~/so2_logs ~/so2_vigilado
touch ~/so2_logs/app.log ~/so2_logs/auth.log
```

---

## 8. Verificación Integral con la Suite de Pruebas

Antes de la presentación oficial, ejecuta la suite completa de extremo a extremo para validar que los 9 componentes del proyecto operan al 100%:

```bash
cd ~/Documentos/mapa/ProyectoUnico_2S2026
sudo scripts/correr_pruebas.sh
```

**Resultado esperado:**
```text
====================================================================
                    RESUMEN GLOBAL DE PRUEBAS                       
====================================================================
  ✓ [PASÓ]  Compilación y pruebas unitarias en C
  ✓ [PASÓ]  Feature F07 - Logwatch e IPC
  ✓ [PASÓ]  Feature F06 - Antivirus y cuarentena
  ✓ [PASÓ]  Feature F05 - Sensor eBPF
  ✓ [PASÓ]  Feature F04 - Procesos
  ✓ [PASÓ]  Feature F03 - Métricas
  ✓ [PASÓ]  Feature F02 - Alertas
  ✓ [PASÓ]  Feature F01 - Autenticación y PAM
  ✓ [PASÓ]  Integración general (F00-F07)
  ✓ [PASÓ]  Limpieza de usuarios (0 residuales)
--------------------------------------------------------------------
  Suites aprobadas: 9 · Suites fallidas: 0
====================================================================
```

---

## 9. Inicio del Sistema y Presentación

Para iniciar la demostración:

1. **Lanzar el menú de demostración:**
   ```bash
   cd ~/Documentos/mapa/ProyectoUnico_2S2026
   scripts/demo.sh
   ```
   - Presiona `1` para arrancar el daemon en segundo plano (puerto `8080`).

2. **Abrir la interfaz gráfica:**
   Abre tu navegador web y entra a:
   **`http://localhost:8080/`** (o `http://<IP_DE_LA_MAQUINA>:8080/`).

3. **Demostración guiada:**
   Sigue las instrucciones detalladas de flujo en [`docs/Guia_Demostracion.md`](Guia_Demostracion.md) para mostrar cada panel y funcionalidad al evaluador.

---

## 10. Solución de Problemas Frecuentes (Troubleshooting)

### A. La máquina inició con el kernel viejo tras reiniciar
- **Causa:** GRUB tiene como predeterminado el kernel anterior de Ubuntu.
- **Solución:**
  1. Durante el arranque, mantén presionada la tecla `Shift` (en BIOS) o `Esc` (en UEFI) para ver el menú de GRUB.
  2. Selecciona **"Opciones avanzadas para Ubuntu"** y elige el kernel `6.12.69-so2-202100106`.
  3. Para fijarlo como predeterminado permanente:
     ```bash
     sudo sed -i 's/GRUB_DEFAULT=0/GRUB_DEFAULT="1>2"/' /etc/default/grub
     sudo update-grub
     ```

### B. Error al compilar eBPF: `vmlinux.h: No such file or directory`
- **Causa:** El kernel activo no tiene soporte BTF cargado o no se está ejecutando el kernel compilado.
- **Solución:** Verifica que `uname -r` devuelva `6.12.69-so2-202100106` y que `/sys/kernel/btf/vmlinux` exista. Si estás en el kernel correcto, regenera el header:
  ```bash
  bpftool btf dump file /sys/kernel/btf/vmlinux format c > ~/Documentos/mapa/ProyectoUnico_2S2026/ProgramaIntermedio/ebpf/vmlinux.h
  ```

### C. Error de inicio de sesión: `Usuario sin grupo autorizado` o `403`
- **Causa:** El usuario ingresado no pertenece a `admin_user` ni a `common_user`.
- **Solución:**
  ```bash
  sudo usermod -aG admin_user <tu_usuario>
  ```

### D. El puerto 8080 está ocupado
- **Causa:** Un proceso anterior quedó escuchando en el puerto.
- **Solución:**
  ```bash
  sudo pkill -TERM daemon_monitor
  ```

### E. Restaurar permisos de archivos puestos en cuarentena
- **Causa:** Durante las pruebas los archivos quedan con permisos `0000`.
- **Solución:**
  ```bash
  chmod 664 ~/Documentos/mapa/ProyectoUnico_2S2026/archivos_prueba/*.txt
  chmod 664 ~/so2_vigilado/*.txt 2>/dev/null || true
  ```
