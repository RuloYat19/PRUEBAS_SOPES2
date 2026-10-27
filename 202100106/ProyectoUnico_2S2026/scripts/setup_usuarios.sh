#!/usr/bin/env bash
set -euo pipefail

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}! $*${NC}"; }
err()  { echo -e "${RED}✗ $*${NC}" >&2; }

if [ "$(id -u)" -ne 0 ]; then
	echo "Ejecuta con sudo" >&2
	exit 1
fi

configurar_base() {
	if [ ! -f /usr/include/security/pam_appl.h ]; then
		log "Instalando libpam0g-dev..."
		apt-get update -y
		apt-get install -y libpam0g-dev
	fi

	getent group admin_user >/dev/null || groupadd admin_user
	getent group common_user >/dev/null || groupadd common_user

	cat << 'EOF' > /etc/pam.d/so2-monitor
auth    required pam_unix.so
account required pam_unix.so
EOF
	chmod 644 /etc/pam.d/so2-monitor
}

if [ "$#" -eq 0 ]; then
	configurar_base
	log "Configuración base de PAM y grupos completada."
	exit 0
fi

subcomando="$1"
case "$subcomando" in
	agregar)
		if [ "$#" -ne 3 ]; then
			err "Uso: $0 agregar <usuario> <admin|comun>"
			exit 1
		fi
		configurar_base
		usuario="$2"
		tipo="$3"
		case "$tipo" in
			admin)
				grupo="admin_user"
				;;
			comun)
				grupo="common_user"
				;;
			*)
				err "Tipo de usuario desconocido: $tipo (usa 'admin' o 'comun')"
				exit 1
				;;
		esac

		if ! id "$usuario" >/dev/null 2>&1; then
			useradd -m -s /bin/bash "$usuario"
		else
			warn "El usuario $usuario ya existe, actualizando grupo..."
		fi
		usermod -aG "$grupo" "$usuario"
		log "Asignando contraseña para $usuario..."
		passwd "$usuario"
		log "Usuario $usuario configurado exitosamente en el grupo $grupo."
		;;
	*)
		err "Uso: $0 [agregar <usuario> <admin|comun>]"
		exit 1
		;;
esac
