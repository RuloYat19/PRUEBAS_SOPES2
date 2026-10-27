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
PUERTO=8094
SLASH="/"
BASE="http:${SLASH}${SLASH}127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR_A="${USER_HOME}/so2_vigilado/pruebas_sensor"
DIR_B="${USER_HOME}/so2_vigilado/pruebas_sensor_b"

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE=""
TEMP_FILES=()
PREPARADO=0

aprobar() { echo -e "  ${GREEN}✓ $*${NC}"; APROBADAS=$((APROBADAS + 1)); }
fallar()  { echo -e "  ${RED}✗ $*${NC}"; FALLIDAS=$((FALLIDAS + 1)); }

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
	if [ "$PREPARADO" -eq 1 ]; then
		rm -rf "$DIR_A" "$DIR_B" 2>/dev/null || true
	fi
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

contar_eventos() {
	local offset="$1"
	local evento="$2"
	local filtro="${3:--}"
	python3 -c '
import sys, json

offset = int(sys.argv[1])
target_ev = sys.argv[2]
expr = sys.argv[3]
log_path = sys.argv[4]

try:
    with open(log_path, "rb") as f:
        f.seek(offset)
        raw = f.read().decode("utf-8", errors="replace")
except Exception:
    print(0)
    sys.exit(0)

blocks = raw.split("\n\n")
c = 0
for b in blocks:
    lines = [l.strip() for l in b.splitlines() if l.strip()]
    ev = ""
    data_str = ""
    for l in lines:
        if l.startswith("event:"):
            ev = l.split(":", 1)[1].strip()
        elif l.startswith("data:"):
            data_str = l.split(":", 1)[1].strip()
    if target_ev and target_ev != "*" and ev != target_ev:
        continue
    if not ev and not target_ev:
        continue
    if not expr or expr == "-":
        c += 1
        continue
    d = {}
    if data_str:
        try:
            d = json.loads(data_str)
        except Exception:
            d = {}
    try:
        if eval(expr, {"__builtins__": None, "d": d, "str": str, "int": int, "bool": bool, "len": len, "ev": ev, "data": data_str}):
            c += 1
    except Exception:
        pass

print(c)
' "$offset" "$evento" "$filtro" "$LOG_SSE"
}

esperar_evento() {
	local offset="$1"
	local evento="$2"
	local filtro="${3:--}"
	local max_s="${4:-2}"
	local pasos=$((max_s * 20))
	for _ in $(seq 1 "$pasos"); do
		if [ "$(contar_eventos "$offset" "$evento" "$filtro")" -gt 0 ]; then
			return 0
		fi
		sleep 0.05
	done
	return 1
}

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}       PRUEBAS DE SENSOR EBPF E INTEGRIDAD (F05) - SO2              ${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/18" "Condiciones previas y preparación de entorno"
if [[ "$(uname -r)" != *so2-202100106* ]]; then
	fallar "Se requiere kernel 6.12.69-so2-202100106 (actual: $(uname -r))"
	exit 1
fi
aprobar "Kernel en ejecución: $(uname -r)"

if [ ! -d "${USER_HOME}/so2_vigilado" ]; then
	sudo -u "$REAL_USER" mkdir -p "${USER_HOME}/so2_vigilado"
fi

if [ -e "$DIR_A" ] || [ -e "$DIR_B" ]; then
	fallar "DIR_A (${DIR_A}) o DIR_B (${DIR_B}) ya existen; elimina a mano con rm -rf ${DIR_A} ${DIR_B}"
	exit 1
fi

if ! usuarios_prueba_condiciones; then
	fallar "Grupos o PAM no configurados"
	exit 1
fi
usuarios_prueba_crear || { fallar "No se pudieron crear usuarios de prueba"; exit 1; }
aprobar "Usuarios de prueba creados"

rm -rf "$DIR_A" "$DIR_B" 2>/dev/null || true
PREPARADO=1
sudo -u "$REAL_USER" mkdir -p "${DIR_A}/sub" "${DIR_B}"
echo "contenido base 1" | sudo -u "$REAL_USER" tee "${DIR_A}/base1.txt" > /dev/null
echo "contenido base 2" | sudo -u "$REAL_USER" tee "${DIR_A}/base2.txt" > /dev/null
aprobar "Directorios y archivos base preparados bajo ~/so2_vigilado"

CONF_FILE=$(mktemp_rastreado)
LOG_DAEMON=$(mktemp_rastreado)
COOKIE_ADMIN=$(mktemp_rastreado)
COOKIE_COMUN=$(mktemp_rastreado)
LOG_SSE=$(mktemp_rastreado)

cat <<EOF > "$CONF_FILE"
puerto=${PUERTO}
intervalo=1
dashboard=${DASHBOARD_DIR}
ruta_vigilada=${DIR_A}
EOF

"$DAEMON" -c "$CONF_FILE" > "$LOG_DAEMON" 2>&1 &
PID_DAEMON=$!

ESPERA=0
while [ $ESPERA -lt 50 ]; do
	if curl -s -o /dev/null "${BASE}/api/estado"; then
		break
	fi
	sleep 0.1
	ESPERA=$((ESPERA + 1))
done

if ! curl -s -o /dev/null "${BASE}/api/estado"; then
	fallar "El daemon no respondió en el puerto ${PUERTO}"
	exit 1
fi
aprobar "Daemon iniciado (PID ${PID_DAEMON}) en puerto ${PUERTO}"

login_stdin "so2t_admin" "$COOKIE_ADMIN"
login_stdin "so2t_comun" "$COOKIE_COMUN"
aprobar "Sesiones iniciadas para so2t_admin y so2t_comun"

fase "1/18" "Arranque: estado de monitoreo de archivos"
RESP_MON=$(mktemp_rastreado)
HTTP_CODE=$(curl -s -b "$COOKIE_COMUN" -w "%{http_code}" -o "$RESP_MON" "${BASE}/api/monitoreo")
if [ "$HTTP_CODE" = "200" ] && python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("archivos") is True
assert d.get("ruta_vigilada") == sys.argv[2]
assert d.get("sensor_error") == ""
' "$RESP_MON" "${DIR_A}"; then
	aprobar "Arranque correcto: archivos=true, ruta=${DIR_A}, sensor_error=''"
else
	fallar "Arranque incorrecto en /api/monitoreo"
fi

fase "2/18" "Verificación en kernel con bpftool"
PROG_SHOW=$(bpftool prog show 2>/dev/null || true)
if echo "$PROG_SHOW" | grep -q "ent_openat" && echo "$PROG_SHOW" | grep -q "liberar_archivo"; then
	aprobar "Programas eBPF cargados en kernel: ent_openat y liberar_archivo presentes"
else
	fallar "Programas eBPF ausentes en bpftool prog show"
fi

MAP_CFG=$(bpftool map dump name config_sensor 2>/dev/null || true)
if echo "$MAP_CFG" | grep -q "${DIR_A}/"; then
	aprobar "Mapa config_sensor contiene la ruta vigilada con barra final"
else
	fallar "Mapa config_sensor no contiene ${DIR_A}/"
fi

MAP_PID=$(bpftool map dump name pid_daemon 2>/dev/null || true)
if echo "$MAP_PID" | grep -q "$PID_DAEMON"; then
	aprobar "Mapa pid_daemon contiene el PID del daemon (${PID_DAEMON})"
else
	fallar "Mapa pid_daemon no contiene PID ${PID_DAEMON}"
fi

fase "3/18" "Análisis inicial de archivos existentes"
RESP_ARCH=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ARCH" "${BASE}/api/archivos"
HASH1=$(sha256sum "${DIR_A}/base1.txt" | cut -d' ' -f1)
HASH2=$(sha256sum "${DIR_A}/base2.txt" | cut -d' ' -f1)
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
archs = {a["nombre"]: a for a in d.get("archivos", [])}
assert "base1.txt" in archs and "base2.txt" in archs
assert archs["base1.txt"]["accion"] == "inicial"
assert archs["base2.txt"]["accion"] == "inicial"
assert archs["base1.txt"]["sha256"] == sys.argv[2]
assert archs["base2.txt"]["sha256"] == sys.argv[3]
' "$RESP_ARCH" "$HASH1" "$HASH2"; then
	aprobar "Análisis inicial contiene base1.txt y base2.txt con acción 'inicial' y hashes exactos"
else
	fallar "Análisis inicial no coincide con archivos base"
fi

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
for a in d.get("alertas", []):
    assert a.get("motivo") != "Archivo modificado"
' "$RESP_ALER"; then
	aprobar "Ninguna alerta 'Archivo modificado' generada durante el análisis inicial"
else
	fallar "Se emitieron alertas durante el análisis inicial"
fi

fase "4/18" "Permisos de endpoints de archivos"
CODE_SIN=$(curl -s -o /dev/null -w "%{http_code}" "${BASE}/api/archivos")
CODE_GET=$(curl -s -b "$COOKIE_COMUN" -o /dev/null -w "%{http_code}" "${BASE}/api/archivos")
BODY_TMP=$(mktemp_rastreado)
CODE_POST_MON=$(curl -s -b "$COOKIE_COMUN" -X POST -H "Content-Type: application/json" -d '{"activo":false}' -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/monitoreo/archivos")
CODE_POST_RUTA=$(curl -s -b "$COOKIE_COMUN" -X POST -H "Content-Type: application/json" -d "{\"ruta\":\"${DIR_B}\"}" -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/ruta")

if [ "$CODE_SIN" = "401" ] && [ "$CODE_GET" = "200" ] && [ "$CODE_POST_MON" = "403" ] && [ "$CODE_POST_RUTA" = "403" ]; then
	aprobar "Permisos validados: sin cookie 401, GET comun 200, POSTs comun 403"
else
	fallar "Permisos incorrectos (sin=$CODE_SIN, get=$CODE_GET, mon=$CODE_POST_MON, ruta=$CODE_POST_RUTA)"
fi

fase "5/18" "Creación de archivo por proceso de usuario"
curl -s -N -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$LOG_SSE" 2>&1 &
PID_SSE=$!
sleep 0.5

PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')

T_INICIO=$(python3 -c "import time; print(time.time())")
PY_OUT=$(sudo -u "$REAL_USER" python3 -I -c "import os; print(os.getpid()); f=open('${DIR_A}/creado.txt', 'w'); f.write('nuevo contenido'); f.close()")
PY_PID=$(echo "$PY_OUT" | head -n1)

T_EVENTO=""
for _ in $(seq 1 40); do
	if [ "$(contar_eventos 0 "archivo" "d.get('nombre') == 'creado.txt'")" -gt 0 ]; then
		T_EVENTO=$(python3 -c "import time; print(time.time())")
		break
	fi
	sleep 0.05
done

sleep 0.5

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
target_pid = int(sys.argv[3])
target_path = sys.argv[4] + "/creado.txt"
found = False
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        assert a.get("severidad") == "LOW"
        assert a.get("tipo") == "archivos"
        assert a.get("descripcion") == f"El archivo {target_path} fue creado por el PID {target_pid} (python3)"
        found = True
        break
assert found
' "$RESP_ALER" "$PREV_ALERTA_ID" "$PY_PID" "${DIR_A}"; then
	aprobar "Alerta LOW emitida con texto exacto para creación por PID ${PY_PID} (python3)"
else
	fallar "Alerta de creación ausente o incorrecta"
fi

RESP_ARCH=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ARCH" "${BASE}/api/archivos"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
target_pid = int(sys.argv[2])
archs = {a["nombre"]: a for a in d.get("archivos", [])}
assert "creado.txt" in archs
assert archs["creado.txt"]["accion"] == "creado"
assert archs["creado.txt"]["pid"] == target_pid
' "$RESP_ARCH" "$PY_PID"; then
	aprobar "Listado de archivos contiene creado.txt con acción 'creado' y PID ${PY_PID}"
else
	fallar "Listado de archivos no refleja entrada creado.txt"
fi

LATENCIA=$(python3 -c "
t1 = float('$T_INICIO')
t2 = float('$T_EVENTO') if '$T_EVENTO' else 0.0
if t2 > 0:
    print(f'{t2 - t1:.3f}')
else:
    print('-1')
")

if [ -n "$T_EVENTO" ] && python3 -c "import sys; sys.exit(0 if float('$LATENCIA') <= 1.0 else 1)"; then
	aprobar "Canal SSE recibió event: archivo para creado.txt en ${LATENCIA} s (<= 1 s)"
else
	fallar "Canal SSE no recibió event: archivo para creado.txt en <= 1 s (latencia: ${LATENCIA} s)"
fi

fase "6/18" "Modificación por redirección de shell"
PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')

sudo -u "$REAL_USER" bash -c "echo 'cambio linea' >> ${DIR_A}/base1.txt"
HASH_POST=$(sha256sum "${DIR_A}/base1.txt" | cut -d' ' -f1)

sleep 1.5

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
if python3 -c '
import json, sys, re
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
target_path = sys.argv[3] + "/base1.txt"
found = False
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        assert a.get("severidad") == "LOW"
        assert re.match(r"^El archivo " + re.escape(target_path) + r" fue modificado por el PID \d+ \(bash\)$", a.get("descripcion", ""))
        found = True
        break
assert found
' "$RESP_ALER" "$PREV_ALERTA_ID" "${DIR_A}"; then
	aprobar "Alerta emitida para modificación de base1.txt por PID (bash)"
else
	fallar "Alerta de modificación de base1.txt ausente o incorrecta"
fi

RESP_ARCH=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ARCH" "${BASE}/api/archivos"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
archs = {a["nombre"]: a for a in d.get("archivos", [])}
assert archs["base1.txt"]["accion"] == "modificado"
assert archs["base1.txt"]["sha256"] == sys.argv[2]
' "$RESP_ARCH" "$HASH_POST"; then
	aprobar "Hash de base1.txt coincide con contenido final tras liberación"
else
	fallar "Hash en listado no coincide con archivo modificado"
fi

fase "7/18" "Prevención de bucles de auto-monitoreo"
sleep 3
RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
COUNT_BASE1=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
target_path = sys.argv[3] + "/base1.txt"
c = 0
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado" and target_path in a.get("descripcion", ""):
        c += 1
print(c)
' "$RESP_ALER" "$PREV_ALERTA_ID" "${DIR_A}")

COUNT_SSE_BASE1=$(contar_eventos 0 "archivo" "d.get('nombre') == 'base1.txt'")

if [ "$COUNT_BASE1" = "1" ] && [ "$COUNT_SSE_BASE1" = "1" ]; then
	aprobar "Sin bucle: exactamente 1 alerta y 1 event: archivo para base1.txt tras 3 s de reposo"
else
	fallar "Se detectó auto-monitoreo (alertas=${COUNT_BASE1}, event:archivo=${COUNT_SSE_BASE1})"
fi

fase "8/18" "Operaciones de solo lectura descartadas"
PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')
SSE_LEN_PREV=$(wc -c < "$LOG_SSE")

sudo -u "$REAL_USER" cat "${DIR_A}/base1.txt" > /dev/null
sudo -u "$REAL_USER" python3 -I -c "f=open('${DIR_A}/base1.txt', 'r'); f.read(); f.close()"
sleep 2

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
NUEVAS_READ=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
c = 0
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        c += 1
print(c)
' "$RESP_ALER" "$PREV_ALERTA_ID")

NUEVOS_SSE_ARCH=$(contar_eventos "$SSE_LEN_PREV" "archivo" "-")

if [ "$NUEVAS_READ" = "0" ] && [ "$NUEVOS_SSE_ARCH" = "0" ]; then
	aprobar "Lecturas (cat, python r) ignoradas por el sensor: 0 alertas nuevas y 0 event: archivo"
else
	fallar "Operación de lectura generó alerta ($NUEVAS_READ) o event: archivo ($NUEVOS_SSE_ARCH)"
fi

fase "9/18" "Apertura en modo escritura sin cambios"
PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')

sudo -u "$REAL_USER" python3 -I -c "f=open('${DIR_A}/base1.txt', 'r+'); f.close()"
sleep 2

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
NUEVAS_RPLUS=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
c = 0
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        c += 1
print(c)
' "$RESP_ALER" "$PREV_ALERTA_ID")

if [ "$NUEVAS_RPLUS" = "0" ]; then
	aprobar "Apertura r+ sin cambio de contenido ni mtime no emite alerta (SIN_CAMBIO)"
else
	fallar "Apertura sin cambios generó alerta"
fi

fase "10/18" "Operaciones fuera de alcance (subdirectorios y otra ruta)"
PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')

echo "fuera sub" | sudo -u "$REAL_USER" tee "${DIR_A}/sub/x.txt" > /dev/null
echo "fuera b" | sudo -u "$REAL_USER" tee "${DIR_B}/y.txt" > /dev/null
sleep 2

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
NUEVAS_FUERA=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
c = 0
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        c += 1
print(c)
' "$RESP_ALER" "$PREV_ALERTA_ID")

if [ "$NUEVAS_FUERA" = "0" ]; then
	aprobar "Archivos en subdirectorio y en otra carpeta descartados correctamente"
else
	fallar "Escrituras fuera de alcance generaron alerta"
fi

fase "11/18" "Enlaces simbólicos descartados"
PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')

sudo -u "$REAL_USER" ln -s "${DIR_B}/y.txt" "${DIR_A}/enlace_y"
echo "modifico enlace" | sudo -u "$REAL_USER" tee "${DIR_A}/enlace_y" > /dev/null
sleep 2

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
NUEVAS_ENLACE=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
c = 0
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        c += 1
print(c)
' "$RESP_ALER" "$PREV_ALERTA_ID")

RESP_ARCH=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ARCH" "${BASE}/api/archivos"
TIENE_ENLACE=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
nombres = [a.get("nombre") for a in d.get("archivos", [])]
print(1 if "enlace_y" in nombres else 0)
' "$RESP_ARCH")

if [ "$NUEVAS_ENLACE" = "0" ] && [ "$TIENE_ENLACE" = "0" ]; then
	aprobar "Enlace simbólico ignorado: sin alerta y sin entrada en listado"
else
	fallar "Enlace simbólico no fue ignorado correctamente"
fi

fase "12/18" "Desactivación del escaneo por administrador"
PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')
SSE_LEN_PREV=$(wc -c < "$LOG_SSE")

RESP_DESACT=$(mktemp_rastreado)
HTTP_CODE=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d '{"activo":false}' -w "%{http_code}" -o "$RESP_DESACT" "${BASE}/api/monitoreo/archivos")

if [ "$HTTP_CODE" = "200" ] && python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("archivos") is False
assert d.get("cambio") is True
' "$RESP_DESACT"; then
	aprobar "POST /api/monitoreo/archivos falso respondió 200 con archivos=false y cambio=true"
else
	fallar "Respuesta inesperada al desactivar escaneo"
fi

if esperar_evento "$SSE_LEN_PREV" "monitoreo" "d.get('archivos') is False" 2; then
	aprobar "Canal SSE recibió event: monitoreo con archivos=false"
else
	fallar "Canal SSE no recibió event: monitoreo con archivos=false"
fi

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
found = False
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Monitoreo del sistema desactivado":
        assert a.get("severidad") == "MEDIUM"
        assert a.get("descripcion") == "Escaneo de archivos desactivado por so2t_admin"
        found = True
        break
assert found
' "$RESP_ALER" "$PREV_ALERTA_ID"; then
	aprobar "Alerta MEDIUM emitida: Escaneo de archivos desactivado por so2t_admin"
else
	fallar "Alerta de desactivación no emitida"
fi

PROG_SHOW_OFF=$(bpftool prog show 2>/dev/null || true)
if ! echo "$PROG_SHOW_OFF" | grep -q "ent_openat" && ! echo "$PROG_SHOW_OFF" | grep -q "liberar_archivo"; then
	aprobar "Programas eBPF descargados completamente del kernel"
else
	fallar "Programas eBPF siguen presentes tras desactivación"
fi

MAP_SHOW_OFF=$(bpftool map show 2>/dev/null || true)
if ! echo "$MAP_SHOW_OFF" | grep -q "config_sensor"; then
	aprobar "Mapas eBPF destruidos del kernel"
else
	fallar "Mapa config_sensor sigue presente tras desactivación"
fi

echo "escritura apagada" | sudo -u "$REAL_USER" tee "${DIR_A}/apagado.txt" > /dev/null
sleep 1.5

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
NUEVAS_OFF=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
c = 0
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        c += 1
print(c)
' "$RESP_ALER" "$PREV_ALERTA_ID")

RESP_ARCH=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ARCH" "${BASE}/api/archivos"
ACTIVO_VAL=$(python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("activo") is False
assert len(d.get("archivos", [])) > 0
print(1)
' "$RESP_ARCH")

if [ "$NUEVAS_OFF" = "0" ] && [ "$ACTIVO_VAL" = "1" ]; then
	aprobar "Escritura con sensor desactivado no genera alertas; /api/archivos conserva lista anterior con activo=false"
else
	fallar "Comportamiento erróneo durante escaneo desactivado"
fi

fase "13/18" "Desactivar de nuevo (idempotencia)"
RESP_DESACT2=$(mktemp_rastreado)
HTTP_CODE=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d '{"activo":false}' -w "%{http_code}" -o "$RESP_DESACT2" "${BASE}/api/monitoreo/archivos")

if [ "$HTTP_CODE" = "200" ] && python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("archivos") is False
assert d.get("cambio") is False
' "$RESP_DESACT2"; then
	aprobar "Desactivar nuevamente retorna cambio=false sin alertas"
else
	fallar "Desactivación repetida no retornó cambio=false"
fi

fase "14/18" "Reactivación del escaneo e incorporación de archivo escrito en reposo"
PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')
SSE_LEN_PREV=$(wc -c < "$LOG_SSE")

RESP_ACT=$(mktemp_rastreado)
HTTP_CODE=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d '{"activo":true}' -w "%{http_code}" -o "$RESP_ACT" "${BASE}/api/monitoreo/archivos")

if [ "$HTTP_CODE" = "200" ] && python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("archivos") is True
assert d.get("cambio") is True
' "$RESP_ACT"; then
	aprobar "Activación exitosa: archivos=true, cambio=true"
else
	fallar "Fallo en activación de sensor"
fi

if esperar_evento "$SSE_LEN_PREV" "archivos" "d.get('activo') is True and 'archivos' in d" 2; then
	aprobar "Canal SSE recibió event: archivos con listado tras reactivación"
else
	fallar "Canal SSE no recibió event: archivos tras reactivación"
fi

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
found = False
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Monitoreo del sistema activado":
        assert a.get("severidad") == "LOW"
        assert a.get("descripcion") == "Escaneo de archivos activado por so2t_admin"
        found = True
        break
assert found
' "$RESP_ALER" "$PREV_ALERTA_ID"; then
	aprobar "Alerta LOW emitida: Escaneo de archivos activado por so2t_admin"
else
	fallar "Alerta de reactivación no emitida"
fi

PROG_SHOW_ON=$(bpftool prog show 2>/dev/null || true)
if echo "$PROG_SHOW_ON" | grep -q "ent_openat"; then
	aprobar "Programas eBPF recargados en el kernel"
else
	fallar "Programas eBPF no se recargaron al kernel"
fi

RESP_ARCH=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ARCH" "${BASE}/api/archivos"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
archs = {a["nombre"]: a for a in d.get("archivos", [])}
assert "apagado.txt" in archs
assert archs["apagado.txt"]["accion"] == "inicial"
' "$RESP_ARCH"; then
	aprobar "Archivo escrito durante reposo (apagado.txt) incorporado con acción 'inicial' sin alerta"
else
	fallar "apagado.txt no fue incorporado correctamente en análisis inicial"
fi

fase "15/18" "Validación de rutas inválidas en POST /api/ruta"
C_REL=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d '{"ruta":"relativa"}' -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/ruta")
ERR_REL=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("error",""))' "$BODY_TMP")

C_ROOT=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d '{"ruta":"/"}' -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/ruta")
ERR_ROOT=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("error",""))' "$BODY_TMP")

C_NUM=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d '{"ruta":5}' -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/ruta")
ERR_NUM=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("error",""))' "$BODY_TMP")

C_NOEX=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d "{\"ruta\":\"${DIR_A}/no_existe_so2_test\"}" -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/ruta")
ERR_NOEX=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("error",""))' "$BODY_TMP")

C_REG=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d "{\"ruta\":\"${DIR_A}/base1.txt\"}" -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/ruta")
ERR_REG=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("error",""))' "$BODY_TMP")

SUB_LARGO="${DIR_A}/n1_$(python3 -c 'print("a"*90)')/n2_$(python3 -c 'print("b"*90)')"
sudo -u "$REAL_USER" mkdir -p "$SUB_LARGO"
CANON_LARGO=$(realpath "$SUB_LARGO")
LEN_CANON=${#CANON_LARGO}

if [ "$LEN_CANON" -le 200 ]; then
	fallar "Ruta canónica de prueba no supera 200 caracteres ($LEN_CANON)"
	exit 1
fi

C_LARGA=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d "{\"ruta\":\"${SUB_LARGO}\"}" -w "%{http_code}" -o "$BODY_TMP" "${BASE}/api/ruta")
ERR_LARGA=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("error",""))' "$BODY_TMP")

if [ "$C_REL" = "400" ] && [ "$ERR_REL" = "Ruta invalida" ] && \
   [ "$C_ROOT" = "400" ] && [ "$ERR_ROOT" = "Ruta invalida" ] && \
   [ "$C_NUM" = "400" ] && [ "$ERR_NUM" = "Ruta invalida" ] && \
   [ "$C_NOEX" = "400" ] && [ "$ERR_NOEX" = "La ruta no es un directorio existente" ] && \
   [ "$C_REG" = "400" ] && [ "$ERR_REG" = "La ruta no es un directorio existente" ] && \
   [ "$C_LARGA" = "400" ] && [ "$ERR_LARGA" = "Ruta demasiado larga" ]; then
	aprobar "Validación exhaustiva de rutas 400: 'Ruta invalida', 'La ruta no es un directorio existente' y 'Ruta demasiado larga'"
else
	fallar "Respuestas de validación de ruta no coinciden con especificación"
fi

fase "16/18" "Cambio de ruta vigilada en caliente"
SSE_LEN_PREV=$(wc -c < "$LOG_SSE")
RESP_CH=$(mktemp_rastreado)
HTTP_CODE=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d "{\"ruta\":\"${DIR_B}/./\"}" -w "%{http_code}" -o "$RESP_CH" "${BASE}/api/ruta")

if [ "$HTTP_CODE" = "200" ] && python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("ruta") == sys.argv[2]
assert d.get("cambio") is True
' "$RESP_CH" "${DIR_B}"; then
	aprobar "Cambio de ruta responde 200 con canónica limpia y cambio=true"
else
	fallar "Fallo al cambiar ruta a ${DIR_B}/./"
fi

OK_MON=0
OK_ARCH=0
if esperar_evento "$SSE_LEN_PREV" "monitoreo" "d.get('ruta_vigilada') == '${DIR_B}'" 2; then
	OK_MON=1
fi
if esperar_evento "$SSE_LEN_PREV" "archivos" "d.get('ruta') == '${DIR_B}'" 2; then
	OK_ARCH=1
fi

if [ "$OK_MON" = "1" ] && [ "$OK_ARCH" = "1" ]; then
	aprobar "Canal SSE recibió event: monitoreo con ruta nueva (${DIR_B}) y event: archivos"
else
	fallar "Canal SSE no recibió eventos esperados tras cambio de ruta (mon=$OK_MON, arch=$OK_ARCH)"
fi

MAP_CFG_B=$(bpftool map dump name config_sensor 2>/dev/null || true)
if echo "$MAP_CFG_B" | grep -q "${DIR_B}/"; then
	aprobar "config_sensor actualizado en kernel a ${DIR_B}/"
else
	fallar "config_sensor no se actualizó a ${DIR_B}/"
fi

PREV_ALERTA_ID=$(curl -s -b "$COOKIE_COMUN" "${BASE}/api/alertas" | python3 -c '
import json, sys
d = json.load(sys.stdin)
ids = [a["id"] for a in d.get("alertas", []) if "id" in a]
print(max(ids) if ids else 0)
')

echo "en viejo" | sudo -u "$REAL_USER" tee "${DIR_A}/no_vigilado.txt" > /dev/null
echo "en nuevo" | sudo -u "$REAL_USER" tee "${DIR_B}/vigilado.txt" > /dev/null
sleep 2

RESP_ALER=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ALER" "${BASE}/api/alertas"
if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
prev_id = int(sys.argv[2])
target_nuevo = sys.argv[3] + "/vigilado.txt"
target_viejo = sys.argv[4] + "/no_vigilado.txt"
found_nuevo = False
found_viejo = False
for a in d.get("alertas", []):
    if a.get("id", 0) > prev_id and a.get("motivo") == "Archivo modificado":
        desc = a.get("descripcion", "")
        if target_nuevo in desc:
            found_nuevo = True
        if target_viejo in desc:
            found_viejo = True
assert found_nuevo and not found_viejo
' "$RESP_ALER" "$PREV_ALERTA_ID" "${DIR_B}" "${DIR_A}"; then
	aprobar "Directorio nuevo vigilado genera alerta; directorio viejo ignorado"
else
	fallar "Disparos de alertas incorrectos tras cambio de ruta"
fi

RESP_CH2=$(mktemp_rastreado)
HTTP_CODE=$(curl -s -b "$COOKIE_ADMIN" -X POST -H "Content-Type: application/json" -d "{\"ruta\":\"${DIR_B}\"}" -w "%{http_code}" -o "$RESP_CH2" "${BASE}/api/ruta")
if [ "$HTTP_CODE" = "200" ] && python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("cambio") is False
' "$RESP_CH2"; then
	aprobar "Misma ruta nuevamente retorna cambio=false sin reiniciar"
else
	fallar "Misma ruta no retornó cambio=false"
fi

fase "17/18" "Comportamiento en reposo del hilo archivos (CPU ticks y cambios de contexto)"
sleep 1
TID_ARCHIVOS=""
for task in /proc/"${PID_DAEMON}"/task/[0-9]*; do
	comm_file="${task}/comm"
	if [ -f "$comm_file" ] && [ "$(cat "$comm_file")" = "archivos" ]; then
		TID_ARCHIVOS=$(basename "$task")
		break
	fi
done

if [ -n "$TID_ARCHIVOS" ] && [ -f "/proc/${PID_DAEMON}/task/${TID_ARCHIVOS}/stat" ] && [ -f "/proc/${PID_DAEMON}/task/${TID_ARCHIVOS}/status" ]; then
	TICS_INICIAL=$(awk '{print $14 + $15}' "/proc/${PID_DAEMON}/task/${TID_ARCHIVOS}/stat")
	CTX_INICIAL=$(awk '/^voluntary_ctxt_switches:/ {print $2}' "/proc/${PID_DAEMON}/task/${TID_ARCHIVOS}/status")
	sleep 5
	TICS_FINAL=$(awk '{print $14 + $15}' "/proc/${PID_DAEMON}/task/${TID_ARCHIVOS}/stat")
	CTX_FINAL=$(awk '/^voluntary_ctxt_switches:/ {print $2}' "/proc/${PID_DAEMON}/task/${TID_ARCHIVOS}/status")
	DIFF_TICS=$((TICS_FINAL - TICS_INICIAL))
	DIFF_CTX=$((CTX_FINAL - CTX_INICIAL))
	if [ "$DIFF_TICS" -eq 0 ] && [ "$DIFF_CTX" -eq 0 ]; then
		aprobar "Hilo archivos en reposo durante 5 s sin consumir CPU (0 ticks, 0 ctxt switches)"
	else
		fallar "Hilo archivos no estuvo en reposo (tics=${DIFF_TICS}, ctxt_switches=${DIFF_CTX})"
	fi
else
	fallar "No se encontró el hilo archivos del daemon para medir ticks"
fi

fase "18/18" "Paro limpio con SIGTERM y limpieza de kernel"
if [ -n "$PID_SSE" ]; then
	kill "$PID_SSE" 2>/dev/null || true
	wait "$PID_SSE" 2>/dev/null || true
	PID_SSE=""
fi

kill -TERM "$PID_DAEMON"
wait "$PID_DAEMON"
CODE_TERM=$?
PID_DAEMON=""

if [ "$CODE_TERM" = "0" ]; then
	aprobar "Daemon finalizó limpiamente con código 0"
else
	fallar "Daemon terminó con código ${CODE_TERM}"
fi

PROG_FINAL=$(bpftool prog show 2>/dev/null || true)
if ! echo "$PROG_FINAL" | grep -q "ent_openat" && ! echo "$PROG_FINAL" | grep -q "liberar_archivo"; then
	aprobar "Sensor eBPF descargado completamente al terminar daemon"
else
	fallar "Sensor eBPF quedó en memoria tras SIGTERM"
fi

if ! grep -q -i "password" "$LOG_DAEMON" && ! grep -q "libbpf: Error" "$LOG_DAEMON"; then
	aprobar "Log no contiene contraseñas ni errores de libbpf"
else
	fallar "Log contiene información sensible o errores inesperados"
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${BLUE}  RESUMEN DE PRUEBAS DE SENSOR EBPF (F05)${NC}"
echo -e "${BLUE}====================================================================${NC}"
echo -e "  Aprobadas: ${GREEN}${APROBADAS}${NC}"
echo -e "  Fallidas:  ${RED}${FALLIDAS}${NC}"

if [ "$FALLIDAS" -eq 0 ]; then
	echo -e "\n${GREEN}✓ TODAS LAS PRUEBAS DE LA FEATURE F05 PASARON EXITOSAMENTE${NC}"
	exit 0
else
	echo -e "\n${RED}✗ SE ENCONTRARON FALLAS EN LA FEATURE F05${NC}"
	exit 1
fi
