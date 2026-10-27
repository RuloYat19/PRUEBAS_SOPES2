#!/usr/bin/env bash

declare -g -A CLAVE=()
declare -g -a USUARIOS_CREADOS=()

usuarios_prueba_condiciones() {
	if [ ! -f /etc/pam.d/so2-monitor ] || ! getent group admin_user >/dev/null || ! getent group common_user >/dev/null; then
		echo "Falta configurar PAM o grupos: ejecuta sudo scripts/setup_usuarios.sh" >&2
		return 1
	fi
	return 0
}

usuarios_prueba_agregar() {
	local usuario="$1"
	local grupo="${2:--}"
	local pass

	if [[ "$usuario" != so2t_* ]]; then
		echo "Nombre de usuario inválido: debe iniciar con so2t_" >&2
		return 1
	fi

	id "$usuario" >/dev/null 2>&1 && userdel -f "$usuario" >/dev/null 2>&1 || true
	useradd -M -s /usr/sbin/nologin "$usuario"

	if [ "$grupo" = "ambos" ]; then
		usermod -aG admin_user,common_user "$usuario"
	elif [ "$grupo" != "-" ] && [ -n "$grupo" ]; then
		usermod -aG "$grupo" "$usuario"
	fi

	pass=$(head -c 18 /dev/urandom | base64 | tr -d '/+=')
	CLAVE["$usuario"]="$pass"
	echo "${usuario}:${pass}" | chpasswd
	USUARIOS_CREADOS+=("$usuario")
}

usuarios_prueba_crear() {
	usuarios_prueba_condiciones || return 1
	usuarios_prueba_agregar "so2t_admin" "admin_user"
	usuarios_prueba_agregar "so2t_comun" "common_user"
	usuarios_prueba_agregar "so2t_ambos" "ambos"
	usuarios_prueba_agregar "so2t_nadie" "-"
}

usuarios_prueba_borrar() {
	for u in "${USUARIOS_CREADOS[@]}"; do
		if [[ "$u" == so2t_* ]]; then
			id "$u" >/dev/null 2>&1 && userdel -f "$u" >/dev/null 2>&1 || true
			unset "CLAVE[$u]"
		fi
	done
	USUARIOS_CREADOS=()
}

login_json() {
	local usuario="$1"
	local pass="${CLAVE[$usuario]:-}"
	printf '{"usuario":"%s","password":"%s"}' "$usuario" "$pass"
}
