#!/usr/bin/env bash
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "Ejecuta con sudo" >&2
	exit 1
fi

GREEN='\033[0;32m'; BLUE='\033[0;34m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
fase() { echo -e "\n${YELLOW}[$1] $2${NC}"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON="${SCRIPT_DIR}/../ProgramaIntermedio/daemon_monitor"
DASHBOARD_DIR="$(cd "${SCRIPT_DIR}/../Dashboard" && pwd)"
PUERTO=8099
BASE="http://127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR="${USER_HOME}/so2_logs/pruebas"
PALABRA="INTRUSO"
REPOSO_S=10
RAFAGA=20

source "${SCRIPT_DIR}/usuarios_prueba.sh"

COOKIE_JAR=$(mktemp)
APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE=""

aprobar() { echo -e "  ${GREEN}✓ $*${NC}"; APROBADAS=$((APROBADAS + 1)); }
fallar()  { echo -e "  ${RED}✗ $*${NC}"; FALLIDAS=$((FALLIDAS + 1)); }
comprobar() {
	local descripcion="$1"; shift
	if "$@" >/dev/null 2>&1; then aprobar "$descripcion"; else fallar "$descripcion"; fi
}

limpiar() {
	[ -n "$PID_SSE" ] && kill "$PID_SSE" 2>/dev/null || true
	[ -n "$PID_DAEMON" ] && kill -TERM "$PID_DAEMON" 2>/dev/null || true
	[ -f "$COOKIE_JAR" ] && rm -f "$COOKIE_JAR" || true
	usuarios_prueba_borrar
	chown -R "$REAL_USER:$REAL_USER" "$DIR" 2>/dev/null || true
}
trap limpiar EXIT

ahora_ms()       { date +%s%3N; }
codigo_http()    { curl -s -m 5 -b "$COOKIE_JAR" -o /dev/null -w '%{http_code}' "$@"; }
hilos_log_watch() { ps -eo comm= | grep -c '^log_watch_' || true; }
eventos_sse()    { grep -c '^data: .*"tipo":"logs"' "${DIR}/sse.txt" || true; }
vivo()           { kill -0 "$PID_DAEMON" 2>/dev/null; }

esperar_eventos() {
	local esperados="$1" limite_ms="$2" inicio
	inicio=$(ahora_ms)
	while [ "$(eventos_sse)" -lt "$esperados" ]; do
		[ $(( $(ahora_ms) - inicio )) -gt "$limite_ms" ] && return 1
		sleep 0.02
	done
}

tics_de() { awk '{ print $14 + $15 }' "$1/stat" 2>/dev/null || echo 0; }
cambios_de() { awk '/ctxt_switches/ { suma += $2 } END { print suma + 0 }' "$1/status" 2>/dev/null; }

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}   PRUEBAS DE INTEGRACION PROYECTO UNICO - SO2 2S2026 - CARNE 202100106${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/8" "Condiciones previas"
if [[ "$(uname -r)" != *so2-202100106* ]]; then
	fallar "El kernel en ejecucion es $(uname -r), se requiere 6.12.69-so2-202100106"
	exit 1
fi
aprobar "Kernel en ejecucion: $(uname -r)"

if ! usuarios_prueba_condiciones; then
	fallar "Falta configurar PAM o grupos: ejecuta sudo scripts/setup_usuarios.sh"
	exit 1
fi
usuarios_prueba_crear

if pgrep -x daemon_monitor >/dev/null; then
	fallar "Ya hay un daemon_monitor activo; el canal IPC es unico y se repartirian los eventos"
	echo "    Detenlo con: pkill -TERM -x daemon_monitor"
	exit 1
fi
[ -x "$DAEMON" ] || make -C "${SCRIPT_DIR}/../ProgramaIntermedio" >/dev/null
comprobar "Ejecutable del daemon disponible" test -x "$DAEMON"

mkdir -p "$DIR"
: > "${DIR}/a.log"; : > "${DIR}/b.log"; : > "${DIR}/central.txt"; : > "${DIR}/sse.txt"
HILOS_ANTES=$(hilos_log_watch)

fase "1/8" "Arranque del daemon y de log_watch"
CONF_TEST="${DIR}/daemon_test.conf"
cat << EOF > "$CONF_TEST"
puerto = ${PUERTO}
intervalo = 2
dashboard = ${DASHBOARD_DIR}
log_central = ${DIR}/central.txt
log_rutas = ${DIR}/a.log, ${DIR}/b.log
log_palabras = ${PALABRA}
ruta_vigilada = ${DIR}/vigilado
blacklist = ${SCRIPT_DIR}/../ProgramaIntermedio/blacklist.json
EOF

"$DAEMON" -c "$CONF_TEST" 2> "${DIR}/daemon.txt" &
PID_DAEMON=$!
for _ in $(seq 50); do
	[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/login.html")" = "200" ] && break
	sleep 0.1
done
login_json "so2t_admin" | curl -s -c "$COOKIE_JAR" -H "Content-Type: application/json" -d @- "${BASE}/api/login" >/dev/null
comprobar "El daemon responde en el puerto ${PUERTO}" test "$(codigo_http "${BASE}/api/estado")" = "200"
comprobar "sys_start_log_watch creo un hilo de kernel" test "$(hilos_log_watch)" -eq $((HILOS_ANTES + 1))

fase "2/8" "Snapshot /api/estado contra el sistema"
curl -s -m 5 -b "$COOKIE_JAR" "${BASE}/api/estado" > "${DIR}/estado.json"
python3 -I - "${DIR}/estado.json" <<'PYEOF' > "${DIR}/estado.txt"
import json, os, sys

d = json.load(open(sys.argv[1]))
mem = {}
for linea in open('/proc/meminfo'):
	clave, valor = linea.split(':')
	mem[clave] = int(valor.split()[0])

def cerca(a, b):
	return abs(a - b) <= max(b * 0.05, 20480)

r = d['resumen_procesos']
pids = sum(1 for e in os.listdir('/proc') if e.isdigit())
campos = ['memoria_usada', 'memoria_libre', 'memoria_cache', 'swap_usada',
          'procesos']
pruebas = {
	'campos': all(c in d for c in campos),
	'usada': cerca(d['memoria_usada'], mem['MemTotal'] - mem['MemAvailable']),
	'libre': cerca(d['memoria_libre'], mem['MemAvailable']),
	'cache': cerca(d['memoria_cache'], mem['Cached'] + mem['Buffers']),
	'swap': cerca(d['swap_usada'], mem['SwapTotal'] - mem['SwapFree']),
	'suma': r['running'] + r['sleeping'] + r['zombie'] == r['total'],
	'tabla': abs(len(d['procesos']) - pids) <= 15,
	'resumen': abs(r['total'] - pids) <= 15,
	'forma': all(set(p) == {'nombre', 'pid', 'estado'} for p in d['procesos']),
}
for nombre, ok in pruebas.items():
	print(nombre, 'ok' if ok else 'mal')
print('detalle', len(d['procesos']), r['total'], pids)
PYEOF
revisar() { grep -q "^$1 ok$" "${DIR}/estado.txt"; }
comprobar "JSON con todos los campos del enunciado" revisar campos
comprobar "memoria_usada coincide con /proc/meminfo" revisar usada
comprobar "memoria_libre coincide con /proc/meminfo" revisar libre
comprobar "memoria_cache coincide con /proc/meminfo" revisar cache
comprobar "swap_usada coincide con /proc/meminfo" revisar swap
comprobar "sys_get_processes_state: running + sleeping + zombie = total" revisar suma
comprobar "El total de la syscall coincide con /proc" revisar resumen
comprobar "La tabla de procesos coincide con /proc" revisar tabla
comprobar "Cada proceso trae nombre, pid y estado" revisar forma
read -r _ EN_TABLA EN_SYSCALL EN_PROC < <(grep '^detalle' "${DIR}/estado.txt")
echo "    Procesos: tabla=${EN_TABLA} syscall=${EN_SYSCALL} /proc=${EN_PROC}"

fase "3/8" "Consulta por PID (sys_get_process_info)"
RESPUESTA=$(curl -s -m 5 -b "$COOKIE_JAR" "${BASE}/api/proceso?pid=${PID_DAEMON}")
comprobar "PID ${PID_DAEMON} devuelve nombre daemon_monitor" \
	grep -q '"nombre":"daemon_monitor"' <<< "$RESPUESTA"
comprobar "PID inexistente responde 404 (ESRCH)" \
	test "$(codigo_http "${BASE}/api/proceso?pid=4194000")" = "404"
comprobar "PID no numerico responde 400" test "$(codigo_http "${BASE}/api/proceso?pid=abc")" = "400"
comprobar "PID ausente responde 400" test "$(codigo_http "${BASE}/api/proceso")" = "400"

fase "4/8" "Evento de extremo a extremo: log -> kernel -> IPC -> daemon -> SSE"
curl -s -N -m 120 -b "$COOKIE_JAR" "${BASE}/api/eventos" > "${DIR}/sse.txt" &
PID_SSE=$!
sleep 0.5
LINEA="${PALABRA} acceso no autorizado desde 10.0.0.7 \"root\" | puerto 22"
INICIO=$(ahora_ms)
echo "$LINEA" >> "${DIR}/a.log"
if esperar_eventos 1 6000; then
	aprobar "El evento llego por SSE en $(( $(ahora_ms) - INICIO )) ms sin refrescar ni reconsultar"
else
	fallar "El evento no llego por SSE en 6 s"
fi
EVENTO=$(grep '^data: .*"tipo":"logs"' "${DIR}/sse.txt" | head -1)
comprobar "archivo_origen es la ruta del log" grep -q "\"archivo_origen\":\"${DIR}/a.log\"" <<< "$EVENTO"
comprobar "palabra_detectada es ${PALABRA}" grep -q "\"palabra_detectada\":\"${PALABRA}\"" <<< "$EVENTO"
comprobar "contenido conserva la linea, con comillas escapadas" \
	grep -qF 'desde 10.0.0.7 \"root\" | puerto 22' <<< "$EVENTO"
comprobar "El log centralizado registro el hallazgo" grep -q "$PALABRA acceso" "${DIR}/central.txt"
comprobar "/api/alertas incluye la alerta" \
	grep -q "\"palabra_detectada\":\"${PALABRA}\"" <(curl -s -m 5 -b "$COOKIE_JAR" "${BASE}/api/alertas")

echo "linea normal sin la palabra vigilada" >> "${DIR}/b.log"
sleep 4
comprobar "Una linea sin la palabra clave no genera evento" test "$(eventos_sse)" -eq 1

fase "5/8" "Rafaga de ${RAFAGA} eventos"
for i in $(seq "$RAFAGA"); do echo "${PALABRA} rafaga ${i}" >> "${DIR}/b.log"; done
if esperar_eventos $((RAFAGA + 1)) 8000; then
	aprobar "Llegaron los ${RAFAGA} eventos de la rafaga, sin perdidas"
else
	fallar "Solo llegaron $(( $(eventos_sse) - 1 )) de ${RAFAGA} eventos"
fi
comprobar "Los eventos conservan el orden de escritura" \
	test "$(grep '^data: .*"tipo":"logs"' "${DIR}/sse.txt" | grep -o '"contenido":"[^"]*rafaga [0-9]*' | grep -o 'rafaga [0-9]*' | awk '{ print $2 }' | paste -sd' ')" \
	   = "$(seq -s' ' "$RAFAGA")"

fase "6/8" "Consumo en reposo durante ${REPOSO_S} s (sin espera activa)"
TID_EVENTOS=$(ps -L -o tid=,wchan:40= -p "$PID_DAEMON" | awk '/ipc_channel_receive/ { print $1 }')
if [ -z "$TID_EVENTOS" ]; then
	fallar "No se encontro un hilo bloqueado en sys_ipc_channel_receive"
else
	aprobar "Hilo ${TID_EVENTOS} bloqueado dentro de sys_ipc_channel_receive"
	TAREA="/proc/${PID_DAEMON}/task/${TID_EVENTOS}"
	TICS_HILO=$(tics_de "$TAREA"); CAMBIOS=$(cambios_de "$TAREA")
	TICS_PROC=0; for t in /proc/"${PID_DAEMON}"/task/*; do TICS_PROC=$((TICS_PROC + $(tics_de "$t"))); done
	sleep "$REPOSO_S"
	D_HILO=$(( $(tics_de "$TAREA") - TICS_HILO )); D_CAMBIOS=$(( $(cambios_de "$TAREA") - CAMBIOS ))
	FIN_PROC=0; for t in /proc/"${PID_DAEMON}"/task/*; do FIN_PROC=$((FIN_PROC + $(tics_de "$t"))); done
	ESTADO=$(awk '{ print $3 }' "${TAREA}/stat")
	TCK=$(getconf CLK_TCK)

	comprobar "El hilo de eventos consumio 0 tics de CPU" test "$D_HILO" -eq 0
	comprobar "El hilo de eventos no desperto ni una vez (0 cambios de contexto)" test "$D_CAMBIOS" -eq 0
	comprobar "El hilo de eventos esta en estado S" test "$ESTADO" = "S"
	echo "    Proceso completo: $(awk -v d=$((FIN_PROC - TICS_PROC)) -v t="$TCK" -v s="$REPOSO_S" \
		'BEGIN { printf "%.2f", d * 100 / (t * s) }') % de CPU (hilo de metricas cada 2 s)"
fi

fase "7/8" "Resiliencia ante peticiones invalidas y clientes que se desconectan"
comprobar "POST responde 405" test "$(codigo_http -X POST -d '' "${BASE}/api/estado")" = "405"
comprobar "Ruta con .. responde 400" test "$(codigo_http --path-as-is "${BASE}/../../etc/passwd")" = "400"
comprobar "Recurso inexistente responde 404" test "$(codigo_http "${BASE}/no-existe.js")" = "404"
comprobar "El dashboard se sirve en /" test "$(codigo_http "${BASE}/")" = "200"
printf 'BASURA SIN FORMATO\r\n\r\n' | curl -s -m 2 "telnet://127.0.0.1:${PUERTO}" >/dev/null 2>&1
kill -9 "$PID_SSE" 2>/dev/null; wait "$PID_SSE" 2>/dev/null; PID_SSE=""
PARALELAS=()
for _ in $(seq 30); do curl -s -m 5 -b "$COOKIE_JAR" -o /dev/null "${BASE}/api/estado" & PARALELAS+=($!); done
wait "${PARALELAS[@]}"
comprobar "El daemon sigue vivo y respondiendo" test "$(codigo_http "${BASE}/api/estado")" = "200"

fase "8/8" "Apagado limpio con SIGTERM"
kill -TERM "$PID_DAEMON"
wait "$PID_DAEMON"; SALIDA=$?
PID_DAEMON=""
comprobar "El daemon termino con codigo 0" test "$SALIDA" -eq 0
comprobar "sys_stop_log_watch elimino el hilo de kernel" test "$(hilos_log_watch)" -eq "$HILOS_ANTES"
comprobar "El puerto ${PUERTO} quedo libre" test "$(codigo_http "${BASE}/api/estado")" = "000"

echo -e "\n${BLUE}====================================================================${NC}"
if [ "$FALLIDAS" -eq 0 ]; then
	echo -e "${GREEN}   ${APROBADAS} PRUEBAS APROBADAS, 0 FALLIDAS${NC}"
else
	echo -e "${RED}   ${APROBADAS} APROBADAS, ${FALLIDAS} FALLIDAS${NC}"
fi
echo -e "${BLUE}====================================================================${NC}"
echo "Registro del daemon: ${DIR}/daemon.txt"
[ "$FALLIDAS" -eq 0 ]
