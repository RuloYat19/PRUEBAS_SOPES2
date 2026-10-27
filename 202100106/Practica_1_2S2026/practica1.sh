#!/usr/bin/env bash
# ==============================================================================
# Carne: 202100106
# ==============================================================================

set -euo pipefail  

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'

log()  { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
err()  { echo -e "${RED}✗ $*${NC}" >&2; }
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

trap 'err "El script fallo en la linea $LINENO. Revisa el mensaje anterior."' ERR

# ------------------------------------------------------------------------------
# 0. RUTAS Y VARIABLES
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(basename "$SCRIPT_DIR")" = "Practica_1_2S2026" ]; then
    PRACTICA_DIR="$SCRIPT_DIR"
elif [ -d "${SCRIPT_DIR}/Practica_1_2S2026" ]; then
    PRACTICA_DIR="${SCRIPT_DIR}/Practica_1_2S2026"
else
    PRACTICA_DIR="$(find "$SCRIPT_DIR" -maxdepth 3 -type d -name "Practica_1_2S2026" -print -quit 2>/dev/null || true)"
    if [ -z "$PRACTICA_DIR" ]; then
        D="$SCRIPT_DIR"
        while [ "$D" != "/" ]; do
            if [ -d "${D}/Practica_1_2S2026" ]; then PRACTICA_DIR="${D}/Practica_1_2S2026"; break; fi
            [ "$(basename "$D")" = "Practica_1_2S2026" ] && { PRACTICA_DIR="$D"; break; }
            D="$(dirname "$D")"
        done
    fi
fi

if [ -z "${PRACTICA_DIR:-}" ] || [ ! -d "$PRACTICA_DIR" ]; then
    err "No se pudo localizar 'Practica_1_2S2026' desde: ${SCRIPT_DIR}"
    exit 1
fi

WORKSPACE_DIR="${SCRIPT_DIR}/so2_build_workspace"

log "Carpeta de entrega: ${PRACTICA_DIR}"
log "Workspace de build: ${WORKSPACE_DIR}"

CARNE="202100106"
KERNEL_VERSION="7.1.8"
KERNEL_TAR="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_DIR="linux-${KERNEL_VERSION}"
LOCALVERSION_STR="-so2-${CARNE}"
FULL_KERNEL_NAME="${KERNEL_VERSION}${LOCALVERSION_STR}"
MOK_DIR="/root/mok"

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}   KERNEL ${KERNEL_VERSION} - ${CARNE}${NC}"
echo -e "${BLUE}====================================================================${NC}"

for f in kernel/sys.c include/linux/syscalls.h arch/x86/entry/syscalls/syscall_64.tbl; do
    if [ ! -f "${PRACTICA_DIR}/${f}" ]; then
        err "Falta el archivo modificado: ${PRACTICA_DIR}/${f}"
        exit 1
    fi
done
log "Los 3 archivos modificados del kernel estan presentes."

ESPACIO_KB=$(df --output=avail -k "$SCRIPT_DIR" | tail -1)
if [ "$ESPACIO_KB" -lt 15000000 ]; then
    err "Espacio insuficiente: $((ESPACIO_KB/1024/1024)) GB libres. Se requieren 15 GB."
    exit 1
fi
log "Espacio en disco suficiente: $((ESPACIO_KB/1024/1024)) GB."

# Cachear sudo y mantenerlo vivo durante toda la compilacion
sudo -v
( while true; do sudo -n true; sleep 50; kill -0 "$$" 2>/dev/null || exit; done ) &
SUDO_KEEPALIVE_PID=$!
trap 'kill $SUDO_KEEPALIVE_PID 2>/dev/null || true' EXIT

# ------------------------------------------------------------------------------
# 1. DEPENDENCIAS
# ------------------------------------------------------------------------------
fase "1/9" "Verificando dependencias de compilacion..."

DEPENDENCIAS=(
    build-essential libncurses-dev flex bison libssl-dev libelf-dev
    bc dwarves zstd cpio kmod rsync git wget gawk sbsigntool openssl mokutil
    pciutils initramfs-tools
)

FALTANTES=()
for dep in "${DEPENDENCIAS[@]}"; do
    dpkg -s "$dep" &>/dev/null || FALTANTES+=("$dep")
done

if [ ${#FALTANTES[@]} -ne 0 ]; then
    warn "Instalando: ${FALTANTES[*]}"
    sudo apt update
    sudo apt install -y "${FALTANTES[@]}"
fi
log "Dependencias listas."

# ------------------------------------------------------------------------------
# 2. LIMPIEZA DE INSTALACIONES PREVIAS
# ------------------------------------------------------------------------------
fase "2/9" "Verificando kernels previos con carne ${CARNE}..."

if [[ "$(uname -r)" == *"${CARNE}"* ]]; then
    warn "Estas ejecutando actualmente el kernel ${FULL_KERNEL_NAME}."
    warn "Se omite la eliminacion para no comprometer la sesion actual."
    warn "Recomendado: arranca con el kernel original de Ubuntu antes de recompilar."
else
    KERNELS_A_BORRAR=$(ls -d /boot/*"${CARNE}"* /lib/modules/*"${CARNE}"* 2>/dev/null || true)

    if [ -n "$KERNELS_A_BORRAR" ]; then
        echo -e "${YELLOW}Se encontraron los siguientes archivos/directorios de kernels previos:${NC}"
        echo "$KERNELS_A_BORRAR" | sed 's/^/  - /'
        echo
        read -r -p "¿Deseas eliminar estos kernels anteriores? [y/N]: " RESP
        if [[ "$RESP" =~ ^[YySs]$ ]]; then
            echo "$KERNELS_A_BORRAR" | xargs -r sudo rm -rf
            log "Archivos de kernels previos eliminados correctamente."
        else
            warn "Limpieza de kernels previos omitida por el usuario."
        fi
    else
        log "No se encontraron kernels previos instalados con el carne ${CARNE}."
    fi
fi

# ------------------------------------------------------------------------------
# 3. CODIGO FUENTE
# ------------------------------------------------------------------------------
fase "3/9" "Preparando codigo fuente de Linux ${KERNEL_VERSION}..."

mkdir -p "$WORKSPACE_DIR"
cd "$WORKSPACE_DIR"

[ -f "$KERNEL_TAR" ] || wget -c "https://cdn.kernel.org/pub/linux/kernel/v7.x/${KERNEL_TAR}"
[ -d "$KERNEL_DIR" ] || tar -xf "$KERNEL_TAR"

cd "$KERNEL_DIR"
log "Arbol del kernel listo en $(pwd)"
fase "4/9" "Aplicando los 3 archivos modificados..."

cp "${PRACTICA_DIR}/kernel/sys.c"                                   kernel/sys.c
cp "${PRACTICA_DIR}/include/linux/syscalls.h"                       include/linux/syscalls.h
cp "${PRACTICA_DIR}/arch/x86/entry/syscalls/syscall_64.tbl"         arch/x86/entry/syscalls/syscall_64.tbl

grep -q "getpid_counter" kernel/sys.c                            || { err "sys.c no contiene getpid_counter"; exit 1; }
grep -q "sys_getpid_counter" include/linux/syscalls.h            || { err "syscalls.h no declara el prototipo"; exit 1; }
NUM_SYSCALL=$(grep "getpid_counter" arch/x86/entry/syscalls/syscall_64.tbl | awk '{print $1}')
[ -n "$NUM_SYSCALL" ] || { err "syscall_64.tbl no registra getpid_counter"; exit 1; }

log "Archivos aplicados. Numero de syscall asignado: ${NUM_SYSCALL}"

# ------------------------------------------------------------------------------
# 5. CONFIGURACION
# ------------------------------------------------------------------------------
fase "5/9" "Configurando .config para el hardware de esta maquina..."

cp "/boot/config-$(uname -r)" .config
make olddefconfig >/dev/null

# --- Neutralizar trampas del .config de Ubuntu ---
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
scripts/config --disable DEBUG_INFO
scripts/config --disable DEBUG_INFO_DWARF5
scripts/config --disable DEBUG_INFO_BTF
scripts/config --enable  DEBUG_INFO_NONE
scripts/config --disable RUST

# --- Reducir con localmodconfig segun el hardware REAL de esta maquina ---
# Esto es lo que hace el script portable: conserva solo los modulos cargados aqui.
warn "Asegurate de tener conectado y activo todo el hardware (wifi, USB, audio)."
lsmod > /tmp/mods-actuales.txt
set +o pipefail
yes '' | make LSMOD=/tmp/mods-actuales.txt localmodconfig >/dev/null 2>&1 || true
set -o pipefail

# --- Blindaje de almacenamiento y entrada ---
for opt in BLK_DEV_NVME SATA_AHCI ATA_PIIX USB_XHCI_HCD USB_EHCI_HCD \
           HID_GENERIC USB_HID EXT4_FS BTRFS_FS VFAT_FS BLK_DEV_INITRD; do
    scripts/config --enable "$opt"
done

# --- Blindaje de video: lo que faltaba y causaba la pantalla negra ---
for opt in DRM DRM_FBDEV_EMULATION DRM_SIMPLEDRM SYSFB_SIMPLEFB \
           FB FB_EFI FRAMEBUFFER_CONSOLE; do
    scripts/config --enable "$opt"
done

# --- Driver de GPU segun el hardware detectado ---
GPU_INFO=$(lspci | grep -iE "VGA|3D controller" || true)
echo "GPU detectada: ${GPU_INFO:-ninguna}"
if echo "$GPU_INFO" | grep -qi nvidia; then
    scripts/config --enable DRM_NOUVEAU
    log "Habilitado DRM_NOUVEAU (GPU NVIDIA)."
fi
if echo "$GPU_INFO" | grep -qiE "\bAMD\b|ATI|Radeon"; then
    scripts/config --enable DRM_AMDGPU
    scripts/config --enable DRM_RADEON
    log "Habilitado DRM_AMDGPU (GPU AMD)."
fi
if echo "$GPU_INFO" | grep -qi intel; then
    scripts/config --enable DRM_I915
    log "Habilitado DRM_I915 (GPU Intel)."
fi

# --- Version, firma de modulos y lockdown ---
scripts/config --set-str LOCALVERSION "$LOCALVERSION_STR"
scripts/config --disable LOCALVERSION_AUTO
scripts/config --enable  MODULE_SIG
scripts/config --enable  MODULE_SIG_ALL
scripts/config --enable  MODULE_SIG_SHA256
# Lockdown bloquea modulos sin firmar bajo Secure Boot. Se desactiva para que
# el kernel cargue sus propios modulos; Secure Boot sigue verificando la imagen.
scripts/config --disable SECURITY_LOCKDOWN_LSM
scripts/config --disable SECURITY_LOCKDOWN_LSM_EARLY
scripts/config --disable LOCK_DOWN_KERNEL_FORCE_INTEGRITY
scripts/config --disable LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY

make olddefconfig >/dev/null

# --- VERIFICACION: scripts/config propone, olddefconfig dispone ---
echo -e "\nVerificando que la configuracion critica sobrevivio a olddefconfig:"
verificar_off() {
    if grep -qE "^CONFIG_$1=(y|m)" .config; then
        err "CONFIG_$1 sigue ACTIVO y deberia estar apagado."
        return 1
    fi
    echo "  [ok] CONFIG_$1 desactivado"
}
verificar_on() {
    if ! grep -qE "^CONFIG_$1=(y|m)" .config; then
        err "CONFIG_$1 NO quedo activo y es necesario."
        return 1
    fi
    echo "  [ok] CONFIG_$1 activo"
}

FALLO_CONFIG=0
verificar_off SECURITY_LOCKDOWN_LSM || FALLO_CONFIG=1
verificar_off DEBUG_INFO_BTF        || FALLO_CONFIG=1
verificar_off RUST                  || FALLO_CONFIG=1
verificar_on  DRM                   || FALLO_CONFIG=1
verificar_on  DRM_SIMPLEDRM         || FALLO_CONFIG=1
verificar_on  BLK_DEV_INITRD        || FALLO_CONFIG=1

grep -q 'CONFIG_LOCALVERSION="-so2-'"${CARNE}"'"' .config \
    && echo "  [ok] LOCALVERSION con carne" \
    || { err "LOCALVERSION no quedo configurado"; FALLO_CONFIG=1; }

grep -q 'debian/' .config && { err "El .config aun referencia rutas debian/"; FALLO_CONFIG=1; } || true

if [ "$FALLO_CONFIG" -ne 0 ]; then
    err "La configuracion tiene problemas. Corrige antes de compilar (usa: make menuconfig)."
    exit 1
fi
log "Configuracion validada."

# ------------------------------------------------------------------------------
# 6. COMPILACION
# ------------------------------------------------------------------------------
CORES=$(nproc)
fase "6/9" "Compilando con ${CORES} nucleos (esto tarda entre 15 y 60 min)..."

# pipefail (definido arriba) hace que un fallo de make aborte el script
time make -j"$CORES" 2>&1 | tee "${WORKSPACE_DIR}/compilacion.log"

[ -f arch/x86/boot/bzImage ] || { err "No se genero bzImage. Revisa compilacion.log"; exit 1; }
log "Compilacion exitosa: $(ls -lh arch/x86/boot/bzImage | awk '{print $5}')"

# ------------------------------------------------------------------------------
# 7. INSTALACION
# ------------------------------------------------------------------------------
fase "7/9" "Instalando modulos y kernel..."

sudo make modules_install
log "Modulos instalados en /lib/modules/${FULL_KERNEL_NAME}"

# Reconstruir modulos DKMS (NVIDIA propietario, VirtualBox, etc.) si existen
if command -v dkms &>/dev/null && [ -n "$(dkms status 2>/dev/null)" ]; then
    warn "Modulos DKMS detectados. Reconstruyendo para el kernel nuevo..."
    sudo dkms autoinstall -k "$FULL_KERNEL_NAME" || \
        warn "DKMS fallo. Si usas NVIDIA propietario podrias arrancar sin video."
fi

sudo make install

# Verificar que /boot recibio todo
for archivo in vmlinuz initrd.img System.map config; do
    if [ ! -f "/boot/${archivo}-${FULL_KERNEL_NAME}" ]; then
        if [ "$archivo" = "initrd.img" ]; then
            warn "Falta initrd. Generandolo..."
            sudo update-initramfs -c -k "$FULL_KERNEL_NAME"
        else
            err "Falta /boot/${archivo}-${FULL_KERNEL_NAME}"
            exit 1
        fi
    fi
done
log "Kernel instalado en /boot."

# Eliminar el .old que crea make install: no esta firmado y ensucia el menu
sudo rm -f "/boot/vmlinuz-${FULL_KERNEL_NAME}.old" "/boot/initrd.img-${FULL_KERNEL_NAME}.old" 2>/dev/null || true

# ------------------------------------------------------------------------------
# 8. SECURE BOOT Y FIRMA MOK
# ------------------------------------------------------------------------------
fase "8/9" "Evaluando Secure Boot..."

SB_STATE=$(mokutil --sb-state 2>/dev/null || echo "disabled")
echo "Estado: $SB_STATE"

if echo "$SB_STATE" | grep -qi "enabled"; then
    warn "Secure Boot ACTIVO. El kernel debe firmarse con una MOK."

    sudo mkdir -p "$MOK_DIR"

    if ! sudo test -f "${MOK_DIR}/MOK.priv" || ! sudo test -f "${MOK_DIR}/MOK.pem"; then
        echo "Generando par de claves MOK..."
        sudo openssl req -new -x509 -newkey rsa:2048 \
            -keyout "${MOK_DIR}/MOK.priv" \
            -out    "${MOK_DIR}/MOK.pem" \
            -nodes -days 3650 \
            -subj "/CN=SO2 Kernel ${KERNEL_VERSION} ${CARNE}/"
        sudo chmod 600 "${MOK_DIR}/MOK.priv"
        # mokutil exige DER; sbsign exige PEM. Por eso se guardan ambos.
        sudo openssl x509 -in "${MOK_DIR}/MOK.pem" -outform DER -out "${MOK_DIR}/MOK.der"
        sudo openssl x509 -inform DER -in "${MOK_DIR}/MOK.der" -noout -subject \
            || { err "El DER generado no es valido"; exit 1; }
        log "Claves MOK generadas."
    else
        log "Claves MOK existentes reutilizadas."
    fi

    # Enrolar solo si aun no esta en el firmware
    if sudo mokutil --list-enrolled 2>/dev/null | grep -q "SO2 Kernel"; then
        log "La clave MOK ya esta enrolada en el firmware."
    else
        echo
        warn "==> Se pedira una contrasena TEMPORAL de un solo uso."
        warn "==> Usa SOLO DIGITOS: MokManager arranca con teclado en ingles."
        sudo mokutil --import "${MOK_DIR}/MOK.der"
        MOK_PENDIENTE=1
    fi

    # Firmar a un temporal y verificar ANTES de reemplazar el kernel bueno
    echo "Firmando el kernel..."
    sudo sbsign --key "${MOK_DIR}/MOK.priv" --cert "${MOK_DIR}/MOK.pem" \
        --output /tmp/vmlinuz-firmado \
        "/boot/vmlinuz-${FULL_KERNEL_NAME}"

    sudo sbverify --list /tmp/vmlinuz-firmado \
        || { err "La firma no se pudo verificar. No se reemplaza el kernel."; exit 1; }

    sudo mv /tmp/vmlinuz-firmado "/boot/vmlinuz-${FULL_KERNEL_NAME}"
    sudo chmod 644 "/boot/vmlinuz-${FULL_KERNEL_NAME}"
    log "Kernel firmado y verificado."
else
    log "Secure Boot desactivado. No se requiere firma."
fi

# update-grub va AL FINAL, despues de firmar y de limpiar los .old
sudo update-grub
echo
echo "Entradas del menu de arranque:"
sudo grep "menuentry '" /boot/grub/grub.cfg | sed 's/^/  /'

# ------------------------------------------------------------------------------
# 9. PROGRAMA DE PRUEBA
# ------------------------------------------------------------------------------
fase "9/9" "Compilando el programa de espacio de usuario..."

PROGRAMA_DIR="${PRACTICA_DIR}/Programa_Intermedio"
if [ -d "$PROGRAMA_DIR" ]; then
    cd "$PROGRAMA_DIR"
    if [ -f Makefile ]; then
        make clean &>/dev/null || true
        make
    else
        gcc -Wall -o prueba prueba.c
    fi
    log "Binario compilado: ${PROGRAMA_DIR}/prueba"

    C_SRC_FILE="$(ls Prueba.C prueba.c 2>/dev/null | head -1 || echo "")"
    if [ -n "$C_SRC_FILE" ]; then
        NUM_EN_C=$(grep -oP '#define\s+SYS_GETPID_COUNTER\s+\K[0-9]+' "$C_SRC_FILE" || echo "")
        if [ -n "$NUM_EN_C" ] && [ "$NUM_EN_C" != "$NUM_SYSCALL" ]; then
            err "DESAJUSTE: ${C_SRC_FILE} usa ${NUM_EN_C} pero la tabla asigna ${NUM_SYSCALL}."
            err "Corrige el #define en ${C_SRC_FILE} o la syscall devolvera ENOSYS."
        else
            log "Numero de syscall coincide: ${NUM_SYSCALL}"
        fi
    fi
else
    warn "No se encontro Programa_Intermedio."
fi

# ------------------------------------------------------------------------------
# RESUMEN
# ------------------------------------------------------------------------------
echo -e "\n${RED}====================================================================${NC}"
echo -e "${RED} 💀 YA TE HAKIE BRO 💀${NC}"
echo -e "${RED}====================================================================${NC}"
sleep 3
echo -e "\n${RED}====================================================================${NC}"
echo -e "${RED} XDDDDD${NC}"
echo -e "${RED}====================================================================${NC}"
sleep 3
echo -e "\n${GREEN}====================================================================${NC}"
echo -e "${GREEN}   PROCESO COMPLETADO${NC}"
echo -e "${GREEN}====================================================================${NC}"
echo "Kernel instalado : ${FULL_KERNEL_NAME}"
echo "Syscall asignada : ${NUM_SYSCALL}"
echo "Log de build     : ${WORKSPACE_DIR}/compilacion.log"
echo
echo "Pasos siguientes:"
echo "  1. sudo reboot"
if [ "${MOK_PENDIENTE:-0}" = "1" ]; then
    echo -e "  2. ${YELLOW}Pantalla azul MokManager${NC}: Enroll MOK > Continue > Yes > contrasena"
    echo "     Si te saltas esta pantalla, el enrolamiento se cancela."
    echo "  3. En GRUB: Advanced options > Ubuntu, with Linux ${FULL_KERNEL_NAME}"
else
    echo "  2. En GRUB: Advanced options > Ubuntu, with Linux ${FULL_KERNEL_NAME}"
fi
echo
echo "Ya dentro del kernel nuevo:"
echo "  uname -r                                   # debe mostrar ${FULL_KERNEL_NAME}"
echo "  lsmod | wc -l                              # debe ser un numero alto, no 0"
echo "  cd ${PROGRAMA_DIR} && ./prueba"
echo "  sudo dmesg | grep getpid_counter"
echo
warn "Si recompilas: make install sobrescribe el kernel SIN firma."
warn "Vuelve a ejecutar este script o firma manualmente antes de reiniciar."