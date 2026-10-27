#!/usr/bin/env bash
set -u

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON_BIN="${SCRIPT_DIR}/../ProgramaIntermedio/daemon_monitor"
DASHBOARD_DIR="$(cd "${SCRIPT_DIR}/../Dashboard" && pwd)"
PUERTO=8080
LOG_DIR="${HOME}/so2_logs"
AUTH_LOG="${LOG_DIR}/auth.log"
APP_LOG="${LOG_DIR}/app.log"
CENTRAL_LOG="${LOG_DIR}/log_centralizado.txt"
DAEMON_OUT="${LOG_DIR}/daemon_monitor.out"
CONF_DEMO="${LOG_DIR}/demo.conf"
PALABRA_DEFAULT="INTRUSO"
COOKIE_JAR=$(mktemp)

limpiar_cookie() {
	rm -f "$COOKIE_JAR" 2>/dev/null || true
}
trap limpiar_cookie EXIT

iniciar_sesion() {
	if ! verificar_daemon_vivo; then
		echo -e "${YELLOW}! El daemon no está en ejecución. Iniciándolo...${NC}"
		arrancar_daemon
	fi

	echo -e "${BLUE}=== Autenticación requerida para el demo ===${NC}"
	read -r -p "Usuario: " DEMO_USER
	read -rs -p "Contraseña: " DEMO_PASS
	echo ""

	local u_escapado="${DEMO_USER//\\/\\\\}"
	u_escapado="${u_escapado//\"/\\\"}"
	local p_escapado="${DEMO_PASS//\\/\\\\}"
	p_escapado="${p_escapado//\"/\\\"}"

	local resp
	resp=$(printf '{"usuario":"%s","password":"%s"}' "$u_escapado" "$p_escapado" | curl -s -c "$COOKIE_JAR" -H "Content-Type: application/json" -d @- "http://127.0.0.1:${PUERTO}/api/login")
	unset DEMO_PASS p_escapado

	if ! grep -q "so2_sesion" "$COOKIE_JAR" 2>/dev/null; then
		local err
		err=$(echo "$resp" | grep -o '"error":"[^"]*"' | cut -d: -f2- | tr -d '"' || true)
		[ -z "$err" ] && err="Credenciales inválidas o no se pudo contactar al daemon"
		echo -e "${RED}✗ Error al autenticar: ${err}${NC}"
		exit 1
	fi
	echo -e "${GREEN}✓ Sesión iniciada correctamente.${NC}"
}

obtener_ips() {
    ip -4 addr show | awk '/inet / && !/127.0.0.1/ {print $2}' | cut -d/ -f1 || true
}

verificar_daemon_vivo() {
    pgrep -x daemon_monitor >/dev/null 2>&1
}

obtener_pid_daemon() {
    pgrep -x daemon_monitor | head -n 1 || true
}

limpiar_logs() {
    if ! mkdir -p "$LOG_DIR" 2>/dev/null; then
        LOG_DIR="${SCRIPT_DIR}/so2_logs"
        mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR="/tmp/so2_logs"
        mkdir -p "$LOG_DIR" 2>/dev/null || true
        AUTH_LOG="${LOG_DIR}/auth.log"
        APP_LOG="${LOG_DIR}/app.log"
        CENTRAL_LOG="${LOG_DIR}/log_centralizado.txt"
        DAEMON_OUT="${LOG_DIR}/daemon_monitor.out"
        CONF_DEMO="${LOG_DIR}/demo.conf"
    fi
    mkdir -p "$LOG_DIR" 2>/dev/null || true
    : > "$AUTH_LOG" 2>/dev/null || true
    : > "$APP_LOG" 2>/dev/null || true
    : > "$CENTRAL_LOG" 2>/dev/null || true
    : > "$DAEMON_OUT" 2>/dev/null || true
    echo -e "${GREEN}✓ Archivos de log preparados en ${LOG_DIR}${NC}"
}

arrancar_daemon() {
    if verificar_daemon_vivo; then
        echo -e "${YELLOW}! El daemon ya está en ejecución (PID: $(obtener_pid_daemon))${NC}"
        return 0
    fi

    if [ ! -x "$DAEMON_BIN" ] || ldd "$DAEMON_BIN" 2>&1 | grep -q 'not found'; then
        echo -e "${CYAN}Compilando daemon_monitor...${NC}"
        make -B -C "${SCRIPT_DIR}/../ProgramaIntermedio" || {
            echo -e "${RED}✗ Error al compilar el daemon.${NC}"
            return 1
        }
    fi

    limpiar_logs

    cat << EOF > "$CONF_DEMO"
puerto = ${PUERTO}
intervalo = 2
dashboard = ${DASHBOARD_DIR}
log_central = ${CENTRAL_LOG}
log_rutas = ${AUTH_LOG}, ${APP_LOG}
log_palabras = ${PALABRA_DEFAULT}, ERROR, FAIL
ruta_vigilada = ${HOME}/so2_vigilado
blacklist = ${SCRIPT_DIR}/../ProgramaIntermedio/blacklist.json
EOF

    echo -e "${CYAN}Iniciando daemon_monitor en segundo plano (Puerto ${PUERTO}, vigilando '${PALABRA_DEFAULT}')...${NC}"
    "$DAEMON_BIN" -c "$CONF_DEMO" -d

    sleep 0.8
    if verificar_daemon_vivo; then
        local pid
        pid=$(obtener_pid_daemon)
        echo -e "${GREEN}Daemon Listo (PID: ${pid})${NC}"
        echo -e "${GREEN}Dashboard: http://localhost:${PUERTO}${NC}"
        local ips
        ips=$(obtener_ips)
        for ip in $ips; do
            echo -e "  ->remoto: ${CYAN}http://${ip}:${PUERTO}${NC}"
        done
    else
        echo -e "${RED}✗ El daemon no pudo iniciar. Revisa ${DAEMON_OUT}:${NC}"
        cat "$DAEMON_OUT" 2>/dev/null || true
    fi
}

detener_daemon() {
    if ! verificar_daemon_vivo; then
        echo -e "${YELLOW}! El daemon no está en ejecución.${NC}"
        return 0
    fi

    local pid
    pid=$(obtener_pid_daemon)
    echo -e "${CYAN}Deteniendo daemon (PID ${pid}) con señal SIGTERM...${NC}"
    kill -TERM "$pid" 2>/dev/null || true

    for _ in $(seq 20); do
        if ! verificar_daemon_vivo; then
            break
        fi
        sleep 0.1
    done

    if ! verificar_daemon_vivo; then
        echo -e "${GREEN}Daemon detenido (recursos y watches de kernel liberados).${NC}"
    else
        echo -e "${RED}! Forzando detención...${NC}"
        kill -9 "$pid" 2>/dev/null || true
    fi
}

inyectar_evento() {
    mkdir -p "$LOG_DIR"
    local palabra="${1:-$PALABRA_DEFAULT}"
    local msg="${2:-Intento no autorizado detectado en el servicio SSH}"
    local fecha
    fecha=$(date '+%Y-%m-%d %H:%M:%S')
    local linea="[${fecha}] ALERTA CRITICA - ${palabra}: ${msg}"

    echo "$linea" >> "$AUTH_LOG"
    echo -e "${GREEN}Inyectada línea en ${AUTH_LOG}:${NC}"
    echo -e "  ${YELLOW}${linea}${NC}"
    echo -e "${CYAN}Alerta ya en dashboard (http://localhost:${PUERTO})${NC}"
}

inyectar_rafaga() {
    mkdir -p "$LOG_DIR"
    local total="${1:-10}"
    echo -e "${CYAN} Inyectando ${total} eventos concurrentes en ${AUTH_LOG}...${NC}"
    for i in $(seq 1 "$total"); do
        local fecha
        fecha=$(date '+%Y-%m-%d %H:%M:%S')
        echo "[${fecha}] EVENTO_MASICVO${i} - ${PALABRA_DEFAULT}: Deteccion masiva #${i}" >> "$AUTH_LOG"
        sleep 0.02
    done
    echo -e "${GREEN}✓ ${total} eventos inyectados sin pérdida en cola FIFO de kernel.${NC}"
}

consultar_pid() {
    local pid_obj="$1"
    if [ -z "$pid_obj" ]; then
        if verificar_daemon_vivo; then
            pid_obj=$(obtener_pid_daemon)
        else
            pid_obj="1"
        fi
    fi

    echo -e "${CYAN}Consultando PID ${pid_obj} /api/proceso?pid=${pid_obj} (Syscall 552)...${NC}"
    local resp
    resp=$(curl -s -b "$COOKIE_JAR" "http://127.0.0.1:${PUERTO}/api/proceso?pid=${pid_obj}")
    if command -v jq >/dev/null 2>&1; then
        echo "$resp" | jq .
    elif command -v python3 >/dev/null 2>&1; then
        echo "$resp" | python3 -m json.tool 2>/dev/null || echo "$resp"
    else
        echo "$resp"
    fi
}

ver_estado() {
    echo -e "${CYAN}Consulta a /api/estado${NC}"
    local resp
    resp=$(curl -s -b "$COOKIE_JAR" "http://127.0.0.1:${PUERTO}/api/estado")
    if command -v jq >/dev/null 2>&1; then
        echo "$resp" | jq '{memoria_usada, memoria_libre, swap_usada, resumen_procesos}'
    elif command -v python3 >/dev/null 2>&1; then
        echo "$resp" | python3 -c 'import sys, json; d=json.load(sys.stdin); print(json.dumps({"memoria_usada": d.get("memoria_usada"), "memoria_libre": d.get("memoria_libre"), "resumen_procesos": d.get("resumen_procesos")}, indent=2))' 2>/dev/null || echo "$resp"
    else
        echo "$resp"
    fi
}

menu() {
    while true; do
        clear
        echo -e "${BLUE}====================================================================${NC}"
        echo -e "${BLUE}   DEMO PROYECTO UNICO - SO2 2S2026 - CARNE 202100106               ${NC}"
        echo -e "${BLUE}   Kernel: $(uname -r)                                              ${NC}"
        echo -e "${BLUE}====================================================================${NC}"

        local estado_str="${RED}DETENIDO${NC}"
        if verificar_daemon_vivo; then
            estado_str="${GREEN}EN EJECUCION (PID: $(obtener_pid_daemon))${NC}"
        fi
        echo -e "Daemon: ${estado_str}"
        echo -e "Dashboard URL:     ${CYAN}http://localhost:${PUERTO}${NC}"
        local ips
        ips=$(obtener_ips)
        for ip in $ips; do
            echo -e "Acceso en Red:     ${CYAN}http://${ip}:${PUERTO}${NC}"
        done
        echo -e "--------------------------------------------------------------------"
        echo -e " ${BOLD}1)${NC} Iniciar / Reiniciar Daemon de Monitoreo"
        echo -e " ${BOLD}2)${NC} Detener Daemon (SIGTERM)"
        echo -e " ${BOLD}3)${NC} Abrir Dashboard en Navegador"
        echo -e "--------------------------------------------------------------------"
        echo -e " ${BOLD}4)${NC} Inyectar Alerta Individual"
        echo -e " ${BOLD}5)${NC} Inyectar Rafaga de 10 Alertas"
        echo -e " ${BOLD}6)${NC} Consultar Proceso por PID (Syscall 552)"
        echo -e " ${BOLD}7)${NC} Ver Resumen de Estado (/api/estado)"
        echo -e "--------------------------------------------------------------------"
        echo -e " ${BOLD}8)${NC} Ver Log Centralizado (tail -f)"
        echo -e " ${BOLD}9)${NC} Mensajes en kernel (dmesg)"
        echo -e " ${BOLD}T)${NC} Validar syscalls (test_syscalls)"
        echo -e " ${BOLD}P)${NC} Pruebas de integracion"
        echo -e "--------------------------------------------------------------------"
        echo -e " ${BOLD}0)${NC} Salir"
        echo -e "===================================================================="
        read -r -p "Seleccionar [0-9, T, P]: " opcion

        case "$opcion" in
            1)
                detener_daemon
                arrancar_daemon
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            2)
                detener_daemon
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            3)
                if command -v xdg-open >/dev/null 2>&1; then
                    xdg-open "http://localhost:${PUERTO}" >/dev/null 2>&1 &
                elif command -v sensible-browser >/dev/null 2>&1; then
                    sensible-browser "http://localhost:${PUERTO}" >/dev/null 2>&1 &
                fi
                echo -e "Abrir: ${CYAN}http://localhost:${PUERTO}${NC}"
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            4)
                echo -e "\nPalabras clave: ${YELLOW}${PALABRA_DEFAULT}, ERROR, FAIL${NC}"
                read -r -p "Palabra clave a inyectar [default: ${PALABRA_DEFAULT}]: " kw
                kw="${kw:-$PALABRA_DEFAULT}"
                read -r -p "Mensaje de log: " msg
                msg="${msg:-Acceso no autorizado detectado en el puerto 22}"
                inyectar_evento "$kw" "$msg"
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            5)
                inyectar_rafaga 10
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            6)
                read -r -p "Ingresa el PID a consultar [default: PID daemon]: " pid_in
                consultar_pid "$pid_in"
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            7)
                ver_estado
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            8)
                echo -e "${CYAN}Mostrando ${CENTRAL_LOG} (Ctrl+C para salir)...${NC}"
                tail -n 20 -f "$CENTRAL_LOG" 2>/dev/null || echo "Log vacío o no existe aún."
                ;;
            9)
                echo -e "${CYAN}Últimos mensajes de dmesg relacionados con la práctica:${NC}"
                sudo dmesg | tail -n 25 || dmesg | tail -n 25
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            T|t)
                echo -e "${CYAN}Ejecutando ./test_syscalls...${NC}"
                "${SCRIPT_DIR}/test_syscalls"
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            P|p)
                echo -e "${CYAN}Ejecutando pruebas de integración completas...${NC}"
                detener_daemon
                "${SCRIPT_DIR}/pruebas_integracion.sh"
                read -r -p "Presiona ENTER para continuar..." _
                ;;
            0)
                exit 0
                ;;
            *)
                echo -e "${RED}Opción no válida.${NC}"
                sleep 1
                ;;
        esac
    done
}

case "${1:-}" in
    stop)
        detener_daemon
        ;;
    clean)
        limpiar_logs
        ;;
    *)
        iniciar_sesion
        case "${1:-}" in
            start)
                arrancar_daemon
                ;;
            event)
                inyectar_evento "${2:-$PALABRA_DEFAULT}" "${3:-Evento de prueba}"
                ;;
            burst)
                inyectar_rafaga "${2:-10}"
                ;;
            pid)
                consultar_pid "${2:-}"
                ;;
            status)
                ver_estado
                ;;
            *)
                menu
                ;;
        esac
        ;;
esac
