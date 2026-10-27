#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
err()  { echo -e "${RED}✗ $*${NC}" >&2; }
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

trap 'err "El script fallo en la linea $LINENO."' ERR

CARNE="202100106"
KERNEL_VERSION="6.12.69"
KERNEL_TAR="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${KERNEL_TAR}"
LOCALVERSION_STR="-so2-${CARNE}"
FULL_KERNEL_NAME="${KERNEL_VERSION}${LOCALVERSION_STR}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY="${SCRIPT_DIR}/linux-${KERNEL_VERSION}"
WORKSPACE_DIR="$(dirname "$SCRIPT_DIR")/Practica_3_2S2026/so2_build_workspace"
ARBOL="${WORKSPACE_DIR}/linux-${KERNEL_VERSION}"

SYSCALLS=(ipc_channel_send ipc_channel_receive start_log_watch stop_log_watch
          get_process_info get_processes_state)

SIN_INSTALAR=0
for argumento in "$@"; do
    case "$argumento" in
        --sin-instalar) SIN_INSTALAR=1 ;;
        *) err "Opcion no reconocida: ${argumento}"; exit 1 ;;
    esac
done

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}   PREPARACION DEL KERNEL P4 - SO2 2S2026 - CARNE ${CARNE}${NC}"
echo -e "${BLUE}   Kernel: ${FULL_KERNEL_NAME}${NC}"
echo -e "${BLUE}====================================================================${NC}"
log "Overlay de la practica : ${OVERLAY}"
log "Arbol de compilacion   : ${ARBOL}"

fase "1/7" "Verificando dependencias..."
DEPENDENCIAS=(build-essential libncurses-dev flex bison libssl-dev libelf-dev
              bc zstd cpio kmod rsync gawk wget curl python3 pciutils)
if mokutil --sb-state 2>/dev/null | grep -qi "enabled"; then
    DEPENDENCIAS+=(sbsigntool openssl mokutil)
fi
command -v dracut >/dev/null 2>&1 || DEPENDENCIAS+=(initramfs-tools)

FALTANTES=()
for paquete in "${DEPENDENCIAS[@]}"; do
    dpkg -s "$paquete" >/dev/null 2>&1 || FALTANTES+=("$paquete")
done
if [ ${#FALTANTES[@]} -ne 0 ]; then
    warn "Instalando: ${FALTANTES[*]}"
    sudo apt-get update
    sudo apt-get install -y "${FALTANTES[@]}"
fi
log "Dependencias listas."

fase "2/7" "Ubicando el arbol de compilacion..."
if [ -f "${ARBOL}/Makefile" ]; then
    log "Se reutiliza el arbol existente; la compilacion sera incremental."
else
    warn "No existe ${ARBOL}; se descargan las fuentes."
    mkdir -p "$WORKSPACE_DIR"
    ESPACIO_KB=$(df --output=avail -k "$WORKSPACE_DIR" | tail -1)
    if [ "$ESPACIO_KB" -lt 10000000 ]; then
        err "Espacio insuficiente: $((ESPACIO_KB / 1024 / 1024)) GB libres, se requieren 10 GB."
        exit 1
    fi
    cd "$WORKSPACE_DIR"
    [ -f "$KERNEL_TAR" ] || wget -c "$KERNEL_URL"
    tar -xf "$KERNEL_TAR"
fi
cd "$ARBOL"
if [ "$(make -s kernelversion)" != "$KERNEL_VERSION" ]; then
    err "El arbol reporta $(make -s kernelversion) y se requiere ${KERNEL_VERSION}."
    exit 1
fi

fase "3/7" "Aplicando el overlay de la Practica 4..."
rsync -rc --itemize-changes "${OVERLAY}/" "${ARBOL}/" | sed 's/^/  /'
for nombre in "${SYSCALLS[@]}"; do
    if ! grep -qP "\t${nombre}\tsys_${nombre}$" arch/x86/entry/syscalls/syscall_64.tbl; then
        err "Falta la syscall ${nombre} en syscall_64.tbl"
        exit 1
    fi
done
for objeto in ipc_channel.o log_watch.o mi_syscall.o; do
    grep -q "$objeto" kernel/Makefile || { err "Falta ${objeto} en kernel/Makefile"; exit 1; }
done
log "Overlay aplicado: 6 syscalls (548-553) y 3 objetos registrados."

fase "4/7" "Preparando la configuracion..."
if [ -f .config ]; then
    log "Se conserva la configuracion existente, ya ajustada al hardware de esta maquina."
else
    warn "No hay .config; se genera a partir del kernel en ejecucion."
    warn "Conecta y activa todo el hardware (wifi, USB, audio): localmodconfig solo conserva lo cargado."
    cp "/boot/config-$(uname -r)" .config
    make olddefconfig >/dev/null
    scripts/config --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
        --disable DEBUG_INFO --disable DEBUG_INFO_DWARF5 \
        --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT --disable DEBUG_INFO_BTF \
        --enable DEBUG_INFO_NONE --disable RUST
    lsmod > "${WORKSPACE_DIR}/modulos-activos.txt"
    set +o pipefail
    yes '' | make LSMOD="${WORKSPACE_DIR}/modulos-activos.txt" localmodconfig >/dev/null 2>&1 || true
    set -o pipefail

    for opcion in BLK_DEV_NVME SATA_AHCI ATA_PIIX BLK_DEV_SD USB_XHCI_HCD USB_EHCI_HCD \
                  HID_GENERIC USB_HID EXT4_FS BTRFS_FS VFAT_FS BLK_DEV_INITRD \
                  DRM DRM_FBDEV_EMULATION DRM_SIMPLEDRM SYSFB_SIMPLEFB \
                  FB FB_EFI FRAMEBUFFER_CONSOLE; do
        scripts/config --enable "$opcion"
    done
    GPU_DETECTADA=$(lspci | grep -iE "VGA|3D controller" || true)
    if echo "$GPU_DETECTADA" | grep -qi nvidia; then scripts/config --enable DRM_NOUVEAU; fi
    if echo "$GPU_DETECTADA" | grep -qiE "\bAMD\b|\bATI\b|Radeon"; then
        scripts/config --enable DRM_AMDGPU --enable DRM_RADEON
    fi
    if echo "$GPU_DETECTADA" | grep -qi intel; then scripts/config --enable DRM_I915; fi
    scripts/config --enable MODULE_SIG --enable MODULE_SIG_ALL --enable MODULE_SIG_SHA256 \
        --disable SECURITY_LOCKDOWN_LSM --disable SECURITY_LOCKDOWN_LSM_EARLY
fi
scripts/config --set-str LOCALVERSION "$LOCALVERSION_STR" --disable LOCALVERSION_AUTO
make olddefconfig >/dev/null
if [ "$(make -s kernelrelease)" != "$FULL_KERNEL_NAME" ]; then
    err "El kernel se llamaria $(make -s kernelrelease) y se requiere ${FULL_KERNEL_NAME}."
    exit 1
fi
log "Configuracion lista: $(grep -c '=m$' .config) modulos, version ${FULL_KERNEL_NAME}."

fase "5/7" "Compilando con $(nproc) nucleos..."
warn "Si cambio la tabla de syscalls se recompila casi todo el kernel: puede tardar de 15 minutos a mas de una hora."
set +e
make -j"$(nproc)" 2>&1 | tee "${WORKSPACE_DIR}/compilacion_p4.log" | grep -E '^Kernel:|error:|\*\*\*'
ESTADO_MAKE=${PIPESTATUS[0]}
set -e
if [ "$ESTADO_MAKE" -ne 0 ] || [ ! -f arch/x86/boot/bzImage ]; then
    err "La compilacion fallo; revisa ${WORKSPACE_DIR}/compilacion_p4.log"
    exit 1
fi
log "bzImage listo: $(ls -lh arch/x86/boot/bzImage | awk '{ print $5 }')"

fase "6/7" "Verificando las syscalls en System.map..."
for nombre in "${SYSCALLS[@]}"; do
    grep -q " __x64_sys_${nombre}$" System.map || { err "System.map no contiene sys_${nombre}"; exit 1; }
done
grep -q " ipc_channel_enqueue_kernel$" System.map || { err "Falta ipc_channel_enqueue_kernel"; exit 1; }
log "Las 6 syscalls y la notificacion interna de log_watch estan en el kernel."

fase "7/7" "Compilando el daemon y el validador para esta maquina..."
make -C "${SCRIPT_DIR}/daemon" clean all >/dev/null
gcc -O2 -Wall -o "${SCRIPT_DIR}/test_fase1" "${SCRIPT_DIR}/test_fase1.c"
log "daemon_monitor y test_fase1 recompilados."

if [ "$SIN_INSTALAR" -eq 1 ]; then
    echo -e "\n${GREEN}Kernel compilado. Para instalarlo: ${YELLOW}${SCRIPT_DIR}/instalar_kernel.sh${NC}"
    exit 0
fi

exec "${SCRIPT_DIR}/instalar_kernel.sh"
