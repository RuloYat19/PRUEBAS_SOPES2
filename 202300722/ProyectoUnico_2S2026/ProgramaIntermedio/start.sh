#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
if [[ $EUID -ne 0 ]]; then echo "Ejecuta con sudo: sudo $0" >&2; exit 1; fi
make
exec ./daemon
