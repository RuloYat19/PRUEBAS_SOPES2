#!/usr/bin/env bash
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "Ejecuta con sudo" >&2
	exit 1
fi

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DAEMON="${REPO_DIR}/ProgramaIntermedio/daemon_monitor"
DASHBOARD_DIR="${REPO_DIR}/Dashboard"
PUERTO=8097
BASE="http://127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR="${USER_HOME}/so2_logs/pruebas_alertas"
PALABRA="ALERTA_TEST"

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE=""
TEMP_FILES=()

aprobar() { echo -e "  ${GREEN}✓ $*${NC}"; APROBADAS=$((APROBADAS + 1)); }
fallar()  { echo -e "  ${RED}✗ $*${NC}"; FALLIDAS=$((FALLIDAS + 1)); }
comprobar() {
	local descripcion="$1"; shift
	if "$@" >/dev/null 2>&1; then aprobar "$descripcion"; else fallar "$descripcion"; fi
}

mktemp_rastreado() {
	local archivo
	archivo=$(mktemp)
	TEMP_FILES+=("$archivo")
	echo "$archivo"
}

limpiar() {
	if [ -n "$PID_SSE" ]; then
		kill "$PID_SSE" 2>/dev/null || true
		wait "$PID_SSE" 2>/dev/null || true
	fi
	if [ -n "$PID_DAEMON" ]; then
		kill -TERM "$PID_DAEMON" 2>/dev/null || true
		wait "$PID_DAEMON" 2>/dev/null || true
	fi
	usuarios_prueba_borrar
	for f in "${TEMP_FILES[@]}"; do
		[ -f "$f" ] && rm -f "$f"
	done
	chown -R "$REAL_USER:$REAL_USER" "$DIR" 2>/dev/null || true
}
trap limpiar EXIT

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}       PRUEBAS DE MOTOR DE ALERTAS - PROYECTO UNICO SO2            ${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/12" "Condiciones previas y preparación de usuarios"
if [[ "$(uname -r)" != *so2-202100106* ]]; then
	fallar "Se requiere kernel 6.12.69-so2-202100106 (actual: $(uname -r))"
	exit 1
fi
aprobar "Kernel en ejecución: $(uname -r)"

if ! usuarios_prueba_condiciones; then
	fallar "Grupos o PAM no configurados"
	exit 1
fi

usuarios_prueba_crear
aprobar "Usuarios de prueba creados (so2t_admin, so2t_comun, so2t_ambos, so2t_nadie)"

mkdir -p "$DIR"
mkdir -p "${DIR}/vigilado"
: > "${DIR}/a.log"; : > "${DIR}/b.log"; : > "${DIR}/central.txt"
cat << EOF > "${DIR}/daemon.conf"
puerto = ${PUERTO}
intervalo = 2
dashboard = ${DASHBOARD_DIR}
log_central = ${DIR}/central.txt
log_rutas = ${DIR}/a.log, ${DIR}/b.log
log_palabras = ${PALABRA}
ruta_vigilada = ${DIR}/vigilado
blacklist = ${REPO_DIR}/ProgramaIntermedio/blacklist.json
EOF

"${DAEMON}" -c "${DIR}/daemon.conf" > "${DIR}/daemon.out" 2>&1 &
PID_DAEMON=$!
sleep 1
if ! kill -0 "$PID_DAEMON" 2>/dev/null; then
	fallar "El daemon no pudo arrancar en puerto ${PUERTO}"
	exit 1
fi
for _ in $(seq 50); do
	[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/login.html")" = "200" ] && break
	sleep 0.1
done
aprobar "Daemon iniciado en puerto ${PUERTO} (PID ${PID_DAEMON})"

fase "1/12" "GET /api/alertas sin cookie"
c1=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/api/alertas")
if [ "$c1" = "401" ]; then
	aprobar "Rechaza sin cookie con 401"
else
	fallar "Respondio ${c1} en lugar de 401"
fi

fase "2/12" "Login so2t_comun y consulta de /api/alertas"
COOKIE_COMUN=$(mktemp_rastreado)
resp_login=$(login_json "so2t_comun" | curl -s -c "$COOKIE_COMUN" -H "Content-Type: application/json" -d @- -w '%{http_code}' -o /dev/null "${BASE}/api/login")
resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
codigo_alertas=$(curl -s -b "$COOKIE_COMUN" -o /dev/null -w '%{http_code}' "${BASE}/api/alertas")
if [ "$resp_login" = "200" ] && [ "$codigo_alertas" = "200" ] && grep -q '"alertas":\[' <<< "$resp_alertas"; then
	aprobar "Login exitoso y /api/alertas responde 200 con formato JSON valido"
else
	fallar "Fallo en login (${resp_login}) o en alertas (${codigo_alertas})"
fi

fase "3/12" "Evento SSE al iniciar sesion so2t_admin"
SSE_OUT=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_OUT" &
PID_SSE=$!
sleep 0.5
COOKIE_ADMIN=$(mktemp_rastreado)
t_inicio=$(date +%s%N)
login_json "so2t_admin" | curl -s -c "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/login" >/dev/null
t_fin=$((t_inicio + 1000000000))
sse_recibido=0
while [ "$(date +%s%N)" -lt "$t_fin" ]; do
	if grep -q "event: alerta" "$SSE_OUT" && grep -q "Inicio de sesión exitoso" "$SSE_OUT"; then
		sse_recibido=1
		break
	fi
	sleep 0.05
done
kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""
if [ "$sse_recibido" -eq 1 ] && grep -q '"severidad":"LOW"' "$SSE_OUT" && grep -q '"tipo":"sesion"' "$SSE_OUT"; then
	aprobar "Llega event: alerta con Inicio de sesión exitoso, LOW, sesion en < 1 s"
else
	fallar "No se recibio la alerta SSE esperada dentro del tiempo limite"
fi

fase "4/12" "Alerta por credenciales inválidas"
printf '{"usuario":"so2t_admin","password":"clave_incorrecta_prueba"}' | curl -s -H "Content-Type: application/json" -d @- "${BASE}/api/login" >/dev/null
resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
if python3 -I -c '
import sys, json
data = json.load(sys.stdin)
encontrada = any(
	a.get("motivo") == "Inicio de sesión fallido" and
	a.get("severidad") == "MEDIUM" and
	"Credenciales inválidas para el usuario so2t_admin" in a.get("descripcion", "")
	for a in data.get("alertas", [])
)
sys.exit(0 if encontrada else 1)
' <<< "$resp_alertas"; then
	aprobar "Alerta generada: Inicio de sesión fallido MEDIUM por credenciales inválidas"
else
	fallar "No se encontró la alerta de credenciales inválidas esperada"
fi

fase "5/12" "Alerta por usuario sin grupo autorizado"
login_json "so2t_nadie" | curl -s -H "Content-Type: application/json" -d @- "${BASE}/api/login" >/dev/null
resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
if python3 -I -c '
import sys, json
data = json.load(sys.stdin)
encontrada = any(
	a.get("motivo") == "Inicio de sesión fallido" and
	a.get("severidad") == "MEDIUM" and
	"no pertenece a admin_user ni common_user" in a.get("descripcion", "")
	for a in data.get("alertas", [])
)
sys.exit(0 if encontrada else 1)
' <<< "$resp_alertas"; then
	aprobar "Alerta generada: Inicio de sesión fallido MEDIUM por falta de grupo autorizado"
else
	fallar "No se encontró la alerta de usuario no autorizado esperada"
fi

fase "6/12" "Login con JSON inválido no genera alerta"
max_id_antes=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -I -c '
import sys, json
alertas = json.load(sys.stdin).get("alertas", [])
print(alertas[0]["id"] if alertas else 0)
')
c_bad=$(printf '{mal json' | curl -s -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/login")
max_id_despues=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -I -c '
import sys, json
alertas = json.load(sys.stdin).get("alertas", [])
print(alertas[0]["id"] if alertas else 0)
')
if [ "$c_bad" = "400" ] && [ "$max_id_antes" = "$max_id_despues" ]; then
	aprobar "JSON inválido devuelve 400 y el id máximo de alertas se mantiene en ${max_id_antes}"
else
	fallar "JSON inválido causó comportamiento inesperado (codigo=${c_bad}, antes=${max_id_antes}, despues=${max_id_despues})"
fi

fase "7/12" "Alerta por palabra clave en log vigilado"
SSE_LOGS=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_LOGS" &
PID_SSE=$!
sleep 0.5
linea_log="${PALABRA} intruso detectado en sesion de prueba"
echo "$linea_log" >> "${DIR}/a.log"
t_fin=$(( $(date +%s) + 5 ))
sse_log_ok=0
while [ "$(date +%s)" -lt "$t_fin" ]; do
	if grep -q "Palabra clave detectada en log" "$SSE_LOGS" 2>/dev/null; then
		sse_log_ok=1
		break
	fi
	sleep 0.2
done
kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""
resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
if [ "$sse_log_ok" -eq 1 ] && python3 -I -c '
import sys, json
data = json.load(sys.stdin)
encontrada = any(
	a.get("tipo") == "logs" and
	a.get("motivo") == "Palabra clave detectada en log" and
	a.get("severidad") == "MEDIUM" and
	sys.argv[1] in a.get("archivo_origen", "") and
	a.get("palabra_detectada") == sys.argv[2] and
	sys.argv[3] in a.get("contenido", "")
	for a in data.get("alertas", [])
)
sys.exit(0 if encontrada else 1)
' "${DIR}/a.log" "$PALABRA" "$linea_log" <<< "$resp_alertas"; then
	aprobar "Alerta de log recibida por SSE y registrada en /api/alertas con todos sus campos"
else
	fallar "No se recibió o no coincidió la alerta de log esperada"
fi

fase "8/12" "Orden de /api/alertas"
resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
if python3 -I -c '
import sys, json
alertas = json.load(sys.stdin).get("alertas", [])
ids = [a["id"] for a in alertas]
if len(ids) < 2:
	sys.exit(1)
es_descendente = all(ids[i] > ids[i + 1] for i in range(len(ids) - 1))
sys.exit(0 if es_descendente else 1)
' <<< "$resp_alertas"; then
	aprobar "Los ids en /api/alertas estan ordenados estrictamente de mayor a menor"
else
	fallar "Los ids en /api/alertas no tienen orden estrictamente descendente"
fi

fase "9/12" "Capacidad del historial circular (210 logins fallidos)"
for _ in $(seq 1 210); do
	printf '{"usuario":"so2t_admin","password":"err"}' | curl -s -o /dev/null -H "Content-Type: application/json" -d @- "${BASE}/api/login"
done
resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
if python3 -I -c '
import sys, json
alertas = json.load(sys.stdin).get("alertas", [])
if len(alertas) != 200:
	sys.exit(1)
ids = [a["id"] for a in alertas]
primer_id = ids[0]
ultimo_id = ids[-1]
if not all(ids[i] > ids[i + 1] for i in range(len(ids) - 1)):
	sys.exit(2)
if ultimo_id != primer_id - 199:
	sys.exit(3)
sys.exit(0)
' <<< "$resp_alertas"; then
	aprobar "/api/alertas devuelve exactamente 200 alertas con el id maximo primero"
else
	fallar "El historial circular no acoto a 200 alertas o los ids no son congruentes"
fi

fase "10/12" "Verificar remoción de eventos de seguridad en /api/estado"
resp_estado=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/estado")
clave_obsoleta="eventos_""seguridad"
if ! grep -q "$clave_obsoleta" <<< "$resp_estado"; then
	aprobar "/api/estado no contiene la clave obsoleta de eventos"
else
	fallar "/api/estado todavia incluye la clave obsoleta de eventos"
fi

fase "11/12" "Auditoría de logs del daemon"
log_daemon="${DIR}/daemon.out"
pwd_encontrada=0
for pass in "${CLAVE[@]}"; do
	if [ -n "$pass" ] && grep -Fq "$pass" "$log_daemon" 2>/dev/null; then
		pwd_encontrada=1
		break
	fi
done
if grep -Fq "clave_incorrecta_prueba" "$log_daemon" 2>/dev/null; then
	pwd_encontrada=1
fi
if grep -q "ALERTA \[MEDIUM\]" "$log_daemon" 2>/dev/null && [ "$pwd_encontrada" -eq 0 ]; then
	aprobar "Salida del daemon contiene ALERTA [MEDIUM] y ninguna contraseña de prueba"
else
	fallar "Fallo en auditoria de salida del daemon (pwd_encontrada=${pwd_encontrada})"
fi

fase "12/12" "Detención limpia con SIGTERM"
if kill -0 "$PID_DAEMON" 2>/dev/null; then
	kill -TERM "$PID_DAEMON"
	wait "$PID_DAEMON"
	codigo_salida=$?
	PID_DAEMON=""
	if [ "$codigo_salida" -eq 0 ]; then
		aprobar "Daemon se detiene limpiamente con código 0"
	else
		fallar "Daemon terminó con código ${codigo_salida}"
	fi
else
	fallar "Daemon se cayó antes de recibir SIGTERM"
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "   RESULTADO FINAL: ${GREEN}${APROBADAS} APROBADAS${NC}, ${RED}${FALLIDAS} FALLIDAS${NC}"
echo -e "${BLUE}====================================================================${NC}"

[ "$FALLIDAS" -eq 0 ]
