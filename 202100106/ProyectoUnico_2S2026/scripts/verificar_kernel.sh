#!/usr/bin/env bash
set -u

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
log_ok()   { echo -e "${GREEN}✓ $1${NC}"; }
log_fail() { echo -e "${RED}✗ $1${NC}"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FALLOS=0

KERNEL_ESPERADO="6.12.69-so2-202100106"
KERNEL_ACTUAL="$(uname -r)"
if [ "$KERNEL_ACTUAL" = "$KERNEL_ESPERADO" ]; then
    log_ok "Version de kernel: ${KERNEL_ACTUAL}"
else
    log_fail "Version de kernel: esperado ${KERNEL_ESPERADO}, actual ${KERNEL_ACTUAL}"
    FALLOS=$((FALLOS + 1))
fi

BTF_VMLINUX="/sys/kernel/btf/vmlinux"
if [ -r "$BTF_VMLINUX" ]; then
    log_ok "BTF vmlinux: ${BTF_VMLINUX} existe y es legible"
else
    log_fail "BTF vmlinux: ${BTF_VMLINUX} no encontrado o ilegible"
    FALLOS=$((FALLOS + 1))
fi

CONFIG_BOOT="/boot/config-${KERNEL_ACTUAL}"
OPCIONES_EBPF=(CONFIG_BPF CONFIG_BPF_SYSCALL CONFIG_BPF_EVENTS CONFIG_DEBUG_INFO_BTF CONFIG_FTRACE_SYSCALLS)
if [ -r "$CONFIG_BOOT" ]; then
    OPCIONES_FALTANTES=()
    for opt in "${OPCIONES_EBPF[@]}"; do
        if ! grep -q "^${opt}=y$" "$CONFIG_BOOT"; then
            OPCIONES_FALTANTES+=("$opt")
        fi
    done
    if [ ${#OPCIONES_FALTANTES[@]} -eq 0 ]; then
        log_ok "Configuracion eBPF/BTF en ${CONFIG_BOOT}: todas las opciones =y"
    else
        log_fail "Configuracion eBPF/BTF en ${CONFIG_BOOT}: faltan ${OPCIONES_FALTANTES[*]}"
        FALLOS=$((FALLOS + 1))
    fi
else
    log_fail "Archivo de configuracion no legible: ${CONFIG_BOOT}"
    FALLOS=$((FALLOS + 1))
fi

BIN_TEST="${SCRIPT_DIR}/test_syscalls"
if [ ! -x "$BIN_TEST" ]; then
    gcc -O2 -Wall -Wextra -o "$BIN_TEST" "${SCRIPT_DIR}/test_syscalls.c" 2>/dev/null || true
fi

if [ -x "$BIN_TEST" ]; then
    if "$BIN_TEST" >/dev/null 2>&1; then
        log_ok "Syscalls 548-553: validacion exitosa"
    else
        log_fail "Syscalls 548-553: validacion reporto errores"
        FALLOS=$((FALLOS + 1))
    fi
else
    log_fail "Syscalls 548-553: no se pudo compilar ${BIN_TEST}"
    FALLOS=$((FALLOS + 1))
fi

if [ "$FALLOS" -eq 0 ]; then
    exit 0
else
    exit 1
fi
