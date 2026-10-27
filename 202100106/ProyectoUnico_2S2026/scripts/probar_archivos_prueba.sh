#!/usr/bin/env bash
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "Ejecuta con sudo" >&2
	exit 1
fi

if ! command -v bpftool >/dev/null 2>&1 || ! command -v sha256sum >/dev/null 2>&1; then
	echo "Se requieren bpftool y sha256sum instalados" >&2
	exit 1
fi

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DAEMON="${REPO_DIR}/ProgramaIntermedio/daemon_monitor"
DASHBOARD_DIR="${REPO_DIR}/Dashboard"
ARCHIVOS_PRUEBA_DIR="${REPO_DIR}/archivos_prueba"
PUERTO=8094
SLASH="/"
BASE="http:${SLASH}${SLASH}127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR_VIGILADO="${USER_HOME}/so2_vigilado/pruebas_archivos"
DIR_LOGS="${USER_HOME}/so2_logs/pruebas_archivos"

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
TEMP_FILES=()

aprobar() {
	printf "  ${GREEN}✓${NC} %s\n" "$*"
	APROBADAS=$((APROBADAS + 1))
}

fallar() {
	printf "  ${RED}✗${NC} %s\n" "$*"
	FALLIDAS=$((FALLIDAS + 1))
}

mktemp_rastreado() {
	local archivo
	archivo=$(mktemp)
	TEMP_FILES+=("$archivo")
	echo "$archivo"
}

limpiar() {
	set +e
	if [ -n "$PID_DAEMON" ] && kill -0 "$PID_DAEMON" 2>/dev/null; then
		kill -TERM "$PID_DAEMON" 2>/dev/null
		wait "$PID_DAEMON" 2>/dev/null
	fi
	for f in "${TEMP_FILES[@]}"; do
		[ -f "$f" ] && rm -f "$f"
	done
	if [ -d "$DIR_VIGILADO" ]; then
		chmod -R 777 "$DIR_VIGILADO" 2>/dev/null
		rm -rf "$DIR_VIGILADO"
	fi
	usuarios_prueba_borrar
}
trap limpiar EXIT

login_stdin() {
	local usuario="$1"
	local pass="${CLAVE[$usuario]}"
	local cookie_file="$2"
	printf '{"usuario":"%s","password":"%s"}' "$usuario" "$pass" | \
		curl -s -X POST "${BASE}/api/login" \
		-H "Content-Type: application/json" \
		--data-binary @- -c "$cookie_file" > /dev/null
}

echo "===================================================================="
echo "   PRUEBAS DE VALIDACIÓN FINAL CON ARCHIVOS_PRUEBA (SO2)            "
echo "===================================================================="

fase "0/5" "Verificación de entorno y archivos compartidos"
if [ ! -d "$ARCHIVOS_PRUEBA_DIR" ]; then
	echo "No se encontró el directorio archivos_prueba en $ARCHIVOS_PRUEBA_DIR" >&2
	exit 1
fi

TOTAL_TXT=$(find "$ARCHIVOS_PRUEBA_DIR" -name "*.txt" | wc -l)
if [ "$TOTAL_TXT" -lt 6 ] || [ ! -f "${ARCHIVOS_PRUEBA_DIR}/blacklist.json" ]; then
	echo "archivos_prueba debe contener al menos 6 archivos .txt y blacklist.json" >&2
	exit 1
fi
aprobar "Archivos de prueba encontrados: $TOTAL_TXT muestras .txt y blacklist.json"

usuarios_prueba_crear
mkdir -p "$DIR_VIGILADO" "$DIR_LOGS"
chown -R "${REAL_USER}:${REAL_USER}" "$DIR_VIGILADO" "$DIR_LOGS"
chmod 755 "$DIR_VIGILADO" "$DIR_LOGS"

DAEMON_CONF="${DIR_LOGS}/daemon.conf"
cat > "$DAEMON_CONF" <<EOF
puerto = ${PUERTO}
dashboard = ${DASHBOARD_DIR}
ruta_vigilada = ${DIR_VIGILADO}
blacklist = ${ARCHIVOS_PRUEBA_DIR}/blacklist.json
intervalo = 1
EOF
chown "${REAL_USER}:${REAL_USER}" "$DAEMON_CONF"

fase "1/5" "Arranque del daemon con blacklist de archivos_prueba"
"$DAEMON" -c "$DAEMON_CONF" > "${DIR_LOGS}/daemon.txt" 2>&1 &
PID_DAEMON=$!

LISTO=0
for _ in $(seq 1 40); do
	if curl -s -f "${BASE}/" >/dev/null 2>&1; then
		LISTO=1
		break
	fi
	sleep 0.2
done

if [ "$LISTO" -eq 1 ]; then
	aprobar "Daemon iniciado correctamente en puerto ${PUERTO} (PID ${PID_DAEMON})"
else
	fallar "El daemon no respondió en el puerto ${PUERTO}"
	exit 1
fi

COOKIE_ADMIN=$(mktemp_rastreado)
COOKIE_COMUN=$(mktemp_rastreado)
login_stdin "so2t_admin" "$COOKIE_ADMIN"
login_stdin "so2t_comun" "$COOKIE_COMUN"

if [ -s "$COOKIE_ADMIN" ] && [ -s "$COOKIE_COMUN" ]; then
	aprobar "Sesiones de prueba iniciadas (admin y común)"
else
	fallar "No se pudo iniciar sesión con los usuarios de prueba"
	exit 1
fi

fase "2/5" "Consulta de la base de firmas cargada"
RESP_BL=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/blacklist")
FIRMAS_CANT=$(echo "$RESP_BL" | python3 -c 'import sys, json; d=json.load(sys.stdin); print(d.get("firmas", 0))')

if [ "$FIRMAS_CANT" -eq 6 ]; then
	aprobar "GET /api/blacklist respondió con exactamente 6 firmas cargadas desde archivos_prueba"
else
	fallar "GET /api/blacklist reportó ${FIRMAS_CANT} firmas (se esperaban 6)"
fi

fase "3/5" "Inyección de las 6 muestras y verificación de cuarentena automática"
MUESTRAS=(
	"Simulated.Adware.Popup"
	"Simulated.Keylogger.Basic"
	"Simulated.Ransomware.Encryptor"
	"Simulated.Spyware.Tracker"
	"Simulated.Backdoor.Listener"
	"Simulated.Worm.Spreader"
)

TODAS_CUARENTENA=1
for M in "${MUESTRAS[@]}"; do
	SRC="${ARCHIVOS_PRUEBA_DIR}/${M}.txt"
	DST="${DIR_VIGILADO}/${M}.txt"
	sudo -u "$REAL_USER" sh -c "cat '$SRC' > '$DST'"

	DETECTADO=0
	for _ in $(seq 1 20); do
		PERM=$(stat -c "%a" "$DST" 2>/dev/null || echo "error")
		if [ "$PERM" = "0" ]; then
			DETECTADO=1
			break
		fi
		sleep 0.2
	done

	if [ "$DETECTADO" -eq 1 ]; then
		aprobar "Muestra ${M}.txt detectada y aislada con permisos 0000"
	else
		fallar "Muestra ${M}.txt no fue puesta en cuarentena (permisos: $(stat -c "%a" "$DST" 2>/dev/null))"
		TODAS_CUARENTENA=0
	fi

	if sudo -u "$REAL_USER" cat "$DST" >/dev/null 2>&1; then
		fallar "El archivo ${M}.txt aún puede ser leído por el usuario"
	else
		aprobar "Acceso de lectura a ${M}.txt bloqueado por cuarentena"
	fi
done

fase "4/5" "Verificación de API: amenazas, cuarentena y alertas"
RESP_AMENAZAS=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/amenazas")
TOTAL_AMENAZAS=$(echo "$RESP_AMENAZAS" | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
    print(len(d.get("amenazas", [])))
except Exception:
    print(0)
')

if [ "$TOTAL_AMENAZAS" -eq 6 ]; then
	aprobar "GET /api/amenazas contiene las 6 amenazas detectadas"
else
	fallar "GET /api/amenazas reporta ${TOTAL_AMENAZAS} amenazas (esperadas: 6)"
fi

RESP_CUAR=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/cuarentena")
TOTAL_CUAR=$(echo "$RESP_CUAR" | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
    print(len(d.get("cuarentena", [])))
except Exception:
    print(0)
')

if [ "$TOTAL_CUAR" -eq 6 ]; then
	aprobar "GET /api/cuarentena contiene los 6 registros de cuarentena automática"
else
	fallar "GET /api/cuarentena reporta ${TOTAL_CUAR} registros (esperados: 6)"
fi

RESP_ALERTAS=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
ALERTAS_HIGH=$(echo "$RESP_ALERTAS" | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
    highs = [a for a in d.get("alertas", []) if a.get("severidad") == "HIGH"]
    print(len(highs))
except Exception:
    print(0)
')

if [ "$ALERTAS_HIGH" -ge 12 ]; then
	aprobar "Alertas HIGH generadas correctamente: ${ALERTAS_HIGH} eventos (detección + cuarentena)"
else
	fallar "Alertas HIGH insuficientes: ${ALERTAS_HIGH} (se esperaban >= 12)"
fi

fase "5/5" "Actualización en caliente vía POST /api/blacklist"
RESP_POST=$(curl -s -w "\n%{http_code}" -X POST -b "$COOKIE_ADMIN" \
	-H "Content-Type: application/json" \
	--data-binary "@${ARCHIVOS_PRUEBA_DIR}/blacklist.json" \
	"${BASE}/api/blacklist")
BODY_POST=$(echo "$RESP_POST" | head -n -1)
CODE_POST=$(echo "$RESP_POST" | tail -n 1)

if [ "$CODE_POST" = "200" ]; then
	aprobar "POST /api/blacklist con el JSON de prueba respondió 200 OK"
else
	fallar "POST /api/blacklist falló con código ${CODE_POST}: ${BODY_POST}"
fi

echo -e "\n===================================================================="
echo -e "   RESULTADO: ${GREEN}${APROBADAS} aprobadas${NC}, ${RED}${FALLIDAS} fallidas${NC}"
echo -e "===================================================================="

if [ "$FALLIDAS" -eq 0 ]; then
	echo -e "${GREEN}✓ TODAS LAS PRUEBAS CON ARCHIVOS_PRUEBA PASARON EXITOSAMENTE${NC}"
	exit 0
else
	echo -e "${RED}✗ HUBO PRUEBAS FALLIDAS${NC}"
	exit 1
fi
