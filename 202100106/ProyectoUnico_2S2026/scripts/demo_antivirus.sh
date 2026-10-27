#!/usr/bin/env bash
set -euo pipefail

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DESTINO="${1:-${USER_HOME}/so2_vigilado}"

if [ ! -d "$DESTINO" ]; then
	if [ -n "${SUDO_USER:-}" ]; then
		sudo -u "$REAL_USER" mkdir -p "$DESTINO"
	else
		mkdir -p "$DESTINO"
	fi
fi

crear_muestra() {
	local nombre="$1"
	local contenido="$2"
	local ruta="${DESTINO}/${nombre}"

	if [ -e "$ruta" ]; then
		echo "Ya existe: ${ruta}"
	else
		if [ -n "${SUDO_USER:-}" ]; then
			sudo -u "$REAL_USER" bash -c "printf '%b' '$contenido' > '$ruta'"
		else
			printf "%b" "$contenido" > "$ruta"
		fi
		echo "Creado: ${ruta}"
	fi
	sha256sum "$ruta" 2>/dev/null || echo "(en cuarentena) ${ruta}"
}

echo "=== Muestras de demostración antivirus ==="
echo "Directorio de destino: ${DESTINO}"
echo ""

crear_muestra "troyano.txt" "SO2-MUESTRA-MALICIOSA-1\n"
crear_muestra "ransomware.txt" "SO2-MUESTRA-MALICIOSA-2\n"
crear_muestra "keylogger.txt" "SO2-MUESTRA-MALICIOSA-3\n"
crear_muestra "notas.txt" "archivo benigno de notas personales\n"

echo ""
echo "Demostración lista."
