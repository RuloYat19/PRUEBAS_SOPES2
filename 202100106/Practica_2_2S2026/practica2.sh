#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'

log()  { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
err()  { echo -e "${RED}✗ $*${NC}" >&2; }
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

trap 'err "El script fallo en la linea $LINENO."' ERR

# --- Rutas y Directorios ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(basename "$SCRIPT_DIR")" = "Practica_2_2S2026" ]; then
    PRACTICA_DIR="$SCRIPT_DIR"
elif [ -d "${SCRIPT_DIR}/Practica_2_2S2026" ]; then
    PRACTICA_DIR="${SCRIPT_DIR}/Practica_2_2S2026"
else
    PRACTICA_DIR="$(find "$SCRIPT_DIR" -maxdepth 3 -type d -name "Practica_2_2S2026" -print -quit 2>/dev/null || true)"
    if [ -z "$PRACTICA_DIR" ]; then
        D="$SCRIPT_DIR"
        while [ "$D" != "/" ]; do
            if [ -d "${D}/Practica_2_2S2026" ]; then PRACTICA_DIR="${D}/Practica_2_2S2026"; break; fi
            [ "$(basename "$D")" = "Practica_2_2S2026" ] && { PRACTICA_DIR="$D"; break; }
            D="$(dirname "$D")"
        done
    fi
fi

if [ -z "${PRACTICA_DIR:-}" ] || [ ! -d "$PRACTICA_DIR" ]; then
    err "No se encontro 'Practica_2_2S2026' desde ${SCRIPT_DIR}"
    exit 1
fi

WORKSPACE_DIR="${SCRIPT_DIR}/so2_build_workspace"
mkdir -p "$WORKSPACE_DIR"
CARNE="202100106"
KERNEL_VERSION="7.1.8"
KERNEL_TAR="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_DIR="linux-${KERNEL_VERSION}"
LOCALVERSION_STR="-so2-${CARNE}"
FULL_KERNEL_NAME="${KERNEL_VERSION}${LOCALVERSION_STR}"
MOK_DIR="/root/mok"

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}   P2 - KERNEL ${KERNEL_VERSION} - CARNE ${CARNE}${NC}"
echo -e "${BLUE}====================================================================${NC}"
log "Carpeta de entrega: ${PRACTICA_DIR}"
log "Workspace de build: ${WORKSPACE_DIR}"

KERNEL_SRC_OVERLAY="$(find "$PRACTICA_DIR" -maxdepth 1 -type d -name "linux-*" -print -quit 2>/dev/null || echo "")"
if [ -z "$KERNEL_SRC_OVERLAY" ] || [ ! -d "$KERNEL_SRC_OVERLAY" ]; then
    err "No se encontro el directorio fuente 'linux-6.X.X' en ${PRACTICA_DIR}"
    exit 1
fi

for f in kernel/mi_syscall.c kernel/Makefile arch/x86/entry/syscalls/syscall_64.tbl; do
    if [ ! -f "${KERNEL_SRC_OVERLAY}/${f}" ]; then
        err "Falta el archivo: ${KERNEL_SRC_OVERLAY}/${f}"
        exit 1
    fi
done

ESPACIO_KB=$(df --output=avail -k "$SCRIPT_DIR" | tail -1)
if [ "$ESPACIO_KB" -lt 15000000 ]; then
    err "Espacio insuficiente: $((ESPACIO_KB/1024/1024)) GB libres (se requieren 15 GB)"
    exit 1
fi

sudo -v
( while true; do sudo -n true; sleep 50; kill -0 "$$" 2>/dev/null || exit; done ) &
SUDO_KEEPALIVE_PID=$!
trap 'kill $SUDO_KEEPALIVE_PID 2>/dev/null || true' EXIT

# --- 1. Dependencias ---
fase "1/9" "Verificando dependencias..."
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
    warn "Instalando paquetes: ${FALTANTES[*]}"
    sudo apt update
    sudo apt install -y "${FALTANTES[@]}"
fi
log "Dependencias listas."

# --- 2. Limpieza interactiva ---
fase "2/9" "Verificando kernels previos con carne ${CARNE}..."
if [[ "$(uname -r)" == *"${CARNE}"* ]]; then
    warn "Estas ejecutando actualmente el kernel ${FULL_KERNEL_NAME}."
    warn "Se omite la eliminacion de /boot para no comprometer la sesion actual."
else
    KERNELS_A_BORRAR=$(ls -d /boot/*"${CARNE}"* /lib/modules/*"${CARNE}"* 2>/dev/null || true)
    if [ -n "$KERNELS_A_BORRAR" ]; then
        echo -e "${YELLOW}Kernels previos encontrados:${NC}"
        echo "$KERNELS_A_BORRAR" | sed 's/^/  - /'
        echo
        read -r -p "¿Deseas eliminar estos kernels anteriores? [y/N]: " RESP
        if [[ "$RESP" =~ ^[YySs]$ ]]; then
            echo "$KERNELS_A_BORRAR" | xargs -r sudo rm -rf
            log "Archivos de kernels previos eliminados."
        else
            warn "Limpieza omitida por el usuario."
        fi
    else
        log "No se encontraron kernels previos instalados."
    fi
fi

# --- 3. Codigo fuente ---
fase "3/9" "Preparando arbol de Linux ${KERNEL_VERSION}..."
mkdir -p "$WORKSPACE_DIR"
cd "$WORKSPACE_DIR"

[ -f "$KERNEL_TAR" ] || wget -c "https://cdn.kernel.org/pub/linux/kernel/v7.x/${KERNEL_TAR}"
[ -d "$KERNEL_DIR" ] || tar -xf "$KERNEL_TAR"
cd "$KERNEL_DIR"
log "Arbol del kernel listo en $(pwd)"

# --- 4. Modificaciones del kernel ---
fase "4/9" "Aplicando archivos modificados..."
cp "${KERNEL_SRC_OVERLAY}/kernel/mi_syscall.c" kernel/mi_syscall.c

if ! grep -q "mi_syscall.o" kernel/Makefile; then
    echo "obj-y += mi_syscall.o" >> kernel/Makefile
fi

if ! grep -q "get_process_info" arch/x86/entry/syscalls/syscall_64.tbl; then
    echo "548	common	get_process_info	sys_get_process_info" >> arch/x86/entry/syscalls/syscall_64.tbl
fi
if ! grep -q "get_processes_state" arch/x86/entry/syscalls/syscall_64.tbl; then
    echo "549	common	get_processes_state	sys_get_processes_state" >> arch/x86/entry/syscalls/syscall_64.tbl
fi

if ! grep -q "sys_get_process_info" include/linux/syscalls.h; then
    sed -i '/asmlinkage long sys_getpid(void);/a asmlinkage long sys_get_process_info(pid_t pid, struct process_info __user *info);\nasmlinkage long sys_get_processes_state(struct processes_state __user *state);' include/linux/syscalls.h
fi
log "Archivos aplicados correctamente."

# --- 5. Configuracion (.config) ---
fase "5/9" "Configurando .config..."
cp "/boot/config-$(uname -r)" .config
make olddefconfig >/dev/null

scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
scripts/config --set-str SYSTEM_TRUSTED_KEYS ""
scripts/config --set-str SYSTEM_REVOCATION_KEYS ""
scripts/config --disable DEBUG_INFO
scripts/config --disable DEBUG_INFO_DWARF5
scripts/config --disable DEBUG_INFO_BTF
scripts/config --enable  DEBUG_INFO_NONE
scripts/config --disable RUST

lsmod > /tmp/mods-actuales.txt
set +o pipefail
yes '' | make LSMOD=/tmp/mods-actuales.txt localmodconfig >/dev/null 2>&1 || true
set -o pipefail

for opt in BLK_DEV_NVME SATA_AHCI ATA_PIIX USB_XHCI_HCD USB_EHCI_HCD \
           HID_GENERIC USB_HID EXT4_FS BTRFS_FS VFAT_FS BLK_DEV_INITRD \
           DRM DRM_FBDEV_EMULATION DRM_SIMPLEDRM SYSFB_SIMPLEFB \
           FB FB_EFI FRAMEBUFFER_CONSOLE; do
    scripts/config --enable "$opt"
done

GPU_INFO=$(lspci | grep -iE "VGA|3D controller" || true)
if echo "$GPU_INFO" | grep -qi nvidia; then
    scripts/config --enable DRM_NOUVEAU
fi
if echo "$GPU_INFO" | grep -qiE "\bAMD\b|ATI|Radeon"; then
    scripts/config --enable DRM_AMDGPU
    scripts/config --enable DRM_RADEON
fi
if echo "$GPU_INFO" | grep -qi intel; then
    scripts/config --enable DRM_I915
fi

scripts/config --set-str LOCALVERSION "$LOCALVERSION_STR"
scripts/config --disable LOCALVERSION_AUTO
scripts/config --enable  MODULE_SIG
scripts/config --enable  MODULE_SIG_ALL
scripts/config --enable  MODULE_SIG_SHA256
scripts/config --disable SECURITY_LOCKDOWN_LSM
scripts/config --disable SECURITY_LOCKDOWN_LSM_EARLY
scripts/config --disable LOCK_DOWN_KERNEL_FORCE_INTEGRITY
scripts/config --disable LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY

make olddefconfig >/dev/null
log "Configuracion consolidada."

# --- 6. Compilacion ---
CORES=$(nproc)
fase "6/9" "Compilando con ${CORES} nucleos..."
time make -j"$CORES" 2>&1 | tee "${WORKSPACE_DIR}/compilacion.log"

[ -f arch/x86/boot/bzImage ] || { err "No se genero bzImage"; exit 1; }
log "Compilacion exitosa: $(ls -lh arch/x86/boot/bzImage | awk '{print $5}')"

# --- 7. Instalacion ---
fase "7/9" "Instalando modulos y kernel..."
sudo make modules_install
log "Modulos instalados en /lib/modules/${FULL_KERNEL_NAME}"

if command -v dkms &>/dev/null && [ -n "$(dkms status 2>/dev/null)" ]; then
    sudo dkms autoinstall -k "$FULL_KERNEL_NAME" || true
fi

sudo make install

for archivo in vmlinuz initrd.img System.map config; do
    if [ ! -f "/boot/${archivo}-${FULL_KERNEL_NAME}" ]; then
        if [ "$archivo" = "initrd.img" ]; then
            sudo update-initramfs -c -k "$FULL_KERNEL_NAME"
        else
            err "Falta /boot/${archivo}-${FULL_KERNEL_NAME}"
            exit 1
        fi
    fi
done
log "Kernel instalado en /boot."

sudo rm -f "/boot/vmlinuz-${FULL_KERNEL_NAME}.old" "/boot/initrd.img-${FULL_KERNEL_NAME}.old" 2>/dev/null || true

# --- 8. Firma MOK (Secure Boot) ---
fase "8/9" "Evaluando Secure Boot..."
SB_STATE=$(mokutil --sb-state 2>/dev/null || echo "disabled")

if echo "$SB_STATE" | grep -qi "enabled"; then
    warn "Secure Boot ACTIVO. Verificando firma..."
    sudo mkdir -p "$MOK_DIR"

    if ! sudo test -f "${MOK_DIR}/MOK.priv" || ! sudo test -f "${MOK_DIR}/MOK.pem"; then
        sudo openssl req -new -x509 -newkey rsa:2048 \
            -keyout "${MOK_DIR}/MOK.priv" \
            -out    "${MOK_DIR}/MOK.pem" \
            -nodes -days 3650 \
            -subj "/CN=SO2 Kernel ${KERNEL_VERSION} ${CARNE}/"
        sudo chmod 600 "${MOK_DIR}/MOK.priv"
        sudo openssl x509 -in "${MOK_DIR}/MOK.pem" -outform DER -out "${MOK_DIR}/MOK.der"
    fi

    if sudo mokutil --list-enrolled 2>/dev/null | grep -q "SO2 Kernel"; then
        log "Clave MOK ya enrolada en el firmware."
    else
        warn "Inscribiendo MOK en UEFI (se pedira contrasena temporal)..."
        sudo mokutil --import "${MOK_DIR}/MOK.der"
        MOK_PENDIENTE=1
    fi

    sudo sbsign --key "${MOK_DIR}/MOK.priv" --cert "${MOK_DIR}/MOK.pem" \
        --output /tmp/vmlinuz-firmado \
        "/boot/vmlinuz-${FULL_KERNEL_NAME}"

    sudo sbverify --list /tmp/vmlinuz-firmado || { err "Fallo de verificacion de firma"; exit 1; }
    sudo mv /tmp/vmlinuz-firmado "/boot/vmlinuz-${FULL_KERNEL_NAME}"
    sudo chmod 644 "/boot/vmlinuz-${FULL_KERNEL_NAME}"
    log "Kernel firmado y verificado."
else
    log "Secure Boot desactivado. Firma no requerida."
fi

sudo update-grub

# --- 9. Programa de prueba ---
fase "9/9" "Compilando programa de usuario..."
PROGRAMA_DIR="${PRACTICA_DIR}/Programa_Intermedio"
if [ -d "$PROGRAMA_DIR" ]; then
    cd "$PROGRAMA_DIR"
    if [ -f Makefile ]; then
        make clean &>/dev/null || true
        make
    else
        gcc -Wall -O2 -o test test.c
    fi
    log "Binario listo: ${PROGRAMA_DIR}/test"
fi

# --- Resumen ---
echo -e "\n${GREEN}====================================================================${NC}"
echo -e "${GREEN}   zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz                ${NC}"
echo -e "${GREEN}====================================================================${NC}"
echo "Kernel instalado : ${FULL_KERNEL_NAME}"
echo "Syscalls         : 548 (get_process_info), 549 (get_processes_state)"
echo "Log de build     : ${WORKSPACE_DIR}/compilacion.log"