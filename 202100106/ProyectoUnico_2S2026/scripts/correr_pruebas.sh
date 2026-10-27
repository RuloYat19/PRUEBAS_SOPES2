#!/usr/bin/env bash
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
	echo "Ejecuta con sudo para correr todas las suites de prueba" >&2
	exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

CMD_UNITARIAS="make -C ${REPO_DIR}/ProgramaIntermedio clean all pruebas"
if [ -n "${SUDO_USER:-}" ]; then
	CMD_UNITARIAS="sudo -u \"$SUDO_USER\" make -C ${REPO_DIR}/ProgramaIntermedio clean all pruebas"
fi

SUITES=(
	"Compilación y pruebas unitarias en C" "$CMD_UNITARIAS"
	"Feature F07 - Logwatch e IPC" "${SCRIPT_DIR}/pruebas_logwatch.sh"
	"Feature F06 - Antivirus y cuarentena" "${SCRIPT_DIR}/pruebas_antivirus.sh"
	"Feature F05 - Sensor eBPF" "${SCRIPT_DIR}/pruebas_sensor.sh"
	"Feature F04 - Procesos" "${SCRIPT_DIR}/pruebas_procesos.sh"
	"Feature F03 - Métricas" "${SCRIPT_DIR}/pruebas_metricas.sh"
	"Feature F02 - Alertas" "${SCRIPT_DIR}/pruebas_alertas.sh"
	"Feature F01 - Autenticación y PAM" "${SCRIPT_DIR}/pruebas_auth.sh"
	"Integración general (F00-F07)" "${SCRIPT_DIR}/pruebas_integracion.sh"
)

TOTAL_SUITES=$((${#SUITES[@]} / 2))
EXITOSAS=0
FALLIDAS=0
RESULTADOS=()

echo -e "${BLUE}====================================================================${NC}"
echo -e "${BLUE}       EJECUTOR INTEGRAL DE PRUEBAS - PROYECTO ÚNICO SO2            ${NC}"
echo -e "${BLUE}====================================================================${NC}"

for ((i = 0; i < ${#SUITES[@]}; i += 2)); do
	nombre="${SUITES[i]}"
	cmd="${SUITES[i+1]}"
	indice=$(( (i / 2) + 1 ))

	echo -e "\n${YELLOW}>>> [${indice}/${TOTAL_SUITES}] Ejecutando: ${nombre}${NC}"
	t_inicio=$(date +%s)

	if eval "$cmd"; then
		t_fin=$(date +%s)
		duracion=$((t_fin - t_inicio))
		echo -e "  ${GREEN}✓ ${nombre} aprobada (${duracion}s)${NC}"
		EXITOSAS=$((EXITOSAS + 1))
		RESULTADOS+=("${GREEN}✓ [PASÓ]${NC}  ${nombre} (${duracion}s)")
	else
		t_fin=$(date +%s)
		duracion=$((t_fin - t_inicio))
		echo -e "  ${RED}✗ ${nombre} FALLÓ (${duracion}s)${NC}"
		FALLIDAS=$((FALLIDAS + 1))
		RESULTADOS+=("${RED}✗ [FALLÓ]${NC} ${nombre} (${duracion}s)")
	fi
done

echo -e "\n${YELLOW}>>> Verificación de usuarios residuales en el sistema...${NC}"
n_usuarios=$(getent passwd | grep -c '^so2t_' || true)
if [ "$n_usuarios" -eq 0 ]; then
	echo -e "  ${GREEN}✓ Sin usuarios de prueba residuales so2t_*${NC}"
	RESULTADOS+=("${GREEN}✓ [PASÓ]${NC}  Limpieza de usuarios (0 residuales)")
else
	echo -e "  ${RED}✗ Se encontraron ${n_usuarios} usuarios de prueba so2t_* sin borrar${NC}"
	FALLIDAS=$((FALLIDAS + 1))
	RESULTADOS+=("${RED}✗ [FALLÓ]${NC} Limpieza de usuarios (${n_usuarios} residuales)")
fi

echo -e "\n${BLUE}====================================================================${NC}"
echo -e "${BLUE}                    RESUMEN GLOBAL DE PRUEBAS                       ${NC}"
echo -e "${BLUE}====================================================================${NC}"
for res in "${RESULTADOS[@]}"; do
	echo -e "  ${res}"
done
echo -e "${BLUE}--------------------------------------------------------------------${NC}"
echo -e "  Suites aprobadas: ${GREEN}${EXITOSAS}${NC} · Suites fallidas: ${RED}${FALLIDAS}${NC}"
echo -e "${BLUE}====================================================================${NC}"

if [ "$FALLIDAS" -gt 0 ]; then
	exit 1
fi
exit 0
