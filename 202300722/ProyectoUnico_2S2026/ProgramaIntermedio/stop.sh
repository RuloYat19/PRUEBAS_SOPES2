#!/usr/bin/env bash
set -euo pipefail
PIDFILE=/tmp/proyecto_unico.pid
if [[ ! -f "$PIDFILE" ]]; then echo "No se encontró $PIDFILE"; exit 1; fi
PID=$(cat "$PIDFILE")
if kill -0 "$PID" 2>/dev/null; then kill -TERM "$PID"; echo "SIGTERM enviado a PID $PID"; else echo "El PID $PID ya no está activo"; fi
