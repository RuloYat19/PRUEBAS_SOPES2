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
PUERTO=8096
BASE="http://127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR="${USER_HOME}/so2_logs/pruebas_metricas"

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE=""
PID_SLEEP=""
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
	if [ -n "$PID_SLEEP" ]; then
		kill -9 "$PID_SLEEP" 2>/dev/null || true
		wait "$PID_SLEEP" 2>/dev/null || true
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
echo -e "${BLUE}       PRUEBAS DE MÉTRICAS DEL SISTEMA - PROYECTO ÚNICO SO2         ${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/15" "Condiciones previas y preparación de usuarios"
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
intervalo = 1
dashboard = ${DASHBOARD_DIR}
log_central = ${DIR}/central.txt
log_rutas = ${DIR}/a.log, ${DIR}/b.log
log_palabras = METRICAS_TEST
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

fase "1/15" "GET /api/metricas sin cookie"
c1=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/api/metricas")
if [ "$c1" = "401" ]; then
	aprobar "Rechaza sin cookie con 401"
else
	fallar "Respondio ${c1} en lugar de 401"
fi

fase "2/15" "Común: /api/metricas responde 200, activo true, intervalo_s 1 y >= 1 muestra"
COOKIE_COMUN=$(mktemp_rastreado)
login_json "so2t_comun" | curl -s -c "$COOKIE_COMUN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/login"
sleep 1.2
resp_m=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/metricas")
code_m=$(curl -s -b "$COOKIE_COMUN" -o /dev/null -w '%{http_code}' "${BASE}/api/metricas")

CAMPOS_VALIDOS=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	activo = d.get("activo") == True
	intervalo = d.get("intervalo_s") == 1
	muestras = d.get("muestras", [])
	tiene_muestras = len(muestras) >= 1
	campos = ["id", "timestamp", "primera", "mem_total", "mem_libre", "mem_disponible",
	          "buffers", "cached", "activas", "inactivas", "swap_total", "swap_libre",
	          "usada", "cache", "libre", "swap_usada", "pgfault", "pgmajfault",
	          "pgfault_delta", "pgmajfault_delta"]
	todos_campos = all(all(k in m for k in campos) for m in muestras)
	sys.exit(0 if (activo and intervalo and tiene_muestras and todos_campos) else 1)
except Exception:
	sys.exit(1)
' "$resp_m" && echo "1" || echo "0")

if [ "$code_m" = "200" ] && [ "$CAMPOS_VALIDOS" = "1" ]; then
	aprobar "/api/metricas respondió 200 con campos completos y activo:true, intervalo_s:1"
else
	fallar "/api/metricas falló (código: ${code_m})"
fi

fase "3/15" "Última muestra contra /proc/meminfo en host"
CHECK_MEM=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	muestras = d.get("muestras", [])
	if not muestras:
		sys.exit(1)
	m = muestras[-1]
	meminfo = {}
	with open("/proc/meminfo") as f:
		for line in f:
			parts = line.split(":")
			if len(parts) == 2:
				val = parts[1].strip().split()[0]
				meminfo[parts[0].strip()] = int(val)

	assert m["mem_total"] == meminfo["MemTotal"]
	assert m["swap_total"] == meminfo["SwapTotal"]

	tolerancia_kb = 20480
	for k, mk in [("mem_libre", "MemFree"), ("cached", "Cached"), ("activas", "Active"), ("inactivas", "Inactive")]:
		val_m = m[k]
		val_proc = meminfo[mk]
		tol = max(tolerancia_kb, int(val_proc * 0.05))
		assert abs(val_m - val_proc) <= tol

	sys.exit(0)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_m" && echo "1" || echo "0")

if [ "$CHECK_MEM" = "1" ]; then
	aprobar "mem_total y swap_total exactos; mem_libre, cached, activas, inactivas dentro de tolerancia"
else
	fallar "Valores de meminfo difieren más allá de la tolerancia"
fi

fase "4/15" "Todas las muestras cumplen usada + cache + libre == mem_total y deltas >= 0"
CHECK_DERIV=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	muestras = d.get("muestras", [])
	for m in muestras:
		assert m["usada"] + m["cache"] + m["libre"] == m["mem_total"]
		assert m["pgfault_delta"] >= 0
		assert m["pgmajfault_delta"] >= 0
	sys.exit(0)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_m" && echo "1" || echo "0")

if [ "$CHECK_DERIV" = "1" ]; then
	aprobar "Derivados suman mem_total y deltas son no negativos"
else
	fallar "Discrepancia en suma de memoria o deltas negativos"
fi

fase "5/15" "SSE del común: 3 event: metricas consecutivos en <= 4 s"
SSE_OUT=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_OUT" &
PID_SSE=$!

t_inicio=$(date +%s%N)
t_fin=$((t_inicio + 4000000000))
consecutivos=0
while [ "$(date +%s%N)" -lt "$t_fin" ]; do
	consecutivos=$(python3 -I -c '
import sys, re
try:
	with open(sys.argv[1]) as f:
		txt = f.read()
	ids = [int(m) for m in re.findall(r"id:\s*(\d+)\s*\nevent:\s*metricas", txt)]
	for i in range(len(ids) - 2):
		if ids[i+1] == ids[i] + 1 and ids[i+2] == ids[i+1] + 1:
			sys.exit(0)
	sys.exit(1)
except Exception:
	sys.exit(1)
' "$SSE_OUT" && echo "1" || echo "0")
	if [ "$consecutivos" = "1" ]; then
		break
	fi
	sleep 0.1
done
kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

if [ "$consecutivos" = "1" ]; then
	aprobar "3 eventos metricas recibidos con ids consecutivos en <= 4 s"
else
	fallar "No se recibieron 3 eventos metricas consecutivos a tiempo"
fi

fase "6/15" "Ráfaga de memoria genera pgfault_delta >= 10000"
id_antes=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/metricas" | python3 -I -c '
import sys, json
d = json.load(sys.stdin)
print(d["muestras"][-1]["id"] if d.get("muestras") else 0)
')

python3 -I -c '
b = bytearray(200 * 1024 * 1024)
for i in range(0, len(b), 4096):
	b[i] = 1
'

sleep 2.2
resp_post=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/metricas")
FALLOS_DETECTADOS=$(python3 -I -c '
import sys, json
try:
	d = json.loads(sys.argv[1])
	id_antes = int(sys.argv[2])
	posteriores = [m for m in d.get("muestras", []) if m["id"] > id_antes]
	hay_alta = any(m.get("pgfault_delta", 0) >= 10000 for m in posteriores)
	sys.exit(0 if hay_alta else 1)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_post" "$id_antes" && echo "1" || echo "0")

if [ "$FALLOS_DETECTADOS" = "1" ]; then
	aprobar "pgfault_delta >= 10000 detectado tras tocar 200 MB"
else
	fallar "No se registró pgfault_delta >= 10000 tras tocar 200 MB"
fi

fase "7/15" "Común: POST /api/monitoreo/metricas responde 403"
c_comun=$(printf '{"activo":false}' | curl -s -b "$COOKIE_COMUN" -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/metricas")
if [ "$c_comun" = "403" ]; then
	aprobar "Común recibe 403 en POST /api/monitoreo/metricas"
else
	fallar "Común recibió ${c_comun} en lugar de 403"
fi

fase "8/15" "Admin: validación de cuerpo en POST /api/monitoreo/metricas"
COOKIE_ADMIN=$(mktemp_rastreado)
login_json "so2t_admin" | curl -s -c "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/login"

c_bad_val=$(printf '{"activo":"no"}' | curl -s -b "$COOKIE_ADMIN" -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/metricas")
c_bad_json=$(printf '{no es json' | curl -s -b "$COOKIE_ADMIN" -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/metricas")

if [ "$c_bad_val" = "400" ] && [ "$c_bad_json" = "400" ]; then
	aprobar "Rechaza con 400 valor no booleano y JSON inválido"
else
	fallar "Respuestas inesperadas: ${c_bad_val} y ${c_bad_json}"
fi

fase "9/15" "Admin desactiva métricas (activo: false)"
SSE_MON=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_MON" &
PID_SSE=$!
sleep 0.3

resp_desact=$(printf '{"activo":false}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/metricas")
sleep 0.5
kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

resp_get_mon=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/monitoreo")
resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")

CHECK_DESACT=$(python3 -I -c '
import sys, json
try:
	d_post = json.loads(sys.argv[1])
	d_get = json.loads(sys.argv[2])
	d_alertas = json.loads(sys.argv[3])
	with open(sys.argv[4]) as f:
		txt_sse = f.read()

	assert d_post.get("metricas") == False and d_post.get("cambio") == True
	assert d_get.get("metricas") == False
	assert "event: monitoreo" in txt_sse and "\"metricas\":false" in txt_sse

	alerta_ok = any(
		a.get("motivo") == "Monitoreo del sistema desactivado" and
		a.get("severidad") == "MEDIUM" and
		"Métricas del sistema desactivadas por so2t_admin" in a.get("descripcion", "")
		for a in d_alertas.get("alertas", [])
	)
	assert alerta_ok
	sys.exit(0)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_desact" "$resp_get_mon" "$resp_alertas" "$SSE_MON" && echo "1" || echo "0")

if [ "$CHECK_DESACT" = "1" ]; then
	aprobar "Desactivación exitosa: respuesta 200, alerta MEDIUM, event: monitoreo y GET /api/monitoreo false"
else
	fallar "Fallo en verificación de desactivación"
fi

fase "10/15" "Durante 3 s desactivado no hay eventos metricas ni nuevas muestras"
id_antes=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/metricas" | python3 -I -c '
import sys, json
d = json.load(sys.stdin)
print(d["muestras"][-1]["id"] if d.get("muestras") else 0)
')

SSE_SILENCIO=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_SILENCIO" &
PID_SSE=$!
sleep 3
kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

id_despues=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/metricas" | python3 -I -c '
import sys, json
d = json.load(sys.stdin)
print(d["muestras"][-1]["id"] if d.get("muestras") else 0)
')

if [ "$id_antes" = "$id_despues" ] && ! grep -q "event: metricas" "$SSE_SILENCIO"; then
	aprobar "Sin muestras nuevas ni eventos SSE de métricas durante 3 s"
else
	fallar "Se tomaron muestras o se emitieron eventos SSE estando desactivado"
fi

fase "11/15" "Procesos se siguen actualizando en /api/estado estando desactivado"
sleep 30 &
PID_SLEEP=$!

aparece=0
t_inicio=$(date +%s%N)
t_fin=$((t_inicio + 3000000000))
while [ "$(date +%s%N)" -lt "$t_fin" ]; do
	resp_est=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/estado")
	if python3 -I -c '
import sys, json
d = json.loads(sys.argv[1])
pid = int(sys.argv[2])
procs = [p["pid"] for p in d.get("procesos", [])]
sys.exit(0 if pid in procs else 1)
' "$resp_est" "$PID_SLEEP" 2>/dev/null; then
		aparece=1
		break
	fi
	sleep 0.2
done
kill -9 "$PID_SLEEP" 2>/dev/null || true
wait "$PID_SLEEP" 2>/dev/null || true
PID_SLEEP=""

if [ "$aparece" = "1" ]; then
	aprobar "PID de sleep aparece en /api/estado en <= 3 s"
else
	fallar "PID de sleep no apareció en /api/estado a tiempo"
fi

fase "12/15" "Re-desactivación idempotente no emite alerta"
max_alerta_antes=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -I -c '
import sys, json
d = json.load(sys.stdin)
print(d["alertas"][0]["id"] if d.get("alertas") else 0)
')

resp_idem=$(printf '{"activo":false}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/metricas")

max_alerta_despues=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -I -c '
import sys, json
d = json.load(sys.stdin)
print(d["alertas"][0]["id"] if d.get("alertas") else 0)
')

if grep -q '"cambio":false' <<< "$resp_idem" && [ "$max_alerta_antes" = "$max_alerta_despues" ]; then
	aprobar "Responde cambio: false y no genera nueva alerta"
else
	fallar "Fallo en idempotencia: ${resp_idem}"
fi

fase "13/15" "Reactivación de métricas y muestra inmediata con primera: true (intervalo = 5)"
kill -TERM "$PID_DAEMON" 2>/dev/null || true
wait "$PID_DAEMON" 2>/dev/null || true
sed -i 's/^intervalo = .*/intervalo = 5/' "${DIR}/daemon.conf"
"${DAEMON}" -c "${DIR}/daemon.conf" >> "${DIR}/daemon.out" 2>&1 &
PID_DAEMON=$!
sleep 1
for _ in $(seq 50); do
	[ "$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/login.html")" = "200" ] && break
	sleep 0.1
done

COOKIE_COMUN=$(mktemp_rastreado)
COOKIE_ADMIN=$(mktemp_rastreado)
login_json "so2t_comun" | curl -s -c "$COOKIE_COMUN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/login"
login_json "so2t_admin" | curl -s -c "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- -o /dev/null "${BASE}/api/login"

printf '{"activo":false}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/metricas" >/dev/null
sleep 6

SSE_REACT=$(mktemp_rastreado)
curl -N -s -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$SSE_REACT" &
PID_SSE=$!
for _ in $(seq 30); do
	grep -q ": conectado" "$SSE_REACT" && break
	sleep 0.05
done

t_inicio=$(date +%s%N)
printf '{"activo":true}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d @- "${BASE}/api/monitoreo/metricas" >/dev/null

t_fin=$((t_inicio + 1000000000))
recibido_rapido=0
while [ "$(date +%s%N)" -lt "$t_fin" ]; do
	if grep -q "event: metricas" "$SSE_REACT"; then
		recibido_rapido=1
		break
	fi
	sleep 0.05
done
kill "$PID_SSE" 2>/dev/null || true
wait "$PID_SSE" 2>/dev/null || true
PID_SSE=""

resp_alertas=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas")
CHECK_REACT=$(python3 -I -c '
import sys, json, re
try:
	d_alertas = json.loads(sys.argv[1])
	with open(sys.argv[2]) as f:
		txt_sse = f.read()

	alerta_ok = any(
		a.get("motivo") == "Monitoreo del sistema activado" and
		a.get("severidad") == "LOW" and
		"Métricas del sistema activadas por so2t_admin" in a.get("descripcion", "")
		for a in d_alertas.get("alertas", [])
	)
	assert alerta_ok

	m = re.search(r"event:\s*metricas\s*\ndata:\s*(\{.*?\})\n", txt_sse)
	assert m
	datos_m = json.loads(m.group(1))
	assert datos_m.get("primera") == True
	sys.exit(0)
except Exception as e:
	print(e, file=sys.stderr)
	sys.exit(1)
' "$resp_alertas" "$SSE_REACT" && echo "1" || echo "0")

if [ "$recibido_rapido" = "1" ] && [ "$CHECK_REACT" = "1" ]; then
	aprobar "Con intervalo=5, alerta LOW emitida y primer event: metricas llegó en < 1 s con primera: true"
else
	fallar "Fallo en reactivación rápida o en alerta/primera: true"
fi

fase "14/15" "Salida del daemon sin contraseñas de prueba"
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

fase "15/15" "Paro limpio con SIGTERM (código 0)"
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
