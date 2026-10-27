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
PUERTO=8095
BASE="http://127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR="${USER_HOME}/so2_logs/pruebas_procesos"

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE=""
PID_300=""
PID_MULTI=""
TEMP_FILES=()

aprobar() { echo -e "  ${GREEN}✓ $*${NC}"; APROBADAS=$((APROBADAS + 1)); }
fallar()  { echo -e "  ${RED}✗ $*${NC}"; FALLIDAS=$((FALLIDAS + 1)); }

mktemp_rastreado() {
	local archivo
	archivo=$(mktemp)
	TEMP_FILES+=("$archivo")
	echo "$archivo"
}

limpiar() {
	if [ -n "$PID_300" ]; then
		kill -9 "$PID_300" 2>/dev/null || true
		wait "$PID_300" 2>/dev/null || true
	fi
	if [ -n "$PID_MULTI" ]; then
		kill -9 "$PID_MULTI" 2>/dev/null || true
		wait "$PID_MULTI" 2>/dev/null || true
	fi
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
echo -e "${BLUE}       PRUEBAS DE PROCESOS (TOP 20, KILL) - PROYECTO ÚNICO SO2       ${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/16" "Condiciones previas y preparación de usuarios"
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
aprobar "Usuarios de prueba creados"

mkdir -p "$DIR"
mkdir -p "${DIR}/vigilado"
: > "${DIR}/a.log"; : > "${DIR}/b.log"; : > "${DIR}/central.txt"
cat << EOF > "${DIR}/daemon.conf"
puerto = ${PUERTO}
intervalo = 3
dashboard = ${DASHBOARD_DIR}
log_central = ${DIR}/central.txt
log_rutas = ${DIR}/a.log, ${DIR}/b.log
log_palabras = PROCESOS_TEST
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

fase "1/16" "GET /api/procesos sin cookie responde 401"
c1=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/api/procesos")
if [ "$c1" = "401" ]; then
	aprobar "Rechaza sin cookie con 401"
else
	fallar "Respondió ${c1} en lugar de 401"
fi

fase "2/16" "Común: /api/procesos responde 200, top <= 20 ordenado desc por RSS y porcentaje exacto"
COOKIE_COMUN=$(mktemp_rastreado)
login_json "so2t_comun" | curl -s -c "$COOKIE_COMUN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/login"
sleep 1
resp_p=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/procesos")
code_p=$(curl -s -b "$COOKIE_COMUN" -o /dev/null -w '%{http_code}' "${BASE}/api/procesos")

CHECK_TOP=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	assert d.get("activo") == True
	assert "resumen" in d
	resumen = d["resumen"]
	for k in ("total", "running", "sleeping", "zombie"):
		assert k in resumen
	top = d.get("top", [])
	assert len(top) <= 20
	mem_total = 0
	with open("/proc/meminfo") as f:
		for line in f:
			if line.startswith("MemTotal:"):
				mem_total = int(line.split()[1])
				break
	assert mem_total > 0
	for i in range(len(top)):
		proc = top[i]
		assert "pid" in proc and "nombre" in proc and "estado" in proc and "rss_kb" in proc and "porcentaje_memoria" in proc
		pct_calculado = (proc["rss_kb"] / mem_total) * 100.0
		assert abs(proc["porcentaje_memoria"] - pct_calculado) <= 0.05
		if i > 0:
			ant = top[i - 1]
			assert (ant["rss_kb"] > proc["rss_kb"]) or (ant["rss_kb"] == proc["rss_kb"] and ant["pid"] <= proc["pid"])
	sys.exit(0)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_p" && echo "1" || echo "0")

if [ "$code_p" = "200" ] && [ "$CHECK_TOP" = "1" ]; then
	aprobar "/api/procesos respondió 200 con top <= 20 ordenado y porcentaje correcto"
else
	fallar "/api/procesos no cumplió contrato (código: ${code_p})"
fi

fase "3/16" "Proceso de prueba de 300 MB aparece en top en <= 2 intervalos"
python3 -I -c 'import time; a = bytearray(300 * 1024 * 1024); [a.__setitem__(i, 1) for i in range(0, len(a), 4096)]; time.sleep(60)' &
PID_300=$!

encontrado_300=0
for _ in $(seq 1 14); do
	sleep 0.5
	resp_actual=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/procesos")
	encontrado_300=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	target_pid = int(sys.argv[2])
	for p in d.get("top", []):
		if p.get("pid") == target_pid and p.get("rss_kb", 0) >= 250000:
			sys.exit(0)
	sys.exit(1)
except Exception:
	sys.exit(1)
' "$resp_actual" "$PID_300" && echo "1" || echo "0")
	if [ "$encontrado_300" = "1" ]; then
		break
	fi
done

if [ "$encontrado_300" = "1" ]; then
	aprobar "Proceso de 300 MB (PID ${PID_300}) apareció en top con RSS >= 250 MB"
else
	fallar "Proceso de 300 MB (PID ${PID_300}) no apareció en top a tiempo"
fi

fase "4/16" "SSE del común recibe event: procesos en <= intervalo + 1 s"
SSE_OUT=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_OUT" &
PID_SSE=$!

t_limite=$(($(date +%s) + 4))
recibido_sse_proc=0
while [ "$(date +%s)" -le "$t_limite" ]; do
	if grep -q "^event: procesos" "$SSE_OUT" 2>/dev/null; then
		recibido_sse_proc=1
		break
	fi
	sleep 0.2
done

kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

if [ "$recibido_sse_proc" = "1" ]; then
	aprobar "event: procesos recibido en SSE dentro del límite de tiempo"
else
	fallar "No se recibió event: procesos en SSE en <= 4 s"
fi

fase "5/16" "Común: intento de kill y toggle responden 403"
c_kill_comun=$(printf '{"pid":%d}' "$PID_300" | curl -s -b "$COOKIE_COMUN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")
c_mon_comun=$(printf '{"activo":false}' | curl -s -b "$COOKIE_COMUN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/monitoreo/procesos")

if [ "$c_kill_comun" = "403" ] && [ "$c_mon_comun" = "403" ]; then
	aprobar "Usuario común rechazado con 403 en kill y en toggle de procesos"
else
	fallar "Común no fue rechazado con 403 (kill: ${c_kill_comun}, toggle: ${c_mon_comun})"
fi

fase "6/16" "Admin: kill con cuerpos inválidos responde 400"
COOKIE_ADMIN=$(mktemp_rastreado)
login_json "so2t_admin" | curl -s -c "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/login"

c_k1=$(printf '{"pid":"abc"}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")
c_k2=$(printf '{"pid":1.5}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")
c_k3=$(printf '{"pid":0}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")

if [ "$c_k1" = "400" ] && [ "$c_k2" = "400" ] && [ "$c_k3" = "400" ]; then
	aprobar "Cuerpos con string, decimal y PID 0 respondieron 400"
else
	fallar "Respuestas de kill inválido inesperadas (${c_k1}, ${c_k2}, ${c_k3})"
fi

fase "7/16" "Admin: guardas de kill prohibidos responden 403 y procesos siguen vivos"
resp_k_pid1=$(printf '{"pid":1}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/proceso/kill")
code_k_pid1=$(printf '{"pid":1}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")

resp_k_daemon=$(printf '{"pid":%d}' "$PID_DAEMON" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/proceso/kill")
code_k_daemon=$(printf '{"pid":%d}' "$PID_DAEMON" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")

TID_DAEMON_CHILD=""
for t in /proc/"${PID_DAEMON}"/task/*; do
	tid=$(basename "$t")
	if [ "$tid" != "$PID_DAEMON" ]; then
		TID_DAEMON_CHILD="$tid"
		break
	fi
done

code_k_dhilo="403"
resp_k_dhilo='{"error":"No se permite terminar el propio daemon"}'
if [ -n "$TID_DAEMON_CHILD" ]; then
	resp_k_dhilo=$(printf '{"pid":%s}' "$TID_DAEMON_CHILD" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/proceso/kill")
	code_k_dhilo=$(printf '{"pid":%s}' "$TID_DAEMON_CHILD" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")
fi

resp_k_pid2=$(printf '{"pid":2}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/proceso/kill")
code_k_pid2=$(printf '{"pid":2}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")

PID_KWORKER=$(python3 -I -c '
import os
for entry in os.listdir("/proc"):
	if not entry.isdigit() or entry in ("1", "2"):
		continue
	try:
		with open(f"/proc/{entry}/stat") as f:
			content = f.read()
		idx = content.rfind(")")
		if idx != -1:
			rest = content[idx+2:].split()
			flags = int(rest[6])
			if flags & 0x00200000:
				print(entry)
				break
	except Exception:
		pass
')

code_k_kw="403"
resp_k_kw='{"error":"No se permite terminar hilos del kernel"}'
if [ -n "$PID_KWORKER" ]; then
	resp_k_kw=$(printf '{"pid":%s}' "$PID_KWORKER" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/proceso/kill")
	code_k_kw=$(printf '{"pid":%s}' "$PID_KWORKER" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")
fi

CHECK_GUARDAS=$(python3 -I -c '
import sys, json
try:
	r1 = json.loads(sys.argv[1]).get("error") == "No se permite terminar el proceso con PID 1"
	r2 = json.loads(sys.argv[2]).get("error") == "No se permite terminar el propio daemon"
	r3 = json.loads(sys.argv[3]).get("error") == "No se permite terminar el propio daemon"
	r4 = json.loads(sys.argv[4]).get("error") == "No se permite terminar hilos del kernel"
	r5 = json.loads(sys.argv[5]).get("error") == "No se permite terminar hilos del kernel"
	sys.exit(0 if (r1 and r2 and r3 and r4 and r5) else 1)
except Exception:
	sys.exit(1)
' "$resp_k_pid1" "$resp_k_daemon" "$resp_k_dhilo" "$resp_k_pid2" "$resp_k_kw" && echo "1" || echo "0")

if [ "$code_k_pid1" = "403" ] && [ "$code_k_daemon" = "403" ] && [ "$code_k_dhilo" = "403" ] && \
   [ "$code_k_pid2" = "403" ] && [ "$code_k_kw" = "403" ] && [ "$CHECK_GUARDAS" = "1" ] && \
   kill -0 1 2>/dev/null && kill -0 2 2>/dev/null && kill -0 "$PID_DAEMON" 2>/dev/null; then
	aprobar "Guardas de PID 1, daemon, hilo del daemon, PID 2 y kthread rechazadas con 403 y siguen vivos"
else
	fallar "Fallo en guardas de procesos protegidos (códigos: ${code_k_pid1}, ${code_k_daemon}, ${code_k_dhilo}, ${code_k_pid2}, ${code_k_kw})"
fi

fase "8/16" "Admin: kill de un hilo no líder responde 400"
python3 -I -c 'import threading, time; threading.Thread(target=lambda: time.sleep(60)).start(); time.sleep(60)' &
PID_MULTI=$!
sleep 0.5

TID_MULTI_CHILD=""
for t in /proc/"${PID_MULTI}"/task/*; do
	tid=$(basename "$t")
	if [ "$tid" != "$PID_MULTI" ]; then
		TID_MULTI_CHILD="$tid"
		break
	fi
done

code_k_noleader="000"
resp_k_noleader=""
if [ -n "$TID_MULTI_CHILD" ]; then
	resp_k_noleader=$(printf '{"pid":%s}' "$TID_MULTI_CHILD" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/proceso/kill")
	code_k_noleader=$(printf '{"pid":%s}' "$TID_MULTI_CHILD" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")
fi

kill -9 "$PID_MULTI" 2>/dev/null || true
wait "$PID_MULTI" 2>/dev/null || true
PID_MULTI=""

CHECK_NOLEADER=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	sys.exit(0 if d.get("error") == "El PID no corresponde a un proceso" else 1)
except Exception:
	sys.exit(1)
' "$resp_k_noleader" && echo "1" || echo "0")

if [ "$code_k_noleader" = "400" ] && [ "$CHECK_NOLEADER" = "1" ]; then
	aprobar "Kill sobre hilo no líder respondió 400 'El PID no corresponde a un proceso'"
else
	fallar "Kill sobre hilo no líder no respondió 400 esperado (${code_k_noleader})"
fi

fase "9/16" "Admin: kill de PID inexistente responde 404"
resp_k_noexiste=$(printf '{"pid":4194000}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/proceso/kill")
code_k_noexiste=$(printf '{"pid":4194000}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null -w '%{http_code}' "${BASE}/api/proceso/kill")

CHECK_NOEXISTE=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	sys.exit(0 if d.get("error") == "Proceso no encontrado" else 1)
except Exception:
	sys.exit(1)
' "$resp_k_noexiste" && echo "1" || echo "0")

if [ "$code_k_noexiste" = "404" ] && [ "$CHECK_NOEXISTE" = "1" ]; then
	aprobar "PID inexistente respondió 404 'Proceso no encontrado'"
else
	fallar "PID inexistente respondió ${code_k_noexiste}"
fi

fase "10/16" "Admin: kill del proceso de 300 MB responde 200 SIGKILL y emite alerta MEDIUM"
id_max_previo=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas" | python3 -I -c '
import sys, json
try:
	d = json.load(sys.stdin)
	alertas = d.get("alertas", [])
	print(alertas[0]["id"] if alertas else 0)
except Exception:
	print(0)
')
PID_OBJETIVO="$PID_300"
TMP_RESP_KILL=$(mktemp_rastreado)
code_k_300=$(printf '{"pid":%d}' "$PID_OBJETIVO" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o "$TMP_RESP_KILL" -w '%{http_code}' "${BASE}/api/proceso/kill")
resp_k_300=$(cat "$TMP_RESP_KILL")

sleep 0.5
muerto=0
if ! kill -0 "$PID_OBJETIVO" 2>/dev/null; then
	muerto=1
fi
PID_300=""

resp_alertas_kill=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas")
CHECK_KILL_OK=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	assert d.get("senal") == "SIGKILL"
	nombre = d.get("nombre", "")
	target_pid = int(sys.argv[2])
	id_previo = int(sys.argv[3])
	alertas_obj = json.loads(sys.argv[4])
	alertas = alertas_obj.get("alertas", [])
	nuevas_pid = [a for a in alertas if a.get("id", 0) > id_previo and f"(PID {target_pid})" in a.get("descripcion", "")]
	assert len(nuevas_pid) == 1
	a = nuevas_pid[0]
	assert a.get("motivo") == "Proceso matado"
	assert a.get("severidad") == "MEDIUM"
	desc = a.get("descripcion", "")
	assert nombre and nombre in desc
	assert f"(PID {target_pid})" in desc
	assert "so2t_admin" in desc
	sys.exit(0)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_k_300" "$PID_OBJETIVO" "$id_max_previo" "$resp_alertas_kill" && echo "1" || echo "0")

if [ "$code_k_300" = "200" ] && [ "$muerto" = "1" ] && [ "$CHECK_KILL_OK" = "1" ]; then
	aprobar "Proceso de 300 MB terminado con SIGKILL y alerta MEDIUM registrada"
else
	fallar "Fallo al matar proceso de 300 MB (código: ${code_k_300}, muerto: ${muerto})"
fi

fase "11/16" "Admin desactiva escaneo de procesos (activo: false)"
SSE_MON=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_MON" &
PID_SSE=$!
sleep 0.3

resp_desact_p=$(printf '{"activo":false}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/procesos")
sleep 0.5

kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

resp_mon_get=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/monitoreo")
resp_alertas_desact=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas")

CHECK_DESACT_P=$(python3 -I -c '
import sys, json
try:
	desact = json.loads(sys.argv[1])
	assert desact.get("procesos") == False and desact.get("cambio") == True
	mon = json.loads(sys.argv[2])
	assert "metricas" in mon and mon.get("procesos") == False
	alertas = json.loads(sys.argv[3]).get("alertas", [])
	encontrada = False
	for a in alertas:
		if a.get("motivo") == "Monitoreo del sistema desactivado" and a.get("severidad") == "MEDIUM" and "Escaneo de procesos desactivado por so2t_admin" in a.get("descripcion", ""):
			encontrada = True
			break
	assert encontrada
	with open(sys.argv[4]) as f:
		sse_content = f.read()
	assert "event: monitoreo" in sse_content and "\"procesos\":false" in sse_content
	sys.exit(0)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_desact_p" "$resp_mon_get" "$resp_alertas_desact" "$SSE_MON" && echo "1" || echo "0")

if [ "$CHECK_DESACT_P" = "1" ]; then
	aprobar "Escaneo desactivado: cambio true, alerta MEDIUM y SSE event: monitoreo emitido"
else
	fallar "Fallo al desactivar escaneo de procesos"
fi

fase "12/16" "Desactivado durante 4 s: sin event: procesos, /api/procesos activo false, métricas fluyen"
SSE_SILENCIO=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_SILENCIO" &
PID_SSE=$!

sleep 4
kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

resp_p_desact=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/procesos")
c_proc_sse=$(grep -c "^event: procesos" "$SSE_SILENCIO" || true)
c_met_sse=$(grep -c "^event: metricas" "$SSE_SILENCIO" || true)

CHECK_SILENCIO=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	assert d.get("activo") == False
	assert len(d.get("top", [])) > 0
	sys.exit(0)
except Exception:
	sys.exit(1)
' "$resp_p_desact" && echo "1" || echo "0")

if [ "$c_proc_sse" -eq 0 ] && [ "$c_met_sse" -ge 1 ] && [ "$CHECK_SILENCIO" = "1" ]; then
	aprobar "Durante 4 s no hubo event: procesos, /api/procesos con activo:false y métricas siguieron"
else
	fallar "Fallo en periodo desactivado (event: procesos count: ${c_proc_sse}, metricas: ${c_met_sse})"
fi

fase "13/16" "Desactivar otra vez responde cambio false sin alerta nueva"
max_alerta_antes=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas" | python3 -I -c '
import sys, json
d = json.load(sys.stdin)
print(d["alertas"][0]["id"] if d.get("alertas") else 0)
')
resp_desact_otra=$(printf '{"activo":false}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/procesos")
sleep 0.5
max_alerta_despues=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas" | python3 -I -c '
import sys, json
d = json.load(sys.stdin)
print(d["alertas"][0]["id"] if d.get("alertas") else 0)
')

CHECK_CAMBIO_FALSO=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	sys.exit(0 if (d.get("procesos") == False and d.get("cambio") == False) else 1)
except Exception:
	sys.exit(1)
' "$resp_desact_otra" && echo "1" || echo "0")

if [ "$CHECK_CAMBIO_FALSO" = "1" ] && [ "$max_alerta_antes" = "$max_alerta_despues" ]; then
	aprobar "Reiterar activo:false respondió cambio:false y no generó alerta adicional"
else
	fallar "Reiterar activo:false falló (alerta max antes: ${max_alerta_antes}, después: ${max_alerta_despues})"
fi

fase "14/16" "Activar escaneo de procesos emite alerta LOW y event: procesos en < 1 s"
SSE_REACT=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_REACT" &
PID_SSE=$!
sleep 0.3

t_inicio=$(date +%s%N)
printf '{"activo":true}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/monitoreo/procesos"

recibido_rapido=0
t_limite_ns=$((t_inicio + 1500000000))
while [ "$(date +%s%N)" -le "$t_limite_ns" ]; do
	if grep -q "^event: procesos" "$SSE_REACT" 2>/dev/null; then
		t_llegada=$(date +%s%N)
		diff_ms=$(( (t_llegada - t_inicio) / 1000000 ))
		if [ "$diff_ms" -lt 1000 ]; then
			recibido_rapido=1
		fi
		break
	fi
	sleep 0.05
done

kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

resp_alertas_act=$(curl -s -b "$COOKIE_ADMIN" "${BASE}/api/alertas")
CHECK_ACT_ALERT=$(python3 -I -c '
import sys, json
try:
	alertas = json.loads(sys.argv[1]).get("alertas", [])
	encontrada = False
	for a in alertas:
		if a.get("motivo") == "Monitoreo del sistema activado" and a.get("severidad") == "LOW" and "Escaneo de procesos activado por so2t_admin" in a.get("descripcion", ""):
			encontrada = True
			break
	sys.exit(0 if encontrada else 1)
except Exception:
	sys.exit(1)
' "$resp_alertas_act" && echo "1" || echo "0")

if [ "$recibido_rapido" = "1" ] && [ "$CHECK_ACT_ALERT" = "1" ]; then
	aprobar "Reactivación emitió alerta LOW y event: procesos llegó en < 1 s"
else
	fallar "Reactivación no cumplió tiempo < 1 s o alerta LOW"
fi

fase "15/16" "Desactivar métricas y procesos: hilo monitoreo no consume tics durante 5 s"
printf '{"activo":false}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/monitoreo/metricas"
printf '{"activo":false}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/monitoreo/procesos"
sleep 4

TID_MONITOREO=""
for t in /proc/"${PID_DAEMON}"/task/*; do
	if [ -f "$t/comm" ] && [ "$(< "$t/comm")" = "monitoreo" ]; then
		TID_MONITOREO=$(basename "$t")
		break
	fi
done

if [ -n "$TID_MONITOREO" ] && [ -f "/proc/${PID_DAEMON}/task/${TID_MONITOREO}/stat" ] && [ -f "/proc/${PID_DAEMON}/task/${TID_MONITOREO}/status" ]; then
	read -r _ _ _ _ _ _ _ _ _ _ _ _ _ utime1 stime1 _ < "/proc/${PID_DAEMON}/task/${TID_MONITOREO}/stat"
	tics1=$((utime1 + stime1))
	ctx1=$(awk '/^voluntary_ctxt_switches:/ {print $2}' "/proc/${PID_DAEMON}/task/${TID_MONITOREO}/status")
	sleep 5
	read -r _ _ _ _ _ _ _ _ _ _ _ _ _ utime2 stime2 _ < "/proc/${PID_DAEMON}/task/${TID_MONITOREO}/stat"
	tics2=$((utime2 + stime2))
	ctx2=$(awk '/^voluntary_ctxt_switches:/ {print $2}' "/proc/${PID_DAEMON}/task/${TID_MONITOREO}/status")
	diff_tics=$((tics2 - tics1))
	diff_ctx=$((ctx2 - ctx1))
	if [ "$diff_tics" -eq 0 ] && [ "$diff_ctx" -eq 0 ]; then
		aprobar "Hilo monitoreo (TID ${TID_MONITOREO}) consumió 0 tics y 0 cambios de contexto en 5 s"
	else
		fallar "Hilo monitoreo no estuvo en reposo (tics=${diff_tics}, ctxt_switches=${diff_ctx})"
	fi
else
	fallar "No se pudo localizar el hilo monitoreo del daemon"
fi

fase "16/16" "Reactivar ambos, salida limpia sin contraseñas y SIGTERM"
printf '{"activo":true}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/monitoreo/metricas"
printf '{"activo":true}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/monitoreo/procesos"
sleep 0.5

PASS_ENCONTRADA=0
for u in so2t_admin so2t_comun so2t_ambos so2t_nadie; do
	p="${CLAVE[$u]:-}"
	if [ -n "$p" ] && grep -F -q "$p" "${DIR}/daemon.out"; then
		PASS_ENCONTRADA=1
		break
	fi
done

if [ "$PASS_ENCONTRADA" -eq 0 ]; then
	aprobar "Ninguna contraseña de prueba aparece en la salida del daemon"
else
	fallar "Se encontró al menos una contraseña en ${DIR}/daemon.out"
fi

kill -TERM "$PID_DAEMON" 2>/dev/null || true
wait "$PID_DAEMON"
codigo_salida=$?
PID_DAEMON=""

if [ "$codigo_salida" -eq 0 ]; then
	aprobar "Daemon finalizó limpiamente con código 0 ante SIGTERM"
else
	fallar "Daemon terminó con código ${codigo_salida} en lugar de 0"
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "  Resumen: ${GREEN}${APROBADAS} APROBADAS${NC} · ${RED}${FALLIDAS} FALLIDAS${NC}"
echo -e "${BLUE}====================================================================${NC}"

if [ "$FALLIDAS" -gt 0 ]; then
	exit 1
fi
exit 0
