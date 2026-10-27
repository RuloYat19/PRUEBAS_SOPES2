#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
err()  { echo -e "${RED}✗ $*${NC}" >&2; }
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

CARNE="202100106"
KERNEL_VERSION="6.12.69"
LOCALVERSION_STR="-so2-${CARNE}"
FULL_KERNEL_NAME="${KERNEL_VERSION}${LOCALVERSION_STR}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_KERNEL="$(realpath "${SCRIPT_DIR}/../Practica_3_2S2026/so2_build_workspace/linux-6.12.69")"

if [ ! -f "${WORKSPACE_KERNEL}/arch/x86/boot/bzImage" ]; then
    err "No se encontro bzImage en ${WORKSPACE_KERNEL}."
    err "Compilalo primero con: ${SCRIPT_DIR}/preparar_kernel.sh"
    exit 1
fi

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}   INSTALADOR DE KERNEL P4 - SO2 2S2026 - CARNE ${CARNE}${NC}"
echo -e "${BLUE}   Kernel: ${FULL_KERNEL_NAME}${NC}"
echo -e "${BLUE}====================================================================${NC}"

MOK_DIR="/root/mok"

fase "1/4" "Instalando modulos del kernel en /lib/modules/${FULL_KERNEL_NAME}..."
sudo make -C "$WORKSPACE_KERNEL" modules_install
if command -v dkms >/dev/null 2>&1 && [ -n "$(dkms status 2>/dev/null)" ]; then
    sudo dkms autoinstall -k "$FULL_KERNEL_NAME" || warn "dkms no pudo reconstruir todos los modulos externos."
fi
log "Modulos instalados exitosamente."

fase "2/4" "Instalando imagen del kernel en /boot..."
sudo make -C "$WORKSPACE_KERNEL" install

if [ ! -f "/boot/initrd.img-${FULL_KERNEL_NAME}" ]; then
    warn "Generando initramfs manual para ${FULL_KERNEL_NAME}..."
    if command -v dracut >/dev/null 2>&1; then
        sudo dracut --force "/boot/initrd.img-${FULL_KERNEL_NAME}" "$FULL_KERNEL_NAME"
    else
        sudo update-initramfs -c -k "$FULL_KERNEL_NAME" || sudo update-initramfs -u -k "$FULL_KERNEL_NAME"
    fi
fi
for artefacto in vmlinuz initrd.img System.map config; do
    if [ ! -f "/boot/${artefacto}-${FULL_KERNEL_NAME}" ]; then
        err "Falta /boot/${artefacto}-${FULL_KERNEL_NAME}"
        exit 1
    fi
done
sudo rm -f "/boot/vmlinuz-${FULL_KERNEL_NAME}.old" "/boot/initrd.img-${FULL_KERNEL_NAME}.old" \
    "/boot/System.map-${FULL_KERNEL_NAME}.old" "/boot/config-${FULL_KERNEL_NAME}.old"
log "Archivos de arranque verificados en /boot."

fase "3/4" "Evaluando Secure Boot..."
if mokutil --sb-state 2>/dev/null | grep -qi "enabled"; then
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
        warn "Al reiniciar, elige 'Enroll MOK' en la pantalla azul e ingresa esa contrasena."
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

fase "4/4" "Actualizando configuracion de GRUB..."
sudo update-grub
log "GRUB actualizado correctamente."

echo -e "\n${GREEN}====================================================================${NC}"
echo -e "${GREEN}   INSTALACION COMPLETADA EXITOSAMENTE                              ${NC}"
echo -e "${GREEN}====================================================================${NC}"
echo -e "Pasos siguientes:"
echo -e " 1. Reiniciar la maquina: ${YELLOW}sudo reboot${NC}"
echo -e " 2. En el menu de GRUB, seleccionar: 'Opciones avanzadas' -> '${FULL_KERNEL_NAME}'"
echo -e " 3. Al iniciar sesion, verificar con: ${YELLOW}uname -r${NC}"
echo -e " 4. Ejecutar la demostracion: ${YELLOW}${SCRIPT_DIR}/demo.sh${NC}"
