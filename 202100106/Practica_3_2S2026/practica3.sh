#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'

log()  { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
err()  { echo -e "${RED}✗ $*${NC}" >&2; }
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

trap 'err "El script fallo en la linea $LINENO."' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CARNE="202100106"
KERNEL_VERSION="6.12.69"
KERNEL_TAR="linux-${KERNEL_VERSION}.tar.xz"
KERNEL_DIR="linux-${KERNEL_VERSION}"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/${KERNEL_TAR}"
LOCALVERSION_STR="-so2-${CARNE}"
FULL_KERNEL_NAME="${KERNEL_VERSION}${LOCALVERSION_STR}"
MOK_DIR="/root/mok"
LOGS_DEMO_DIR="/tmp/so2_logs"

SYS_IPC_SEND=548
SYS_IPC_RECEIVE=549
SYS_START_WATCH=550
SYS_STOP_WATCH=551

SOLO_PROGRAMAS=0
SOLO_LOGS=0
for argumento in "$@"; do
    case "$argumento" in
        --solo-programas) SOLO_PROGRAMAS=1 ;;
        --solo-logs)      SOLO_LOGS=1 ;;
        *) err "Opcion no reconocida: ${argumento}"; exit 1 ;;
    esac
done

ubicar_practica() {
    if [ "$(basename "$SCRIPT_DIR")" = "Practica_3_2S2026" ]; then
        echo "$SCRIPT_DIR"; return
    fi
    if [ -d "${SCRIPT_DIR}/Practica_3_2S2026" ]; then
        echo "${SCRIPT_DIR}/Practica_3_2S2026"; return
    fi
    local encontrada
    encontrada="$(find "$SCRIPT_DIR" -maxdepth 3 -type d -name "Practica_3_2S2026" -print -quit 2>/dev/null || true)"
    if [ -n "$encontrada" ]; then echo "$encontrada"; return; fi
    local actual="$SCRIPT_DIR"
    while [ "$actual" != "/" ]; do
        if [ -d "${actual}/Practica_3_2S2026" ]; then echo "${actual}/Practica_3_2S2026"; return; fi
        actual="$(dirname "$actual")"
    done
}

PRACTICA_DIR="$(ubicar_practica)"
if [ -z "${PRACTICA_DIR:-}" ] || [ ! -d "$PRACTICA_DIR" ]; then
    err "No se encontro la carpeta 'Practica_3_2S2026' desde ${SCRIPT_DIR}"
    exit 1
fi

WORKSPACE_DIR="${PRACTICA_DIR}/so2_build_workspace"
PROGRAMA_DIR="${PRACTICA_DIR}/Programa_Intermedio"

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}   PRACTICA 3 - SO2 2S2026 - KERNEL ${KERNEL_VERSION} - CARNE ${CARNE}${NC}"
echo -e "${BLUE}   IPC Channel + Log Watch (kthread)${NC}"
echo -e "${BLUE}====================================================================${NC}"
log "Carpeta de entrega : ${PRACTICA_DIR}"
log "Workspace de build : ${WORKSPACE_DIR}"

preparar_logs_demo() {
    mkdir -p "$LOGS_DEMO_DIR"
    : > "${LOGS_DEMO_DIR}/app1.log"
    : > "${LOGS_DEMO_DIR}/app2.log"
    : > "${LOGS_DEMO_DIR}/log_centralizado.txt"
    chmod 666 "${LOGS_DEMO_DIR}/log_centralizado.txt"
    {
        echo "INFO servicio de autenticacion iniciado"
        echo "INFO cache precargado correctamente"
    } >> "${LOGS_DEMO_DIR}/app1.log"
    {
        echo "INFO worker de correo iniciado"
        echo "WARN uso de memoria al 78 por ciento"
    } >> "${LOGS_DEMO_DIR}/app2.log"
    log "Archivos de demostracion creados en ${LOGS_DEMO_DIR}"
}

compilar_programas() {
    if [ ! -d "$PROGRAMA_DIR" ]; then
        err "No existe ${PROGRAMA_DIR}"
        exit 1
    fi
    cd "$PROGRAMA_DIR"
    if [ -f Makefile ]; then
        make clean >/dev/null 2>&1 || true
        make
    else
        for fuente in test_ipc_channel_send test_ipc_channel_receive \
                      test_start_log_watch test_stop_log_watch; do
            gcc -Wall -Wextra -O2 -o "$fuente" "${fuente}.c"
        done
    fi
    log "Programas de usuario compilados en ${PROGRAMA_DIR}"
}

if [ "$SOLO_LOGS" -eq 1 ]; then
    fase "1/1" "Preparando archivos de log de demostracion..."
    preparar_logs_demo
    exit 0
fi

if [ "$SOLO_PROGRAMAS" -eq 1 ]; then
    fase "1/2" "Compilando programas de espacio de usuario..."
    compilar_programas
    fase "2/2" "Preparando archivos de log de demostracion..."
    preparar_logs_demo
    exit 0
fi

KERNEL_SRC_OVERLAY="$(find "$PRACTICA_DIR" -maxdepth 1 -type d -name "linux-*" -print -quit 2>/dev/null || echo "")"
if [ -z "$KERNEL_SRC_OVERLAY" ] || [ ! -d "$KERNEL_SRC_OVERLAY" ]; then
    err "No se encontro el overlay 'linux-${KERNEL_VERSION}' dentro de ${PRACTICA_DIR}"
    exit 1
fi

for requerido in kernel/ipc_channel.c kernel/log_watch.c; do
    if [ ! -f "${KERNEL_SRC_OVERLAY}/${requerido}" ]; then
        err "Falta el archivo obligatorio: ${KERNEL_SRC_OVERLAY}/${requerido}"
        exit 1
    fi
done
log "Overlay del kernel validado: ${KERNEL_SRC_OVERLAY}"

if ! command -v apt-get >/dev/null 2>&1; then
    err "Este script requiere una distribucion basada en Debian/Ubuntu/Mint."
    exit 1
fi

ESPACIO_KB=$(df --output=avail -k "$PRACTICA_DIR" | tail -1)
if [ "$ESPACIO_KB" -lt 25000000 ]; then
    err "Espacio insuficiente: $((ESPACIO_KB/1024/1024)) GB libres, se requieren 25 GB"
    exit 1
fi
log "Espacio disponible: $((ESPACIO_KB/1024/1024)) GB"

sudo -v
( while true; do sudo -n true; sleep 50; kill -0 "$$" 2>/dev/null || exit; done ) &
SUDO_KEEPALIVE_PID=$!
trap 'kill $SUDO_KEEPALIVE_PID 2>/dev/null || true' EXIT

fase "1/10" "Verificando dependencias de compilacion..."
DEPENDENCIAS=(
    build-essential libncurses-dev flex bison libssl-dev libelf-dev
    bc dwarves zstd cpio kmod rsync git wget gawk sbsigntool openssl
    mokutil pciutils initramfs-tools
)
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

fase "2/10" "Revisando kernels previos del carne ${CARNE}..."
if [[ "$(uname -r)" == *"${CARNE}"* ]]; then
    warn "Estas ejecutando el kernel ${FULL_KERNEL_NAME}; se omite la limpieza de /boot."
else
    KERNELS_PREVIOS=$(ls -d /boot/*"${CARNE}"* /lib/modules/*"${CARNE}"* 2>/dev/null || true)
    if [ -n "$KERNELS_PREVIOS" ]; then
        echo -e "${YELLOW}Kernels previos encontrados:${NC}"
        echo "$KERNELS_PREVIOS" | sed 's/^/  - /'
        read -r -p "¿Eliminar estos kernels anteriores? [y/N]: " RESPUESTA
        if [[ "$RESPUESTA" =~ ^[YySs]$ ]]; then
            echo "$KERNELS_PREVIOS" | xargs -r sudo rm -rf
            log "Kernels previos eliminados."
        else
            warn "Limpieza omitida."
        fi
    else
        log "No hay kernels previos instalados."
    fi
fi

fase "3/10" "Obteniendo el codigo fuente de Linux ${KERNEL_VERSION}..."
mkdir -p "$WORKSPACE_DIR"
cd "$WORKSPACE_DIR"
[ -f "$KERNEL_TAR" ] || wget -c "$KERNEL_URL"
[ -d "$KERNEL_DIR" ] || tar -xf "$KERNEL_TAR"
cd "$KERNEL_DIR"
VERSION_DETECTADA=$(make kernelversion 2>/dev/null || echo "desconocida")
if [ "$VERSION_DETECTADA" != "$KERNEL_VERSION" ]; then
    err "El arbol extraido reporta ${VERSION_DETECTADA} y se requiere ${KERNEL_VERSION}"
    exit 1
fi
log "Arbol del kernel listo en $(pwd)"

fase "4/10" "Aplicando archivos modificados de la entrega..."
log "Arbol de compilacion: $(pwd)"

cp "${KERNEL_SRC_OVERLAY}/kernel/ipc_channel.c" kernel/ipc_channel.c
cp "${KERNEL_SRC_OVERLAY}/kernel/log_watch.c"   kernel/log_watch.c

for fuente in kernel/ipc_channel.c kernel/log_watch.c; do
    if [ ! -s "$fuente" ]; then
        err "El archivo ${fuente} no llego al arbol de compilacion o esta vacio."
        err "Sin el, la compilacion falla con 'No hay ninguna regla para construir'."
        exit 1
    fi
done
log "Fuentes verificadas: $(wc -l < kernel/ipc_channel.c) y $(wc -l < kernel/log_watch.c) lineas."

grep -q "ipc_channel.o" kernel/Makefile || echo "obj-y += ipc_channel.o" >> kernel/Makefile
grep -q "log_watch.o"   kernel/Makefile || echo "obj-y += log_watch.o"   >> kernel/Makefile

TABLA_SYSCALLS="arch/x86/entry/syscalls/syscall_64.tbl"
registrar_syscall() {
    local numero="$1" nombre="$2" simbolo="$3"
    if grep -q "	${nombre}	" "$TABLA_SYSCALLS"; then
        return 0
    fi
    if grep -qP "^${numero}\t" "$TABLA_SYSCALLS"; then
        err "El numero de syscall ${numero} ya esta ocupado en ${TABLA_SYSCALLS}"
        exit 1
    fi
    printf '%s\tcommon\t%s\t%s\n' "$numero" "$nombre" "$simbolo" >> "$TABLA_SYSCALLS"
}
registrar_syscall "$SYS_IPC_SEND"    "ipc_channel_send"    "sys_ipc_channel_send"
registrar_syscall "$SYS_IPC_RECEIVE" "ipc_channel_receive" "sys_ipc_channel_receive"
registrar_syscall "$SYS_START_WATCH" "start_log_watch"     "sys_start_log_watch"
registrar_syscall "$SYS_STOP_WATCH"  "stop_log_watch"      "sys_stop_log_watch"

if ! grep -q "sys_ipc_channel_send" include/linux/syscalls.h; then
    sed -i '/asmlinkage long sys_getpid(void);/a asmlinkage long sys_ipc_channel_send(const char __user *mensaje);\nasmlinkage long sys_ipc_channel_receive(char __user *buffer);\nasmlinkage long sys_start_log_watch(const char __user *const __user *rutas, int cantidad, const char __user *ruta_log_central, const char __user *palabra_clave);\nasmlinkage long sys_stop_log_watch(int id_monitoreo);' include/linux/syscalls.h
fi

mkdir -p "${KERNEL_SRC_OVERLAY}/arch/x86/entry/syscalls" "${KERNEL_SRC_OVERLAY}/include/linux"
cp "$TABLA_SYSCALLS"        "${KERNEL_SRC_OVERLAY}/arch/x86/entry/syscalls/syscall_64.tbl"
cp kernel/Makefile          "${KERNEL_SRC_OVERLAY}/kernel/Makefile"
cp include/linux/syscalls.h "${KERNEL_SRC_OVERLAY}/include/linux/syscalls.h"
log "Syscalls ${SYS_IPC_SEND}-${SYS_STOP_WATCH} registradas y overlay sincronizado."

fase "5/10" "Generando configuracion del kernel..."
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

warn "Asegurate de tener conectado y activo todo el hardware (wifi, USB, audio)."
warn "localmodconfig conserva unicamente los modulos cargados en este momento."

lsmod > /tmp/modulos-activos.txt
MODULOS_CARGADOS=$(($(wc -l < /tmp/modulos-activos.txt) - 1))
MODULOS_ANTES=$(grep -c '=m$' .config || true)

set +o pipefail
yes '' | make LSMOD=/tmp/modulos-activos.txt localmodconfig >/dev/null 2>&1 || true
set -o pipefail

MODULOS_DESPUES=$(grep -c '=m$' .config || true)
echo "Modulos cargados en el sistema : ${MODULOS_CARGADOS}"
echo "Modulos en .config antes       : ${MODULOS_ANTES}"
echo "Modulos en .config despues     : ${MODULOS_DESPUES}"

if [ "$MODULOS_DESPUES" -gt $((MODULOS_CARGADOS * 4 + 200)) ]; then
    err "localmodconfig no surtio efecto: ${MODULOS_DESPUES} modulos para ${MODULOS_CARGADOS} cargados."
    err "Compilar en este estado tarda horas en lugar de minutos. Ejecuta manualmente:"
    err "  cd $(pwd) && yes '' | make LSMOD=/tmp/modulos-activos.txt localmodconfig"
    exit 1
fi
log "Configuracion reducida de ${MODULOS_ANTES} a ${MODULOS_DESPUES} modulos."

for opcion in BLK_DEV_NVME SATA_AHCI ATA_PIIX USB_XHCI_HCD USB_EHCI_HCD \
              HID_GENERIC USB_HID EXT4_FS BTRFS_FS VFAT_FS BLK_DEV_INITRD \
              DRM DRM_FBDEV_EMULATION DRM_SIMPLEDRM SYSFB_SIMPLEFB \
              FB FB_EFI FRAMEBUFFER_CONSOLE; do
    scripts/config --enable "$opcion"
done

GPU_DETECTADA=$(lspci | grep -iE "VGA|3D controller" || true)
echo "$GPU_DETECTADA" | grep -qi nvidia && scripts/config --enable DRM_NOUVEAU
if echo "$GPU_DETECTADA" | grep -qiE "\bAMD\b|\bATI\b|Radeon"; then
    scripts/config --enable DRM_AMDGPU
    scripts/config --enable DRM_RADEON
fi
echo "$GPU_DETECTADA" | grep -qi intel && scripts/config --enable DRM_I915

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

MODULOS_FINALES=$(grep -c '=m$' .config || true)
if [ "$MODULOS_FINALES" -gt $((MODULOS_CARGADOS * 4 + 200)) ]; then
    err "make olddefconfig revirtio la reduccion: ${MODULOS_FINALES} modulos activos."
    exit 1
fi
grep -q "^CONFIG_LOCALVERSION=\"${LOCALVERSION_STR}\"" .config || {
    err "LOCALVERSION no quedo fijado como ${LOCALVERSION_STR}"
    exit 1
}
grep -q "^CONFIG_DEBUG_INFO_NONE=y" .config || {
    err "DEBUG_INFO_NONE no quedo activo, la compilacion sera mucho mas lenta."
    exit 1
}
log "Configuracion consolidada: ${MODULOS_FINALES} modulos, LOCALVERSION ${LOCALVERSION_STR}"

NUCLEOS=$(nproc)
fase "6/10" "Compilando el kernel con ${NUCLEOS} nucleos..."
time make -j"$NUCLEOS" 2>&1 | tee "${WORKSPACE_DIR}/compilacion.log"
[ -f arch/x86/boot/bzImage ] || { err "No se genero arch/x86/boot/bzImage"; exit 1; }
log "Compilacion exitosa: $(ls -lh arch/x86/boot/bzImage | awk '{print $5}')"

fase "7/10" "Instalando modulos y kernel..."
sudo make modules_install
if command -v dkms >/dev/null 2>&1 && [ -n "$(dkms status 2>/dev/null)" ]; then
    sudo dkms autoinstall -k "$FULL_KERNEL_NAME" || true
fi
sudo make install
for artefacto in vmlinuz initrd.img System.map config; do
    if [ ! -f "/boot/${artefacto}-${FULL_KERNEL_NAME}" ]; then
        if [ "$artefacto" = "initrd.img" ]; then
            sudo update-initramfs -c -k "$FULL_KERNEL_NAME"
        else
            err "Falta /boot/${artefacto}-${FULL_KERNEL_NAME}"
            exit 1
        fi
    fi
done
sudo rm -f "/boot/vmlinuz-${FULL_KERNEL_NAME}.old" "/boot/initrd.img-${FULL_KERNEL_NAME}.old" 2>/dev/null || true
log "Kernel instalado en /boot como ${FULL_KERNEL_NAME}"

fase "8/10" "Evaluando Secure Boot..."
ESTADO_SB=$(mokutil --sb-state 2>/dev/null || echo "disabled")
if echo "$ESTADO_SB" | grep -qi "enabled"; then
    warn "Secure Boot ACTIVO, se firmara la imagen del kernel."
    sudo mkdir -p "$MOK_DIR"
    if ! sudo test -f "${MOK_DIR}/MOK.priv" || ! sudo test -f "${MOK_DIR}/MOK.pem"; then
        sudo openssl req -new -x509 -newkey rsa:2048 \
            -keyout "${MOK_DIR}/MOK.priv" -out "${MOK_DIR}/MOK.pem" \
            -nodes -days 3650 -subj "/CN=SO2 Kernel ${KERNEL_VERSION} ${CARNE}/"
        sudo chmod 600 "${MOK_DIR}/MOK.priv"
        sudo openssl x509 -in "${MOK_DIR}/MOK.pem" -outform DER -out "${MOK_DIR}/MOK.der"
    fi
    if sudo mokutil --list-enrolled 2>/dev/null | grep -q "SO2 Kernel"; then
        log "Clave MOK ya enrolada en el firmware."
    else
        warn "Inscribiendo la clave MOK (se pedira una contrasena temporal)."
        sudo mokutil --import "${MOK_DIR}/MOK.der"
    fi
    sudo sbsign --key "${MOK_DIR}/MOK.priv" --cert "${MOK_DIR}/MOK.pem" \
        --output /tmp/vmlinuz-firmado "/boot/vmlinuz-${FULL_KERNEL_NAME}"
    sudo sbverify --list /tmp/vmlinuz-firmado || { err "Fallo la verificacion de firma"; exit 1; }
    sudo mv /tmp/vmlinuz-firmado "/boot/vmlinuz-${FULL_KERNEL_NAME}"
    sudo chmod 644 "/boot/vmlinuz-${FULL_KERNEL_NAME}"
    log "Kernel firmado y verificado."
else
    log "Secure Boot desactivado, no se requiere firma."
fi
sudo update-grub

fase "9/10" "Compilando programas de espacio de usuario..."
compilar_programas

fase "10/10" "Preparando archivos de log de demostracion..."
preparar_logs_demo

echo -e "\n${GREEN}====================================================================${NC}"
echo -e "${GREEN}   PREPARACION COMPLETADA${NC}"
echo -e "${GREEN}====================================================================${NC}"
echo "Kernel instalado  : ${FULL_KERNEL_NAME}"
echo "Syscalls          : ${SYS_IPC_SEND} ipc_channel_send, ${SYS_IPC_RECEIVE} ipc_channel_receive,"
echo "                    ${SYS_START_WATCH} start_log_watch, ${SYS_STOP_WATCH} stop_log_watch"
echo "Programas         : ${PROGRAMA_DIR}"
echo "Logs de prueba    : ${LOGS_DEMO_DIR}"
echo "Log de compilacion: ${WORKSPACE_DIR}/compilacion.log"
echo "NIGGGGGGEEEEEEEEEEEEEEEEEEEEERSSSSSSSSSSSSSSSSSSSSSSS"
echo
echo "Reinicia y elige ${FULL_KERNEL_NAME} en GRUB, luego ejecuta:"
echo "  uname -r"
echo "  cd ${PROGRAMA_DIR} && ./test_ipc_channel_receive"
