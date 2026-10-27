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
PUERTO=8093
SLASH="/"
BASE="http:${SLASH}${SLASH}127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR_VIGILADO="${USER_HOME}/so2_vigilado/pruebas_antivirus"
DIR_LOGS="${USER_HOME}/so2_logs/pruebas_antivirus"

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE=""
TEMP_FILES=()
PREPARADO=0
SHA_REPO_INICIAL=""

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
		rm -rf "$DIR_VIGILADO" "$DIR_LOGS" 2>/dev/null || true
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

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}       PRUEBAS DE ANTIVIRUS, BLACKLIST Y CUARENTENA (F06)           ${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/13" "Condiciones previas y verificación de entorno"
if [[ "$(uname -r)" != *so2-202100106* ]]; then
	fallar "Se requiere kernel 6.12.69-so2-202100106 (actual: $(uname -r))"
	exit 1
fi
aprobar "Kernel en ejecución: $(uname -r)"

SHA_REPO_INICIAL=$(sha256sum "${REPO_DIR}/ProgramaIntermedio/blacklist.json" | cut -d' ' -f1)

if [ -e "$DIR_VIGILADO" ] || [ -e "$DIR_LOGS" ]; then
	fallar "DIR_VIGILADO (${DIR_VIGILADO}) o DIR_LOGS (${DIR_LOGS}) ya existen; elimina a mano con rm -rf ${DIR_VIGILADO} ${DIR_LOGS}"
	exit 1
fi

if ! usuarios_prueba_condiciones; then
	fallar "Grupos o PAM no configurados"
	exit 1
fi
usuarios_prueba_crear || { fallar "No se pudieron crear usuarios de prueba"; exit 1; }
aprobar "Usuarios de prueba creados"

PREPARADO=1
sudo -u "$REAL_USER" mkdir -p "$DIR_VIGILADO" "$DIR_LOGS"
sudo -u "$REAL_USER" cp "${REPO_DIR}/ProgramaIntermedio/blacklist.json" "${DIR_LOGS}/blacklist.json"

sudo -u "$REAL_USER" bash -c "printf 'SO2-MUESTRA-MALICIOSA-3\n' > '${DIR_VIGILADO}/muestra_previa.txt'"
aprobar "Directorio vigilado, copia de blacklist y muestra_previa.txt creados"

CONF_FILE=$(mktemp_rastreado)
LOG_DAEMON=$(mktemp_rastreado)
COOKIE_ADMIN=$(mktemp_rastreado)
COOKIE_COMUN=$(mktemp_rastreado)
LOG_SSE=$(mktemp_rastreado)

cat <<EOF > "$CONF_FILE"
puerto = ${PUERTO}
intervalo = 1
dashboard = ${DASHBOARD_DIR}
ruta_vigilada = ${DIR_VIGILADO}
blacklist = ${DIR_LOGS}/blacklist.json
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

curl -s -N -b "$COOKIE_COMUN" "${BASE}/api/eventos" > "$LOG_SSE" 2>&1 &
PID_SSE=$!

fase "1/13" "Arranque con muestra previa en el directorio (análisis inicial)"
sleep 1.5
MODO_PREV=$(stat -c %a "${DIR_VIGILADO}/muestra_previa.txt" 2>/dev/null || echo "error")
if [ "$MODO_PREV" = "0" ]; then
	aprobar "muestra_previa.txt puesta en cuarentena (permisos 0000)"
else
	fallar "muestra_previa.txt tiene permisos ${MODO_PREV}, esperado 0"
fi

RESP_ALERTAS=$(mktemp_rastreado)
curl -s -b "$COOKIE_ADMIN" -o "$RESP_ALERTAS" "${BASE}/api/alertas"
if python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
als = data.get("alertas", [])
h = any(a.get("severidad") == "HIGH" and "(análisis inicial)" in a.get("descripcion", "") and "SO2.Demo.Keylogger" in a.get("descripcion", "") for a in als)
c = any(a.get("severidad") == "HIGH" and "cuarentena" in a.get("descripcion", "") and "muestra_previa.txt" in a.get("descripcion", "") for a in als)
assert h, "Falta alerta HIGH de deteccion inicial"
assert c, "Falta alerta HIGH de cuarentena inicial"
' "$RESP_ALERTAS"; then
	aprobar "Alertas HIGH generadas con '(análisis inicial)' y puesta en cuarentena"
else
	fallar "No se encontraron las alertas esperadas para la muestra previa"
fi

fase "2/13" "GET /api/blacklist según rol"
RESP_BL=$(mktemp_rastreado)
C_ADMIN=$(curl -s -b "$COOKIE_ADMIN" -w "%{http_code}" -o "$RESP_BL" "${BASE}/api/blacklist")
C_COMUN=$(curl -s -b "$COOKIE_COMUN" -w "%{http_code}" -o /dev/null "${BASE}/api/blacklist")
C_NOCLK=$(curl -s -w "%{http_code}" -o /dev/null "${BASE}/api/blacklist")

if [ "$C_ADMIN" = "200" ] && [ "$C_COMUN" = "403" ] && [ "$C_NOCLK" = "401" ] && python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("firmas") == 4 or len(d.get("lista", [])) == 4
assert d.get("origen") == "archivo"
' "$RESP_BL"; then
	aprobar "GET /api/blacklist: admin 200 (4 firmas, origen archivo), común 403, sin cookie 401"
else
	fallar "Respuestas de GET /api/blacklist no coinciden: admin=$C_ADMIN, comun=$C_COMUN, nocookie=$C_NOCLK"
fi

fase "3/13" "Detección y cuarentena de Muestra 1 creada con python3"
PID_PY=$(sudo -u "$REAL_USER" python3 -I -c '
import os, sys
p = sys.argv[1]
with open(p, "w") as f:
    f.write("SO2-MUESTRA-MALICIOSA-1\n")
print(os.getpid())
' "${DIR_VIGILADO}/muestra1.txt")

sleep 1.5
MODO_M1=$(stat -c %a "${DIR_VIGILADO}/muestra1.txt" 2>/dev/null || echo "error")
if [ "$MODO_M1" = "0" ]; then
	aprobar "muestra1.txt en permisos 0000"
else
	fallar "muestra1.txt tiene permisos ${MODO_M1}, esperado 0"
fi

if sudo -u "$REAL_USER" cat "${DIR_VIGILADO}/muestra1.txt" >/dev/null 2>&1; then
	fallar "Lectura de muestra1.txt no fue bloqueada"
else
	aprobar "Lectura como usuario normal denegada por permisos 0000"
fi

curl -s -b "$COOKIE_ADMIN" -o "$RESP_ALERTAS" "${BASE}/api/alertas"
if python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
als = data.get("alertas", [])
pid_py = sys.argv[2]
h = any(a.get("severidad") == "HIGH" and "SO2.Demo.Troyano" in a.get("descripcion", "") and ("PID " + pid_py) in a.get("descripcion", "") for a in als)
c = any(a.get("severidad") == "HIGH" and "cuarentena" in a.get("descripcion", "") and "muestra1.txt" in a.get("descripcion", "") for a in als)
m = any(a.get("motivo") == "Archivo modificado" and "muestra1.txt" in a.get("descripcion", "") for a in als)
assert h, "Falta alerta de hash malicioso con PID y nombre de firma"
assert c, "Falta alerta de cuarentena automatica"
assert not m, "Se emitio alerta de Archivo modificado indebidamente para archivo infectado"
' "$RESP_ALERTAS" "$PID_PY"; then
	aprobar "Alertas emitidas: Hash malicioso detectado (con PID) y Cuarentena; sin Archivo modificado"
else
	fallar "Verificación de alertas de Muestra 1 no cumplida"
fi

fase "4/13" "Listado, amenazas y SSE"
RESP_ARC=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_ARC" "${BASE}/api/archivos"
RESP_AM=$(mktemp_rastreado)
curl -s -b "$COOKIE_COMUN" -o "$RESP_AM" "${BASE}/api/amenazas"

if python3 -c '
import json, sys
arcs = json.load(open(sys.argv[1])).get("archivos", [])
ams = json.load(open(sys.argv[2])).get("amenazas", [])
a1 = next((a for a in arcs if "muestra1.txt" in a.get("ruta", "")), None)
assert a1 is not None, "muestra1.txt no esta en listado"
assert a1.get("estado") == "infectado", "estado no es infectado"
assert a1.get("firma") == "SO2.Demo.Troyano", "firma no coincide"
assert a1.get("cuarentena") is True, "cuarentena no es true"
assert any(am.get("firma") == "SO2.Demo.Troyano" for am in ams), "amenaza no presente en /api/amenazas"
' "$RESP_ARC" "$RESP_AM"; then
	aprobar "Listado y /api/amenazas reflejan muestra1.txt como infectado y en cuarentena"
else
	fallar "Estado de muestra1.txt en listado o amenazas incorrecto"
fi

if grep -q "event: amenaza" "$LOG_SSE" && grep -q "SO2.Demo.Troyano" "$LOG_SSE"; then
	aprobar "Evento SSE 'amenaza' recibido en tiempo real"
else
	fallar "No se registró evento SSE 'amenaza' en el flujo del usuario común"
fi

fase "5/13" "GET /api/cuarentena según rol"
RESP_CUAR=$(mktemp_rastreado)
C_ADMIN_Q=$(curl -s -b "$COOKIE_ADMIN" -w "%{http_code}" -o "$RESP_CUAR" "${BASE}/api/cuarentena")
C_COMUN_Q=$(curl -s -b "$COOKIE_COMUN" -w "%{http_code}" -o /dev/null "${BASE}/api/cuarentena")

if [ "$C_ADMIN_Q" = "200" ] && [ "$C_COMUN_Q" = "403" ] && python3 -c '
import json, sys
q = json.load(open(sys.argv[1])).get("cuarentena", [])
r = next((item for item in q if "muestra1.txt" in item.get("ruta", "")), None)
assert r is not None, "muestra1.txt no esta en cuarentena"
assert r.get("tipo") == "automatica", "tipo no es automatica"
assert r.get("motivo") == "SO2.Demo.Troyano", "motivo no es SO2.Demo.Troyano"
assert r.get("permisos_anteriores") == "644", "permisos_anteriores no son 644"
' "$RESP_CUAR"; then
	aprobar "GET /api/cuarentena: admin 200 con registro automático válido, común 403"
else
	fallar "Verificación de /api/cuarentena falló"
fi

fase "6/13" "Archivo benigno no genera alertas maliciosas ni altera permisos"
sudo -u "$REAL_USER" bash -c "printf 'contenido benigno de prueba\n' > '${DIR_VIGILADO}/benigno.txt'"
sleep 1.5
MODO_BEN=$(stat -c %a "${DIR_VIGILADO}/benigno.txt" 2>/dev/null || echo "error")
if [ "$MODO_BEN" = "644" ]; then
	aprobar "benigno.txt conserva permisos 644 intactos"
else
	fallar "benigno.txt permisos cambiados a ${MODO_BEN}"
fi

curl -s -b "$COOKIE_ADMIN" -o "$RESP_ALERTAS" "${BASE}/api/alertas"
if python3 -c '
import json, sys
als = json.load(open(sys.argv[1])).get("alertas", [])
assert not any("benigno.txt" in a.get("descripcion", "") and a.get("severidad") == "HIGH" for a in als), "Alerta HIGH indebida en archivo benigno"
' "$RESP_ALERTAS"; then
	aprobar "Sin alertas HIGH para archivo benigno"
else
	fallar "Se detectó alerta HIGH para archivo benigno"
fi

fase "7/13" "Subidas inválidas a /api/blacklist (R1.3)"
SHA_BL_PRE=$(sha256sum "${DIR_LOGS}/blacklist.json" | cut -d' ' -f1)

probar_subida_invalida() {
	local cuerpo="$1"
	local texto_esperado="$2"
	local out=$(mktemp_rastreado)
	local code=$(printf "%s" "$cuerpo" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o "$out" "${BASE}/api/blacklist")
	if [ "$code" = "400" ] && grep -q "Blacklist invalida: ${texto_esperado}" "$out"; then
		return 0
	fi
	return 1
}

if probar_subida_invalida '{"nombre":' "JSON mal formado" && \
   probar_subida_invalida '{"nombre":"base"}' "Falta el arreglo firmas" && \
   probar_subida_invalida '{"firmas":[{"sha256":"abc","nombre":"f","descripcion":"d"}]}' "Firma 1: sha256 invalido" && \
   probar_subida_invalida '{"firmas":[{"sha256":"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660","nombre":"f1","descripcion":"d1"},{"sha256":"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660","nombre":"f2","descripcion":"d2"}]}' "Firma 2: sha256 duplicado" && \
   probar_subida_invalida '{"clave_desconocida":1,"firmas":[{"sha256":"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660","nombre":"t","descripcion":"d"}]}' "Clave desconocida: clave_desconocida" && \
   probar_subida_invalida '{"firmas":[]}' "firmas debe tener entre 1 y 5000 elementos"; then
	aprobar "Los 6 casos de subida inválida devolvieron 400 con los textos exactos de R1.3"
else
	fallar "Uno o más casos de subida inválida no devolvieron el error esperado"
fi

SHA_BL_POST=$(sha256sum "${DIR_LOGS}/blacklist.json" | cut -d' ' -f1)
if [ "$SHA_BL_PRE" = "$SHA_BL_POST" ]; then
	aprobar "Archivo blacklist.json en disco intacto tras intentos inválidos"
else
	fallar "blacklist.json fue alterado tras intentos inválidos"
fi

fase "8/13" "Límites y control de acceso en la subida"
C_SUB_COMUN=$(printf '{"firmas":[]}' | curl -s -b "$COOKIE_COMUN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/blacklist")
C_SUB_NOCLK=$(printf '{"firmas":[]}' | curl -s -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/blacklist")
C_SUB_GRANDE=$(head -c 1048577 /dev/zero | tr '\0' 'A' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/blacklist")
C_RUTA_8K=$(head -c 8192 /dev/zero | tr '\0' 'A' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/ruta")

if [ "$C_SUB_COMUN" = "403" ] && [ "$C_SUB_NOCLK" = "401" ] && [ "$C_SUB_GRANDE" = "413" ] && [ "$C_RUTA_8K" = "413" ]; then
	aprobar "Límites verificados: común 403, sin cookie 401, subida > 1 MiB 413, /api/ruta con 8 KiB 413"
else
	fallar "Respuestas de límites no coinciden: comun=$C_SUB_COMUN, nocookie=$C_SUB_NOCLK, grande=$C_SUB_GRANDE, ruta8k=$C_RUTA_8K"
fi

fase "9/13" "Subida válida (5 firmas) y re-evaluación automática de benigno.txt"
HASH_BENIGNO=$(sha256sum "${DIR_VIGILADO}/benigno.txt" | cut -d' ' -f1)
JSON_5F=$(python3 -c '
import json, sys
orig = json.load(open(sys.argv[1]))
orig["firmas"].append({
    "sha256": sys.argv[2],
    "nombre": "SO2.Demo.BenignoDetectado",
    "descripcion": "Firma agregada para reevaluacion"
})
orig["version"] = orig.get("version", 1) + 1
print(json.dumps(orig))
' "${DIR_LOGS}/blacklist.json" "$HASH_BENIGNO")

RESP_SUB=$(mktemp_rastreado)
C_SUB=$(printf "%s" "$JSON_5F" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o "$RESP_SUB" "${BASE}/api/blacklist")

if [ "$C_SUB" = "200" ] && grep -q '"firmas":5' "$RESP_SUB"; then
	aprobar "Subida válida aceptada: 200 OK con 5 firmas"
else
	fallar "Subida válida rechazada o sin 5 firmas: HTTP $C_SUB"
fi

sleep 1.5
MODO_BEN_POST=$(stat -c %a "${DIR_VIGILADO}/benigno.txt" 2>/dev/null || echo "error")
if [ "$MODO_BEN_POST" = "0" ]; then
	aprobar "benigno.txt puesto en cuarentena (0000) por re-evaluación"
else
	fallar "benigno.txt no fue puesto en cuarentena tras actualización de blacklist (modo ${MODO_BEN_POST})"
fi

curl -s -b "$COOKIE_ADMIN" -o "$RESP_ALERTAS" "${BASE}/api/alertas"
if python3 -c '
import json, sys
als = json.load(open(sys.argv[1])).get("alertas", [])
act = any(a.get("motivo") == "Actualización base de datos local" and "5 firmas" in a.get("descripcion", "") for a in als)
reev = any(a.get("severidad") == "HIGH" and "(actualización de blacklist)" in a.get("descripcion", "") and "benigno.txt" in a.get("descripcion", "") for a in als)
assert act, "Falta alerta de actualizacion de base de firmas"
assert reev, "Falta alerta HIGH con (actualización de blacklist)"
' "$RESP_ALERTAS"; then
	aprobar "Alertas emitidas: Actualización base de datos local y re-evaluación"
else
	fallar "Alertas de actualización y re-evaluación no encontradas"
fi

DUE_POST=$(stat -c %U "${DIR_LOGS}/blacklist.json")
MOD_POST=$(stat -c %a "${DIR_LOGS}/blacklist.json")
if [ "$DUE_POST" = "$REAL_USER" ] && [ "$MOD_POST" = "644" ]; then
	aprobar "blacklist.json en disco preserva dueño (${REAL_USER}) y permisos (644)"
else
	fallar "blacklist.json dueño=${DUE_POST} permisos=${MOD_POST}"
fi

fase "10/13" "Cuarentena manual"
sudo -u "$REAL_USER" bash -c "printf 'archivo para prueba de cuarentena manual\n' > '${DIR_VIGILADO}/manual.txt'"
sleep 0.5

C_MAN_COMUN=$(printf '{"ruta":"%s/manual.txt"}' "$DIR_VIGILADO" | curl -s -b "$COOKIE_COMUN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/cuarentena")
if [ "$C_MAN_COMUN" = "403" ]; then
	aprobar "Cuarentena manual rechazada a común con 403"
else
	fallar "Cuarentena manual a común devolvió $C_MAN_COMUN, esperado 403"
fi

C_MAN_ADMIN=$(printf '{"ruta":"%s/manual.txt"}' "$DIR_VIGILADO" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/cuarentena")
MODO_MAN=$(stat -c %a "${DIR_VIGILADO}/manual.txt" 2>/dev/null || echo "error")
if [ "$C_MAN_ADMIN" = "200" ] && [ "$MODO_MAN" = "0" ]; then
	aprobar "Cuarentena manual por admin exitosa (200 OK, modo 0000)"
else
	fallar "Cuarentena manual admin falló: code=$C_MAN_ADMIN, modo=$MODO_MAN"
fi

C_MAN_YA=$(printf '{"ruta":"%s/manual.txt"}' "$DIR_VIGILADO" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/cuarentena")
if [ "$C_MAN_YA" = "409" ]; then
	aprobar "Cuarentena manual sobre archivo ya en cuarentena devuelve 409 Conflict"
else
	fallar "Cuarentena manual repetida devolvió $C_MAN_YA, esperado 409"
fi

MODO_HOST_INI=$(stat -c %a /etc/hostname 2>/dev/null || echo "644")
C_MAN_FUERA=$(printf '{"ruta":"/etc/hostname"}' | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/cuarentena")
MODO_HOST_FIN=$(stat -c %a /etc/hostname 2>/dev/null || echo "644")
if [ "$C_MAN_FUERA" = "404" ] && [ "$MODO_HOST_INI" = "$MODO_HOST_FIN" ]; then
	aprobar "Cuarentena sobre ruta fuera de vigilancia (/etc/hostname) da 404 y permisos intactos"
else
	fallar "Cuarentena sobre ruta fuera devolvió $C_MAN_FUERA o alteró permisos"
fi

sudo -u "$REAL_USER" ln -s "${DIR_VIGILADO}/manual.txt" "${DIR_VIGILADO}/enlace_manual.txt"
C_MAN_LINK=$(printf '{"ruta":"%s/enlace_manual.txt"}' "$DIR_VIGILADO" | curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" --data-binary @- -w "%{http_code}" -o /dev/null "${BASE}/api/cuarentena")
if [ "$C_MAN_LINK" = "404" ]; then
	aprobar "Cuarentena manual sobre enlace simbólico rechazada con 404"
else
	fallar "Cuarentena sobre enlace simbólico devolvió $C_MAN_LINK, esperado 404"
fi

fase "11/13" "Sensor desactivado y detección en reactivación"
curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d '{"activo":false}' -o /dev/null "${BASE}/api/monitoreo/archivos"
sleep 0.5

sudo -u "$REAL_USER" bash -c "printf 'SO2-MUESTRA-MALICIOSA-2\n' > '${DIR_VIGILADO}/ransomware.txt'"
sleep 1.0
MODO_RW_DES=$(stat -c %a "${DIR_VIGILADO}/ransomware.txt" 2>/dev/null || echo "error")
if [ "$MODO_RW_DES" = "644" ]; then
	aprobar "Con sensor desactivado, ransomware.txt no se detecta (modo 644)"
else
	fallar "Con sensor desactivado, ransomware.txt tiene permisos ${MODO_RW_DES}"
fi

curl -s -b "$COOKIE_ADMIN" -H "Content-Type: application/json" -d '{"activo":true}' -o /dev/null "${BASE}/api/monitoreo/archivos"
sleep 1.5

MODO_RW_ACT=$(stat -c %a "${DIR_VIGILADO}/ransomware.txt" 2>/dev/null || echo "error")
if [ "$MODO_RW_ACT" = "0" ]; then
	aprobar "Al reactivar sensor, línea base detecta ransomware.txt y lo pone en cuarentena (0000)"
else
	fallar "Al reactivar sensor, ransomware.txt no fue puesto en cuarentena (modo ${MODO_RW_ACT})"
fi

fase "12/13" "Persistencia tras reinicio del daemon"
kill -TERM "$PID_DAEMON" 2>/dev/null || true
wait "$PID_DAEMON" 2>/dev/null || true

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

COOKIE_ADMIN_RE=$(mktemp_rastreado)
login_stdin "so2t_admin" "$COOKIE_ADMIN_RE"

RESP_BL_RE=$(mktemp_rastreado)
curl -s -b "$COOKIE_ADMIN_RE" -o "$RESP_BL_RE" "${BASE}/api/blacklist"

if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("firmas") == 5 or len(d.get("lista", [])) == 5, "No se preservaron las 5 firmas"
assert d.get("origen") == "archivo", "Origen no es archivo tras reinicio"
' "$RESP_BL_RE"; then
	aprobar "Tras reinicio, blacklist persiste 5 firmas con origen 'archivo'"
else
	fallar "Reinicio no preservó las 5 firmas en la base de datos local"
fi

fase "13/13" "Terminación limpia, log sin contraseñas y repositorio intacto"
kill -TERM "$PID_DAEMON" 2>/dev/null || true
wait "$PID_DAEMON" 2>/dev/null || true
PID_DAEMON=""

PASS_EXPUESTA=0
for u in so2t_admin so2t_comun; do
	if grep -F "${CLAVE[$u]}" "$LOG_DAEMON" >/dev/null 2>&1; then
		PASS_EXPUESTA=1
	fi
done

if [ "$PASS_EXPUESTA" -eq 0 ]; then
	aprobar "Log del daemon sin contraseñas expuestas"
else
	fallar "Se encontraron contraseñas en el log del daemon"
fi

SHA_REPO_FINAL=$(sha256sum "${REPO_DIR}/ProgramaIntermedio/blacklist.json" | cut -d' ' -f1)
if [ "$SHA_REPO_INICIAL" = "$SHA_REPO_FINAL" ]; then
	aprobar "ProgramaIntermedio/blacklist.json del repositorio intacto (${SHA_REPO_FINAL})"
else
	fallar "ProgramaIntermedio/blacklist.json del repositorio fue modificado"
fi

echo ""
echo -e "${BLUE}====================================================================${NC}"
echo -e "RESULTADO: ${GREEN}${APROBADAS} aprobadas${NC}, ${RED}${FALLIDAS} fallidas${NC}"
echo -e "${BLUE}====================================================================${NC}"

if [ "$FALLIDAS" -gt 0 ]; then
	exit 1
fi
exit 0
