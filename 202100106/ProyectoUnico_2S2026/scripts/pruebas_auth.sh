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
PUERTO=8098
BASE="http://127.0.0.1:${PUERTO}"

REAL_USER="${SUDO_USER:-$(id -un)}"
USER_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
DIR="${USER_HOME}/so2_logs/pruebas_auth"

source "${SCRIPT_DIR}/usuarios_prueba.sh"

APROBADAS=0
FALLIDAS=0
PID_DAEMON=""
PID_SSE_LOGOUT=""
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
	[ -n "$PID_SSE_LOGOUT" ] && kill "$PID_SSE_LOGOUT" 2>/dev/null || true
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
echo -e "${BLUE}   PRUEBAS DE AUTENTICACION Y ROLES (PAM) - PROYECTO UNICO SO2     ${NC}"
echo -e "${BLUE}====================================================================${NC}"

fase "0/18" "Condiciones previas y preparación de usuarios"
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
log_palabras = INTRUSO
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

fase "1/18" "GET / y GET /login.html sin cookie"
c1=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/")
c2=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/login.html")
if [ "$c1" = "200" ] && [ "$c2" = "200" ]; then
	aprobar "Archivos estáticos responden 200 sin sesión"
else
	fallar "Archivos estáticos respondieron ${c1} y ${c2}"
fi

fase "2/18" "GET /api/estado, /api/eventos, /api/sesion sin cookie"
c1=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/api/estado")
c2=$(curl -s -m 1 -o /dev/null -w '%{http_code}' "${BASE}/api/eventos" || true)
c3=$(curl -s -o /dev/null -w '%{http_code}' "${BASE}/api/sesion")
if [ "$c1" = "401" ] && [ "$c2" = "401" ] && [ "$c3" = "401" ]; then
	aprobar "Rutas protegidas rechazan sin cookie con 401"
else
	fallar "Respuestas sin cookie: estado=${c1}, eventos=${c2}, sesion=${c3}"
fi

fase "3/18" "Login so2t_admin"
hdr_admin=$(mktemp_rastreado)
cookie_admin=$(mktemp_rastreado)
resp_admin=$(login_json "so2t_admin" | curl -s -D "$hdr_admin" -c "$cookie_admin" -H "Content-Type: application/json" -d @- "${BASE}/api/login")
c_admin=$(grep -i '^HTTP/' "$hdr_admin" | tail -n 1 | awk '{print $2}')
token_admin=$(grep "so2_sesion" "$cookie_admin" | awk '{print $7}')
if [ "$c_admin" = "200" ] && echo "$resp_admin" | grep -q '"rol":"admin_user"' && grep -iq 'HttpOnly' "$hdr_admin" && grep -iq 'SameSite=Strict' "$hdr_admin" && [ -n "$token_admin" ]; then
	aprobar "Login admin exitoso con rol admin_user y cookie HttpOnly/SameSite=Strict"
else
	fallar "Fallo login admin: codigo=${c_admin}, resp=${resp_admin}"
fi

fase "4/18" "Login so2t_ambos"
resp_ambos=$(login_json "so2t_ambos" | curl -s -H "Content-Type: application/json" -d @- "${BASE}/api/login")
if echo "$resp_ambos" | grep -q '"rol":"admin_user"'; then
	aprobar "Usuario en ambos grupos recibe rol preferente admin_user"
else
	fallar "Fallo login ambos grupos: resp=${resp_ambos}"
fi

fase "5/18" "Login so2t_comun"
hdr_comun=$(mktemp_rastreado)
cookie_comun=$(mktemp_rastreado)
resp_comun=$(login_json "so2t_comun" | curl -s -D "$hdr_comun" -c "$cookie_comun" -H "Content-Type: application/json" -d @- "${BASE}/api/login")
c_comun=$(grep -i '^HTTP/' "$hdr_comun" | tail -n 1 | awk '{print $2}')
if [ "$c_comun" = "200" ] && echo "$resp_comun" | grep -q '"rol":"common_user"'; then
	aprobar "Login comun exitoso con rol common_user"
else
	fallar "Fallo login comun: codigo=${c_comun}, resp=${resp_comun}"
fi

fase "6/18" "Login so2t_nadie con clave correcta"
hdr_nadie=$(mktemp_rastreado)
c_nadie=$(login_json "so2t_nadie" | curl -s -D "$hdr_nadie" -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/login")
if [ "$c_nadie" = "403" ] && [ "$(grep -ci 'set-cookie' "$hdr_nadie")" -eq 0 ]; then
	aprobar "Usuario sin grupo recibe 403 y ninguna cookie de sesion"
else
	fallar "Usuario sin grupo respondio ${c_nadie}"
fi

fase "7/18" "Clave incorrecta y usuario inexistente"
hdr_f1=$(mktemp_rastreado)
hdr_f2=$(mktemp_rastreado)
b_f1=$(printf '{"usuario":"so2t_admin","password":"clave_falsa_123"}' | curl -s -D "$hdr_f1" -H "Content-Type: application/json" -d @- "${BASE}/api/login")
b_f2=$(printf '{"usuario":"so2t_inexistente","password":"clave_falsa_123"}' | curl -s -D "$hdr_f2" -H "Content-Type: application/json" -d @- "${BASE}/api/login")
c_f1=$(grep -i '^HTTP/' "$hdr_f1" | tail -n 1 | awk '{print $2}')
c_f2=$(grep -i '^HTTP/' "$hdr_f2" | tail -n 1 | awk '{print $2}')
if [ "$c_f1" = "401" ] && [ "$c_f2" = "401" ] && [ "$b_f1" = "$b_f2" ]; then
	aprobar "Credenciales incorrectas responden 401 con cuerpo identico"
else
	fallar "Respuestas disimiles o codigo incorrecto: c1=${c_f1}, c2=${c_f2}"
fi

fase "8/18" "Tiempo de respuesta en login fallido (< 1 s)"
t_fallido=$(printf '{"usuario":"so2t_admin","password":"clave_falsa_rapida"}' | curl -s -o /dev/null -w '%{time_total}' -H "Content-Type: application/json" -d @- "${BASE}/api/login")
if awk -v t="$t_fallido" 'BEGIN { exit (t < 1.0 ? 0 : 1) }'; then
	aprobar "Login fallido respondio en ${t_fallido} s (< 1 s)"
else
	fallar "Login fallido tardo ${t_fallido} s (esperado < 1 s)"
fi

fase "9/18" "JSON inválido, sin password y usuario > 32 caracteres"
c_j1=$(printf '{json_invalido' | curl -s -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/login")
c_j2=$(printf '{"usuario":"so2t_admin"}' | curl -s -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/login")
c_j3=$(printf '{"usuario":"usuario_demasiado_largo_para_el_limite_de_32","password":"x"}' | curl -s -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/login")
if [ "$c_j1" = "400" ] && [ "$c_j2" = "400" ] && [ "$c_j3" = "400" ]; then
	aprobar "Peticiones de login malformadas responden 400"
else
	fallar "Peticiones malformadas respondieron: j1=${c_j1}, j2=${c_j2}, j3=${c_j3}"
fi

fase "10/18" "POST sin Content-Length y cuerpo mayor al búfer"
c_cl1=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Length:" "${BASE}/api/login")
c_cl2=$(head -c 5000 /dev/zero | tr '\0' 'a' | curl -s -o /dev/null -w '%{http_code}' -X POST -H "Content-Type: application/json" --data-binary @- "${BASE}/api/login")
if [ "$c_cl1" = "411" ] && [ "$c_cl2" = "413" ]; then
	aprobar "Manejo estricto de Content-Length (411) y desbordamiento de búfer (413)"
else
	fallar "Codigos inesperados: sin_cl=${c_cl1}, grande=${c_cl2}"
fi

fase "11/18" "Permisos de usuario comun"
c_com_est=$(curl -s -b "$cookie_comun" -o /dev/null -w '%{http_code}' "${BASE}/api/estado")
resp_com_ses=$(curl -s -b "$cookie_comun" "${BASE}/api/sesion")
c_com_pid=$(curl -s -b "$cookie_comun" -o /dev/null -w '%{http_code}' "${BASE}/api/proceso?pid=1")
c_com_sse=$(curl -s -m 1 -b "$cookie_comun" -o /dev/null -w '%{http_code}' "${BASE}/api/eventos" || true)
if [ "$c_com_est" = "200" ] && echo "$resp_com_ses" | grep -q '"rol":"common_user"' && [ "$c_com_pid" = "403" ] && [ "$c_com_sse" = "200" ]; then
	aprobar "Usuario comun accede a estado, sesion y eventos, pero recibe 403 en proceso"
else
	fallar "Permisos comunes invalidos: est=${c_com_est}, pid=${c_com_pid}, sse=${c_com_sse}"
fi

fase "12/18" "Permisos de administrador"
c_adm_pid=$(curl -s -b "$cookie_admin" -o /dev/null -w '%{http_code}' "${BASE}/api/proceso?pid=1")
if [ "$c_adm_pid" = "200" ]; then
	aprobar "Administrador consulta exitosamente proceso por PID (200)"
else
	fallar "Administrador recibio ${c_adm_pid} en consulta de proceso"
fi

fase "13/18" "Cookie con token inventado (64 hex)"
c_token_falso=$(curl -s -H "Cookie: so2_sesion=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" -o /dev/null -w '%{http_code}' "${BASE}/api/estado")
if [ "$c_token_falso" = "401" ]; then
	aprobar "Token inventado rechazado con 401"
else
	fallar "Token inventado respondio ${c_token_falso}"
fi

fase "14/18" "Métodos y rutas inexistentes"
c_m1=$(curl -s -b "$cookie_admin" -X PUT -o /dev/null -w '%{http_code}' "${BASE}/api/estado")
c_m2=$(curl -s -X GET -o /dev/null -w '%{http_code}' "${BASE}/api/login")
c_m3=$(curl -s -b "$cookie_admin" -X GET -o /dev/null -w '%{http_code}' "${BASE}/api/noexiste")
if [ "$c_m1" = "405" ] && [ "$c_m2" = "405" ] && [ "$c_m3" = "404" ]; then
	aprobar "Rutas y metodos invalidos responden 405 y 404 correctamente"
else
	fallar "Respuestas invalidas: put=${c_m1}, get_login=${c_m2}, noexiste=${c_m3}"
fi

fase "15/18" "Logout y desconexión de clientes SSE"
cookie_logout=$(mktemp_rastreado)
login_json "so2t_comun" | curl -s -c "$cookie_logout" -H "Content-Type: application/json" -d @- "${BASE}/api/login" >/dev/null
token_logout=$(grep "so2_sesion" "$cookie_logout" | awk '{print $7}')

curl -s -N -b "$cookie_logout" "${BASE}/api/eventos" > "${DIR}/sse_logout.txt" 2>&1 &
PID_SSE_LOGOUT=$!
sleep 0.5

c_logout=$(curl -s -b "$cookie_logout" -c "$cookie_logout" -o /dev/null -w '%{http_code}' -X POST -d '' "${BASE}/api/logout")
c_revocado=$(curl -s -H "Cookie: so2_sesion=${token_logout}" -o /dev/null -w '%{http_code}' "${BASE}/api/estado")

sleep 0.5
if ! kill -0 "$PID_SSE_LOGOUT" 2>/dev/null; then
	sse_desconectado=1
else
	sse_desconectado=0
	kill "$PID_SSE_LOGOUT" 2>/dev/null || true
fi
PID_SSE_LOGOUT=""

if [ "$c_logout" = "200" ] && [ "$c_revocado" = "401" ] && [ "$sse_desconectado" -eq 1 ]; then
	aprobar "Logout responde 200, revoca el token y desconecta el cliente SSE activo"
else
	fallar "Fallo logout: code=${c_logout}, revocado=${c_revocado}, sse_desconectado=${sse_desconectado}"
fi

fase "16/18" "Alta de usuario en caliente"
usuarios_prueba_agregar "so2t_nuevo" "common_user"
c_nuevo=$(login_json "so2t_nuevo" | curl -s -o /dev/null -w '%{http_code}' -H "Content-Type: application/json" -d @- "${BASE}/api/login")
if [ "$c_nuevo" = "200" ]; then
	aprobar "Usuario nuevo creado con daemon en marcha puede iniciar sesión inmediatamente"
else
	fallar "Usuario nuevo respondio ${c_nuevo}"
fi

fase "17/18" "Auditoría de logs (sin contraseñas)"
log_daemon="${DIR}/daemon.out"
pwd_encontrada=0
for u in "${USUARIOS_CREADOS[@]}"; do
	clave_u="${CLAVE[$u]:-}"
	if [ -n "$clave_u" ] && grep -qF "$clave_u" "$log_daemon" 2>/dev/null; then
		pwd_encontrada=1
		break
	fi
done

if grep -q "Login exitoso" "$log_daemon" 2>/dev/null && grep -q "Login fallido" "$log_daemon" 2>/dev/null && [ "$pwd_encontrada" -eq 0 ]; then
	aprobar "Logs contienen 'Login exitoso', 'Login fallido' y ninguna contraseña de prueba"
else
	fallar "Fallo en auditoria de logs: pwd_encontrada=${pwd_encontrada}"
fi

fase "18/18" "Detención limpia con SIGTERM"
if kill -0 "$PID_DAEMON" 2>/dev/null; then
	kill -TERM "$PID_DAEMON"
	wait "$PID_DAEMON"
	codigo_salida=$?
	PID_DAEMON=""
	if [ "$codigo_salida" -eq 0 ]; then
		aprobar "Daemon sigue vivo tras la batería y se detiene limpiamente con código 0"
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
