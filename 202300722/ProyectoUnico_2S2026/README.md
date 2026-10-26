# Proyecto Único SO2 — guía rápida

Este repositorio contiene un punto de partida integrado para Linux 6.18.48, daemon en C, sensor eBPF/libbpf, dashboard web y manuales. Es una base de trabajo que debe compilarse y validarse en la VM con las syscalls reales de las prácticas.

## Antes de compilar

```bash
sudo apt update
sudo apt install -y build-essential gcc make clang llvm bpftool libbpf-dev libelf-dev libssl-dev libpam0g-dev libjson-c-dev pkg-config
```

Comprueba soporte eBPF/BTF en el kernel arrancado y revisa `Kernel/README.md` antes de copiar archivos al árbol del kernel.

## Compilar daemon + sensor

```bash
cd ProgramaIntermedio
make clean
make
```

## Ejecutar

```bash
sudo ./daemon --foreground  # depuración
# o, separado de la terminal:
sudo ./daemon
```

## Dashboard

`http://127.0.0.1:8080/`

## Detener

```bash
sudo kill -TERM "$(cat /tmp/proyecto_unico.pid)"
```

Consulta `ManualTecnico.md` y `ManualUsuario.md` para los detalles y limitaciones que deben validarse antes de entregar.
