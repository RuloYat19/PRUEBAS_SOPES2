#!/usr/bin/env bash
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "Ejecuta con sudo" >&2
	exit 1
fi

if ! command -v jq >/dev/null 2>&1 || ! command -v curl >/dev/null 2>&1; then
	echo "Se requieren jq y curl instalados" >&2
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
PUERTO=8092
SLASH="/"
BASE="http:${SLASH}${SLASH}127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR_LOGS="${USER_HOME}/so2_logs/pruebas_logwatch"
DIR_VIGILADO="${USER_HOME}/so2_vigilado"

if [ -e "$DIR_LOGS" ]; then
	echo "DIR_LOGS (${DIR_LOGS}) ya existe; eliminalo manualmente antes de ejecutar" >&2
	exit 1
fi

if pgrep -x daemon_monitor >/dev/null 2>&1; then
	echo "Existe otro proceso daemon_monitor en ejecución; finalízalo antes de continuar" >&2
	exit 1
fi

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE=""
PREPARADO=0
KTHREADS_INICIALES=$(ps -e -o comm | grep -E '^log_watch_' || true)

aprobar() { echo -e "  ${GREEN}✓ $*${NC}"; APROBADAS=$((APROBADAS + 1)); }
fallar()  { echo -e "  ${RED}✗ $*${NC}"; FALLIDAS=$((FALLIDAS + 1)); }

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
	if [ "$PREPARADO" -eq 1 ]; then
		rm -rf "$DIR_LOGS" 2>/dev/null || true
	fi
}
trap limpiar EXIT

usuarios_prueba_crear || {
	echo "No se pudieron crear los usuarios de prueba" >&2
	exit 1
}

login_stdin() {
	local usuario="$1"
	local pass="${CLAVE[$usuario]}"
	local cookie_file="$2"
	printf '{"usuario":"%s","password":"%s"}' "$usuario" "$pass" | \
		curl -s -X POST "${BASE}/api/login" \
		-H "Content-Type: application/json" \
		--data-binary @- -c "$cookie_file" > /dev/null
}

post_json_resp() {
	local ruta="$1"
	local json_data="$2"
	local cookie="${3:-}"
	if [ -n "$cookie" ]; then
		printf '%s' "$json_data" | curl -s -X POST "${BASE}${ruta}" \
			-H "Content-Type: application/json" \
			-b "$cookie" \
			--data-binary @- \
			-w "\n%{http_code}"
	else
		printf '%s' "$json_data" | curl -s -X POST "${BASE}${ruta}" \
			-H "Content-Type: application/json" \
			--data-binary @- \
			-w "\n%{http_code}"
	fi
}

delete_resp() {
	local ruta="$1"
	local cookie="${2:-}"
	if [ -n "$cookie" ]; then
		curl -s -X DELETE "${BASE}${ruta}" -b "$cookie" -w "\n%{http_code}"
	else
		curl -s -X DELETE "${BASE}${ruta}" -w "\n%{http_code}"
	fi
}

get_resp() {
	local ruta="$1"
	local cookie="${2:-}"
	if [ -n "$cookie" ]; then
		curl -s -X GET "${BASE}${ruta}" -b "$cookie" -w "\n%{http_code}"
	else
		curl -s -X GET "${BASE}${ruta}" -w "\n%{http_code}"
	fi
}

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}              PRUEBAS DE LOGWATCH E IPC (F07)                       ${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/12" "Condiciones previas y verificación de entorno"
if [[ "$(uname -r)" != *so2-202100106* ]]; then
	fallar "Se requiere kernel 6.12.69-so2-202100106 (actual: $(uname -r))"
	exit 1
fi
aprobar "Kernel en ejecución: $(uname -r)"

sudo -u "$REAL_USER" mkdir -p "$DIR_VIGILADO"
sudo -u "$REAL_USER" mkdir -p "$DIR_LOGS"
PREPARADO=1

CONF_PRUEBA="${DIR_LOGS}/daemon.conf"
LOG_DAEMON="${DIR_LOGS}/daemon.log"
COOKIE_ADMIN="${DIR_LOGS}/cookie_admin.txt"
COOKIE_COMUN="${DIR_LOGS}/cookie_comun.txt"
SSE_LOG="${DIR_LOGS}/sse.log"

cat <<EOF > "$CONF_PRUEBA"
puerto = ${PUERTO}
dashboard = ${DASHBOARD_DIR}
log_central = ${DIR_LOGS}/central.txt
log_rutas = ${DIR_LOGS}/inicial.log
log_palabras = SO2T_INICIAL
ruta_vigilada = ${DIR_VIGILADO}
blacklist = ${REPO_DIR}/ProgramaIntermedio/blacklist.json
EOF
chown "$REAL_USER":"$REAL_USER" "$CONF_PRUEBA"

echo "inicio" | sudo -u "$REAL_USER" tee "${DIR_LOGS}/inicial.log" > /dev/null

"$DAEMON" -c "$CONF_PRUEBA" > "$LOG_DAEMON" 2>&1 &
PID_DAEMON=$!

ESPERA_MAX=10
LISTO=0
for ((i = 0; i < ESPERA_MAX * 2; i++)); do
	if curl -s "${BASE}/login.html" >/dev/null 2>&1; then
		LISTO=1
		break
	fi
	sleep 0.5
done
if [ "$LISTO" -eq 1 ]; then
	aprobar "Daemon escuchando en puerto ${PUERTO} (PID ${PID_DAEMON})"
else
	fallar "El daemon no inició en puerto ${PUERTO}"
	exit 1
fi

login_stdin "so2t_admin" "$COOKIE_ADMIN"
login_stdin "so2t_comun" "$COOKIE_COMUN"
aprobar "Sesiones iniciadas para so2t_admin y so2t_comun"

curl -s -N -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_LOG" 2>&1 &
PID_SSE=$!

fase "1/12" "Listado inicial de logwatch y permisos GET"
R_GET_SIN=$(get_resp "/api/logwatch" "")
C_GET_SIN=$(echo "$R_GET_SIN" | tail -n 1)
if [ "$C_GET_SIN" -eq 401 ]; then
	aprobar "GET /api/logwatch sin cookie -> 401"
else
	fallar "GET /api/logwatch sin cookie esperaba 401, obtuvo ${C_GET_SIN}"
fi

R_GET_COM=$(get_resp "/api/logwatch" "$COOKIE_COMUN")
C_GET_COM=$(echo "$R_GET_COM" | tail -n 1)
if [ "$C_GET_COM" -eq 403 ]; then
	aprobar "GET /api/logwatch con comun -> 403"
else
	fallar "GET /api/logwatch con comun esperaba 403, obtuvo ${C_GET_COM}"
fi

R_GET_ADM=$(get_resp "/api/logwatch" "$COOKIE_ADMIN")
C_GET_ADM=$(echo "$R_GET_ADM" | tail -n 1)
B_GET_ADM=$(echo "$R_GET_ADM" | sed '$d')
ID_CONF=""
if [ "$C_GET_ADM" -eq 200 ]; then
	CANT=$(echo "$B_GET_ADM" | jq '.logwatch | length')
	BASE_DIR=$(echo "$B_GET_ADM" | jq -r '.base')
	ORIGEN_0=$(echo "$B_GET_ADM" | jq -r '.logwatch[0].origen')
	PALABRA_0=$(echo "$B_GET_ADM" | jq -r '.logwatch[0].palabra')
	ID_CONF=$(echo "$B_GET_ADM" | jq -r '.logwatch[0].id')
	if [ "$CANT" -eq 1 ] && [ "$BASE_DIR" = "$DIR_LOGS" ] && [ "$ORIGEN_0" = "configuracion" ] && [ "$PALABRA_0" = "SO2T_INICIAL" ]; then
		aprobar "GET /api/logwatch admin -> 200 con base y entrada de configuración"
	else
		fallar "GET /api/logwatch estructura inesperada: ${B_GET_ADM}"
	fi
else
	fallar "GET /api/logwatch admin esperaba 200, obtuvo ${C_GET_ADM}"
fi

fase "2/12" "Detección de palabra clave de configuración y canal IPC"
echo "evento SO2T_INICIAL linea prueba" | sudo -u "$REAL_USER" tee -a "${DIR_LOGS}/inicial.log" > /dev/null

ENCONTRADA_F2=0
for ((i = 0; i < 20; i++)); do
	sleep 0.5
	if [ -f "${DIR_LOGS}/central.txt" ] && grep -Fq "SO2T_INICIAL linea prueba" "${DIR_LOGS}/central.txt"; then
		ALERTAS_F2=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas")
		if echo "$ALERTAS_F2" | jq -e '.alertas[] | select(.motivo == "Palabra clave detectada en log" and (.descripcion | contains("inicial.log")) and (.descripcion | contains("SO2T_INICIAL")))' >/dev/null 2>&1; then
			ENCONTRADA_F2=1
			break
		fi
	fi
done
if [ "$ENCONTRADA_F2" -eq 1 ]; then
	aprobar "Palabra clave detectada en inicial.log reflejada en central.txt y /api/alertas"
else
	fallar "No se detectó alerta o central.txt no recibió la línea de inicial.log"
fi

fase "3/12" "Creación de logwatch desde dashboard por admin"
sudo -u "$REAL_USER" touch "${DIR_LOGS}/a.log" "${DIR_LOGS}/b.log"

JSON_P3=$(printf '{"palabra":"SO2T_NUEVA","central":"%s/central2.txt","rutas":["%s/a.log","%s/b.log"]}' "$DIR_LOGS" "$DIR_LOGS" "$DIR_LOGS")
RESP_P3=$(post_json_resp "/api/logwatch" "$JSON_P3" "$COOKIE_ADMIN")
CODE_P3=$(echo "$RESP_P3" | tail -n 1)
BODY_P3=$(echo "$RESP_P3" | sed '$d')

ID_NUEVO=""
if [ "$CODE_P3" -eq 200 ]; then
	ID_NUEVO=$(echo "$BODY_P3" | jq -r '.id')
	aprobar "POST /api/logwatch admin -> 200 (ID ${ID_NUEVO})"
else
	fallar "POST /api/logwatch admin esperaba 200, obtuvo ${CODE_P3}: ${BODY_P3}"
fi

R_GET_P3=$(get_resp "/api/logwatch" "$COOKIE_ADMIN")
B_GET_P3=$(echo "$R_GET_P3" | sed '$d')
ITEM_NUEVO=$(echo "$B_GET_P3" | jq --argjson id "$ID_NUEVO" '.logwatch[] | select(.id == $id)')
ORIGEN_NUEVO=$(echo "$ITEM_NUEVO" | jq -r '.origen')
USUARIO_NUEVO=$(echo "$ITEM_NUEVO" | jq -r '.usuario')
if [ "$ORIGEN_NUEVO" = "dashboard" ] && [ "$USUARIO_NUEVO" = "so2t_admin" ]; then
	aprobar "Registro contiene ID ${ID_NUEVO} con origen dashboard y usuario so2t_admin"
else
	fallar "Registro no coincide: ${ITEM_NUEVO}"
fi

DIR_OWNER=$(stat -c "%u:%g" "$DIR_LOGS")
CENT_OWNER=$(stat -c "%u:%g" "${DIR_LOGS}/central2.txt" 2>/dev/null || echo "missing")
if [ "$DIR_OWNER" = "$CENT_OWNER" ]; then
	aprobar "central2.txt creada con dueño coincidente con directorio base"
else
	fallar "central2.txt dueño ${CENT_OWNER} no coincide con base ${DIR_OWNER}"
fi

if ps -e -o comm | grep -q "^log_watch_${ID_NUEVO}$"; then
	aprobar "kthread log_watch_${ID_NUEVO} en ejecución"
else
	fallar "kthread log_watch_${ID_NUEVO} no encontrado en proceso del sistema"
fi

fase "4/12" "Monitoreo sobre nuevo logwatch"
echo "evento con SO2T_NUEVA en b.log" | sudo -u "$REAL_USER" tee -a "${DIR_LOGS}/b.log" > /dev/null

ENCONTRADA_F4=0
for ((i = 0; i < 20; i++)); do
	sleep 0.5
	if [ -f "${DIR_LOGS}/central2.txt" ] && grep -Fq "SO2T_NUEVA en b.log" "${DIR_LOGS}/central2.txt"; then
		ALERTAS_F4=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas")
		if echo "$ALERTAS_F4" | jq -e '.alertas[] | select(.motivo == "Palabra clave detectada en log" and (.descripcion | contains("b.log")) and (.descripcion | contains("SO2T_NUEVA")))' >/dev/null 2>&1; then
			ENCONTRADA_F4=1
			break
		fi
	fi
done
if [ "$ENCONTRADA_F4" -eq 1 ]; then
	aprobar "SO2T_NUEVA detectada en b.log y escrita en central2.txt"
else
	fallar "No se detectó alerta de SO2T_NUEVA en b.log"
fi

fase "5/12" "Validaciones de parámetros inválidos"
KTHREADS_ANTES=$(ps -e -o comm | grep -c '^log_watch_' || true)

J_SHADOW=$(printf '{"palabra":"TEST","central":"%s/cen.txt","rutas":["/etc/shadow"]}' "$DIR_LOGS")
R_SHADOW=$(post_json_resp "/api/logwatch" "$J_SHADOW" "$COOKIE_ADMIN")
C_SHADOW=$(echo "$R_SHADOW" | tail -n 1)
B_SHADOW=$(echo "$R_SHADOW" | sed '$d')
if [ "$C_SHADOW" -eq 400 ] && [ "$(echo "$B_SHADOW" | jq -r .error)" = "Ruta invalida: /etc/shadow" ]; then
	aprobar "Ruta fuera de base /etc/shadow -> 400 Ruta invalida: /etc/shadow"
else
	fallar "Ruta /etc/shadow esperaba 400 Ruta invalida: /etc/shadow, obtuvo ${C_SHADOW}: ${B_SHADOW}"
fi

J_ETCX=$(printf '{"palabra":"TEST","central":"/etc/x","rutas":["%s/a.log"]}' "$DIR_LOGS")
R_ETCX=$(post_json_resp "/api/logwatch" "$J_ETCX" "$COOKIE_ADMIN")
C_ETCX=$(echo "$R_ETCX" | tail -n 1)
B_ETCX=$(echo "$R_ETCX" | sed '$d')
if [ "$C_ETCX" -eq 400 ] && [ "$(echo "$B_ETCX" | jq -r .error)" = "Ruta central invalida" ]; then
	aprobar "Central fuera de base /etc/x -> 400 Ruta central invalida"
else
	fallar "Central /etc/x esperaba 400 Ruta central invalida, obtuvo ${C_ETCX}: ${B_ETCX}"
fi

sudo -u "$REAL_USER" ln -s "${DIR_LOGS}/a.log" "${DIR_LOGS}/enlace_a.log"
J_LINK=$(printf '{"palabra":"TEST","central":"%s/cen.txt","rutas":["%s/enlace_a.log"]}' "$DIR_LOGS" "$DIR_LOGS")
R_LINK=$(post_json_resp "/api/logwatch" "$J_LINK" "$COOKIE_ADMIN")
C_LINK=$(echo "$R_LINK" | tail -n 1)
B_LINK=$(echo "$R_LINK" | sed '$d')
sudo -u "$REAL_USER" rm -f "${DIR_LOGS}/enlace_a.log"
if [ "$C_LINK" -eq 400 ] && [ "$(echo "$B_LINK" | jq -r .error)" = "Ruta invalida: ${DIR_LOGS}/enlace_a.log" ]; then
	aprobar "Ruta vigilada enlace simbolico -> 400 Ruta invalida"
else
	fallar "Enlace simbolico esperaba 400 Ruta invalida, obtuvo ${C_LINK}: ${B_LINK}"
fi

J_PIPE=$(printf '{"palabra":"TEST|MAL","central":"%s/cen.txt","rutas":["%s/a.log"]}' "$DIR_LOGS" "$DIR_LOGS")
R_PIPE=$(post_json_resp "/api/logwatch" "$J_PIPE" "$COOKIE_ADMIN")
C_PIPE=$(echo "$R_PIPE" | tail -n 1)
B_PIPE=$(echo "$R_PIPE" | sed '$d')
if [ "$C_PIPE" -eq 400 ] && [ "$(echo "$B_PIPE" | jq -r .error)" = "Palabra invalida" ]; then
	aprobar "Palabra con pipe -> 400 Palabra invalida"
else
	fallar "Palabra con pipe esperaba 400 Palabra invalida, obtuvo ${C_PIPE}: ${B_PIPE}"
fi

J_MISMA=$(printf '{"palabra":"TEST","central":"%s/a.log","rutas":["%s/a.log"]}' "$DIR_LOGS" "$DIR_LOGS")
R_MISMA=$(post_json_resp "/api/logwatch" "$J_MISMA" "$COOKIE_ADMIN")
C_MISMA=$(echo "$R_MISMA" | tail -n 1)
B_MISMA=$(echo "$R_MISMA" | sed '$d')
if [ "$C_MISMA" -eq 400 ] && [ "$(echo "$B_MISMA" | jq -r .error)" = "La ruta central no puede ser un archivo vigilado" ]; then
	aprobar "Central igual a ruta propia -> 400 La ruta central no puede ser un archivo vigilado"
else
	fallar "Central igual a ruta esperaba 400, obtuvo ${C_MISMA}: ${B_MISMA}"
fi

J_CICLO=$(printf '{"palabra":"TEST","central":"%s/cen_ciclo.txt","rutas":["%s/central.txt"]}' "$DIR_LOGS" "$DIR_LOGS")
R_CICLO=$(post_json_resp "/api/logwatch" "$J_CICLO" "$COOKIE_ADMIN")
C_CICLO=$(echo "$R_CICLO" | tail -n 1)
B_CICLO=$(echo "$R_CICLO" | sed '$d')
if [ "$C_CICLO" -eq 409 ] && echo "$B_CICLO" | grep -q "un log central no puede vigilarse"; then
	aprobar "Ruta que es central de otro logwatch -> 409 Conflicto por ciclo"
else
	fallar "Ciclo esperaba 409 Conflicto, obtuvo ${C_CICLO}: ${B_CICLO}"
fi

J_EXTRA=$(printf '{"palabra":"TEST","central":"%s/cen.txt","rutas":["%s/a.log"],"extra":"val"}' "$DIR_LOGS" "$DIR_LOGS")
R_EXTRA=$(post_json_resp "/api/logwatch" "$J_EXTRA" "$COOKIE_ADMIN")
C_EXTRA=$(echo "$R_EXTRA" | tail -n 1)
B_EXTRA=$(echo "$R_EXTRA" | sed '$d')
if [ "$C_EXTRA" -eq 400 ] && [ "$(echo "$B_EXTRA" | jq -r .error)" = "Cuerpo invalido" ]; then
	aprobar "Cuerpo con clave adicional -> 400 Cuerpo invalido"
else
	fallar "Cuerpo con clave extra esperaba 400 Cuerpo invalido, obtuvo ${C_EXTRA}: ${B_EXTRA}"
fi

KTHREADS_DESPUES=$(ps -e -o comm | grep -c '^log_watch_' || true)
if [ "$KTHREADS_ANTES" -eq "$KTHREADS_DESPUES" ] && [ ! -e /etc/x ]; then
	aprobar "Sin mutación de kthreads ni creación de /etc/x tras errores"
else
	fallar "Estado alterado tras peticiones inválidas"
fi

fase "6/12" "Permisos de POST y DELETE en usuario común y anónimo"
J_COMUN=$(printf '{"palabra":"TEST","central":"%s/cen_c.txt","rutas":["%s/a.log"]}' "$DIR_LOGS" "$DIR_LOGS")
R_PCOM=$(post_json_resp "/api/logwatch" "$J_COMUN" "$COOKIE_COMUN")
C_PCOM=$(echo "$R_PCOM" | tail -n 1)
if [ "$C_PCOM" -eq 403 ]; then
	aprobar "POST /api/logwatch usuario común -> 403"
else
	fallar "POST /api/logwatch usuario común esperaba 403, obtuvo ${C_PCOM}"
fi

R_PSIN=$(post_json_resp "/api/logwatch" "$J_COMUN" "")
C_PSIN=$(echo "$R_PSIN" | tail -n 1)
if [ "$C_PSIN" -eq 401 ]; then
	aprobar "POST /api/logwatch sin cookie -> 401"
else
	fallar "POST /api/logwatch sin cookie esperaba 401, obtuvo ${C_PSIN}"
fi

R_DCOM=$(delete_resp "/api/logwatch?id=${ID_NUEVO}" "$COOKIE_COMUN")
C_DCOM=$(echo "$R_DCOM" | tail -n 1)
if [ "$C_DCOM" -eq 403 ]; then
	aprobar "DELETE /api/logwatch usuario común -> 403"
else
	fallar "DELETE /api/logwatch usuario común esperaba 403, obtuvo ${C_DCOM}"
fi

R_DSIN=$(delete_resp "/api/logwatch?id=${ID_NUEVO}" "")
C_DSIN=$(echo "$R_DSIN" | tail -n 1)
if [ "$C_DSIN" -eq 401 ]; then
	aprobar "DELETE /api/logwatch sin cookie -> 401"
else
	fallar "DELETE /api/logwatch sin cookie esperaba 401, obtuvo ${C_DSIN}"
fi

fase "7/12" "Finalización de logwatch y cese de monitoreo"
R_DEL_OK=$(delete_resp "/api/logwatch?id=${ID_NUEVO}" "$COOKIE_ADMIN")
C_DEL_OK=$(echo "$R_DEL_OK" | tail -n 1)
B_DEL_OK=$(echo "$R_DEL_OK" | sed '$d')
if [ "$C_DEL_OK" -eq 200 ] && [ "$(echo "$B_DEL_OK" | jq -r .finalizado)" = "true" ]; then
	aprobar "DELETE /api/logwatch?id=${ID_NUEVO} -> 200 finalizado: true"
else
	fallar "DELETE esperaba 200 finalizado: true, obtuvo ${C_DEL_OK}: ${B_DEL_OK}"
fi

KT_PARADO=0
for ((i = 0; i < 20; i++)); do
	if ! ps -e -o comm | grep -q "^log_watch_${ID_NUEVO}$"; then
		KT_PARADO=1
		break
	fi
	sleep 0.2
done
if [ "$KT_PARADO" -eq 1 ]; then
	aprobar "kthread log_watch_${ID_NUEVO} finalizó"
else
	fallar "kthread log_watch_${ID_NUEVO} no finalizó"
fi

N_ANTES=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas" | jq '[.alertas[] | select(.descripcion | contains("SO2T_NUEVA"))] | length')
echo "otra linea SO2T_NUEVA tras stop" | sudo -u "$REAL_USER" tee -a "${DIR_LOGS}/b.log" > /dev/null
sleep 8
N_DESPUES=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas" | jq '[.alertas[] | select(.descripcion | contains("SO2T_NUEVA"))] | length')
if [ "$N_ANTES" -eq "$N_DESPUES" ]; then
	aprobar "Escritura posterior no produce alerta tras finalizar logwatch"
else
	fallar "Se recibieron alertas tras finalizar logwatch (${N_ANTES} -> ${N_DESPUES})"
fi

R_DEL_2=$(delete_resp "/api/logwatch?id=${ID_NUEVO}" "$COOKIE_ADMIN")
C_DEL_2=$(echo "$R_DEL_2" | tail -n 1)
B_DEL_2=$(echo "$R_DEL_2" | sed '$d')
if [ "$C_DEL_2" -eq 404 ] && [ "$(echo "$B_DEL_2" | jq -r .error)" = "Logwatch no encontrado" ]; then
	aprobar "Segundo DELETE -> 404 Logwatch no encontrado"
else
	fallar "Segundo DELETE esperaba 404 Logwatch no encontrado, obtuvo ${C_DEL_2}: ${B_DEL_2}"
fi

R_DEL_INV=$(delete_resp "/api/logwatch?id=abc" "$COOKIE_ADMIN")
C_DEL_INV=$(echo "$R_DEL_INV" | tail -n 1)
B_DEL_INV=$(echo "$R_DEL_INV" | sed '$d')
if [ "$C_DEL_INV" -eq 400 ] && [ "$(echo "$B_DEL_INV" | jq -r .error)" = "ID invalido" ]; then
	aprobar "DELETE con id=abc -> 400 ID invalido"
else
	fallar "DELETE id invalido esperaba 400 ID invalido, obtuvo ${C_DEL_INV}: ${B_DEL_INV}"
fi

fase "8/12" "Verificación de reposo del hilo IPC"
IPC_TID=""
for t in /proc/"$PID_DAEMON"/task/*; do
	if [ -f "$t/comm" ] && [ "$(cat "$t/comm")" = "ipc" ]; then
		IPC_TID=$(basename "$t")
		break
	fi
done
if [ -n "$IPC_TID" ]; then
	V1=$(grep "voluntary_ctxt_switches:" "/proc/${PID_DAEMON}/task/${IPC_TID}/status" | head -n 1 | awk '{print $2}')
	sleep 5
	V2=$(grep "voluntary_ctxt_switches:" "/proc/${PID_DAEMON}/task/${IPC_TID}/status" | head -n 1 | awk '{print $2}')
	if [ "$V1" -eq "$V2" ]; then
		aprobar "Hilo IPC en reposo estricto: 0 switches en 5 s (${V1} -> ${V2})"
	else
		fallar "Hilo IPC incrementó switches en reposo: ${V1} -> ${V2}"
	fi
else
	fallar "No se encontró el hilo con nombre ipc"
fi

fase "9/12" "Límite de 16 logwatch simultáneos"
IDS_LIMITE=()
for ((i = 1; i <= 15; i++)); do
	J_LIM=$(printf '{"palabra":"LIM_%d","central":"%s/cen_l_%d.txt","rutas":["%s/a.log"]}' "$i" "$DIR_LOGS" "$i" "$DIR_LOGS")
	R_LIM=$(post_json_resp "/api/logwatch" "$J_LIM" "$COOKIE_ADMIN")
	C_LIM=$(echo "$R_LIM" | tail -n 1)
	B_LIM=$(echo "$R_LIM" | sed '$d')
	if [ "$C_LIM" -eq 200 ]; then
		ID_LIM=$(echo "$B_LIM" | jq -r '.id')
		IDS_LIMITE+=("$ID_LIM")
	else
		fallar "Fallo creando logwatch limite ${i}: ${C_LIM} ${B_LIM}"
	fi
done

J_LIM16=$(printf '{"palabra":"LIM_16","central":"%s/cen_l_16.txt","rutas":["%s/a.log"]}' "$DIR_LOGS" "$DIR_LOGS")
R_LIM16=$(post_json_resp "/api/logwatch" "$J_LIM16" "$COOKIE_ADMIN")
C_LIM16=$(echo "$R_LIM16" | tail -n 1)
B_LIM16=$(echo "$R_LIM16" | sed '$d')
if [ "$C_LIM16" -eq 409 ] && [ "$(echo "$B_LIM16" | jq -r .error)" = "Limite de 16 logwatch alcanzado" ]; then
	aprobar "Logwatch 17 rechazado con 409 Limite de 16 logwatch alcanzado"
else
	fallar "Logwatch 17 esperaba 409 Limite alcanzado, obtuvo ${C_LIM16}: ${B_LIM16}"
fi

DEL_OK=0
for id in "${IDS_LIMITE[@]}"; do
	R_DEL=$(delete_resp "/api/logwatch?id=${id}" "$COOKIE_ADMIN")
	C_DEL=$(echo "$R_DEL" | tail -n 1)
	if [ "$C_DEL" -eq 200 ]; then
		DEL_OK=$((DEL_OK + 1))
	else
		fallar "DELETE logwatch ${id} esperaba 200, obtuvo ${C_DEL}"
	fi
done
if [ "$DEL_OK" -eq "${#IDS_LIMITE[@]}" ]; then
	aprobar "15 logwatch de prueba finalizados correctamente con 200"
fi

fase "10/12" "Finalización de logwatch de configuración"
R_DEL_CONF=$(delete_resp "/api/logwatch?id=${ID_CONF:-1}" "$COOKIE_ADMIN")
C_DEL_CONF=$(echo "$R_DEL_CONF" | tail -n 1)
if [ "$C_DEL_CONF" -eq 200 ]; then
	aprobar "DELETE logwatch ${ID_CONF:-1} (configuracion) -> 200"
else
	fallar "DELETE logwatch ${ID_CONF:-1} esperaba 200, obtuvo ${C_DEL_CONF}"
fi

R_LISTA_VACIA=$(get_resp "/api/logwatch" "$COOKIE_ADMIN")
B_LISTA_VACIA=$(echo "$R_LISTA_VACIA" | sed '$d')
CANT_VACIA=$(echo "$B_LISTA_VACIA" | jq '.logwatch | length')
if [ "$CANT_VACIA" -eq 0 ]; then
	aprobar "Listado de logwatch quedó vacío (0 elementos)"
else
	fallar "Listado esperaba 0 elementos, tiene ${CANT_VACIA}"
fi

fase "11/12" "Apagado con SIGTERM y limpieza de kthreads"
J_TERM=$(printf '{"palabra":"TERM_TEST","central":"%s/cen_term.txt","rutas":["%s/a.log"]}' "$DIR_LOGS" "$DIR_LOGS")
R_TERM=$(post_json_resp "/api/logwatch" "$J_TERM" "$COOKIE_ADMIN")
C_TERM=$(echo "$R_TERM" | tail -n 1)
if [ "$C_TERM" -eq 200 ]; then
	aprobar "Logwatch previo a SIGTERM creado"
else
	fallar "Error creando logwatch previo a SIGTERM: ${C_TERM}"
fi

if [ -n "$PID_SSE" ]; then
	kill "$PID_SSE" 2>/dev/null || true
	wait "$PID_SSE" 2>/dev/null || true
	PID_SSE=""
fi

kill -TERM "$PID_DAEMON"
wait "$PID_DAEMON"
EXIT_CODE=$?
PID_DAEMON=""
if [ "$EXIT_CODE" -eq 0 ]; then
	aprobar "Daemon finalizó limpiamente con código 0 ante SIGTERM"
else
	fallar "Daemon finalizó con código ${EXIT_CODE}"
fi

KTHREADS_FINALES=$(ps -e -o comm | grep -E '^log_watch_' || true)
if [ "$KTHREADS_FINALES" = "$KTHREADS_INICIALES" ]; then
	aprobar "Todos los kthreads de prueba fueron terminados limpiamente"
else
	fallar "Kthreads residuales detectados: ${KTHREADS_FINALES}"
fi

fase "12/12" "Seguridad en logs y persistencia de directorios"
PASS_FOUND=0
for u in "${USUARIOS_CREADOS[@]}"; do
	p="${CLAVE[$u]:-}"
	if [ -n "$p" ] && grep -Fq "$p" "$LOG_DAEMON"; then
		PASS_FOUND=1
		break
	fi
done
if [ "$PASS_FOUND" -eq 0 ]; then
	aprobar "Log del daemon no expone contraseñas en texto claro"
else
	fallar "Se encontraron contraseñas en el log del daemon"
fi

if [ -d "${USER_HOME}/so2_logs" ]; then
	aprobar "Directorio ~/so2_logs sigue existiendo"
else
	fallar "Directorio ~/so2_logs fue eliminado"
fi

if grep -Fq "clave desconocida" "$LOG_DAEMON"; then
	fallar "Log del daemon contiene 'clave desconocida'"
else
	aprobar "Log del daemon no contiene claves desconocidas"
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "  Aprobadas: ${GREEN}${APROBADAS}${NC} · Fallidas: ${RED}${FALLIDAS}${NC}"
echo -e "${BLUE}====================================================================${NC}"

if [ "$FALLIDAS" -gt 0 ]; then
	exit 1
fi
exit 0
