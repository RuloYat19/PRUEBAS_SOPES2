# Proyecto Único — Sistemas Operativos 2 (2S 2026)

> Sistema Integral de Monitoreo en Tiempo Real, Integridad con eBPF, Detección de Malware y Auditoría de Logs en Linux Kernel 6.12.69.

---

## 1. Componentes del Sistema

| Componente | Directorio | Tecnologías | Responsabilidad |
|---|---|---|---|
| **Kernel** | `Kernel/` | C, Linux 6.12.69 | Syscalls 548–553, canal IPC y kthread `log_watch`. |
| **Sensor eBPF** | `ProgramaIntermedio/ebpf/` | eBPF, Clang, libbpf | Tracepoints y kprobes de apertura/escritura, Ring Buffer. |
| **Daemon C** | `ProgramaIntermedio/` | C ANSI, POSIX Threads, OpenSSL | Auditoría SHA-256, cuarentena 0000, PAM, HTTP y SSE. |
| **Frontend** | `Dashboard/` | HTML5, CSS Grid, Vanilla JS | Visualización en tiempo real sin dependencias externas. |
| **Automatización**| `scripts/` | Bash, jq, curl | Compilación del kernel, suites de prueba y demos. |

---

## 2. Requisitos del Sistema

- **Sistema Operativo**: Linux (Debian 12 / Ubuntu 24.04 LTS recomendados, arquitectura x86_64).
- **Kernel Soportado**: Linux `6.12.69-so2-202100106` con soporte para BTF (`/sys/kernel/btf/vmlinux`).
- **Paquetes del Sistema**:
  ```bash
  sudo apt update && sudo apt install -y \
      build-essential libncurses-dev bison flex libssl-dev libelf-dev \
      clang llvm libbpf-dev bpftool libpam0g-dev jq curl
  ```

---

## 3. Inicio Rápido

El despliegue completo se realiza en seis pasos:

### Paso 1: Preparación y Compilación del Kernel (en la VM)
```bash
scripts/preparar_kernel.sh --sin-instalar
```

### Paso 2: Instalación del Kernel y Reinicio
```bash
scripts/instalar_kernel.sh && sudo reboot
```

### Paso 3: Verificación del Kernel y Syscalls
Tras reiniciar con el kernel 6.12.69:
```bash
scripts/verificar_kernel.sh
```

### Paso 4: Configuración de Usuarios y Grupos PAM
Crea los grupos `admin_user`, `common_user` y el servicio `/etc/pam.d/so2_auth`:
```bash
sudo scripts/setup_usuarios.sh
```

### Paso 5: Compilación del Programa Intermedio y Sensor eBPF
```bash
make -C ProgramaIntermedio clean all
```

### Paso 6: Ejecución del Daemon de Monitoreo
```bash
# Modo primer plano (logs por consola):
sudo ProgramaIntermedio/daemon_monitor -c ProgramaIntermedio/daemon.conf

# O en segundo plano (modo daemon):
sudo ProgramaIntermedio/daemon_monitor -c ProgramaIntermedio/daemon.conf -d
```

Acceda inmediatamente al Dashboard web desde su navegador:
```text
http://localhost:8080
```

---

## 4. Estructura del Repositorio

```text
ProyectoUnico_2S2026/
├── README.md                       # Portada y guía de inicio rápido (este archivo)
├── AGENTS.md                       # Reglas de desarrollo, commits y flujo de trabajo
├── enunciado.md                    # Especificación oficial y criterios de evaluación
├── steering/                       # Contexto permanente (product, tech, structure)
├── specs/                          # Especificaciones por feature (00 a 08)
├── Kernel/                         # Código modificado del kernel Linux (syscalls e IPC)
├── ProgramaIntermedio/             # Daemon multihilo en C y módulo ebpf/
├── Dashboard/                      # Frontend web en Vanilla HTML5/CSS/JS
├── scripts/                        # Suites de pruebas automatizadas y utilidades
├── docs/                           # Manual Técnico y Manual de Usuario
└── imgs/                           # Evidencias y capturas de interfaz
```

---

## 5. Documentación Completa

Para detalles exhaustivos de arquitectura, llamadas al sistema, seguridad y operación, consulte:

- 📘 [**Manual Técnico**](docs/Manual_Tecnico.md): Arquitectura interna, syscalls 548–553, mapas y hooks eBPF, tabla de mutexes, catálogo de 24 endpoints REST, eventos SSE, decisiones de seguridad y limitaciones conocidas.
- 📗 [**Manual de Usuario**](docs/Manual_Usuario.md): Guía paso a paso para administradores y operadores, uso de métricas, control de procesos, auditoría de archivos, gestión de antivirus/cuarentena y resolución de problemas frecuentes.
- 📋 [**Especificaciones de Features (SDD)**](specs/README.md): Requisitos, diseño, tareas y evidencias de verificación de cada feature (F00 a F08).
- 📜 [**Enunciado Oficial**](enunciado.md): Términos de referencia, ponderaciones y rúbrica de evaluación.

---

## 6. Pruebas Automatizadas

El proyecto incluye suites de pruebas unitarias y de integración que cubren todas las funcionalidades:

### Pruebas Unitarias en C (11 Suites en Host)
Valida parsers, autenticación, cálculo de métricas, procesos, alertas, cuarentena y logwatch sin requerir privilegios de root:
```bash
make -C ProgramaIntermedio pruebas
```

### Ejecutor Integral de Pruebas en VM (9 Suites de Integración)
Ejecuta la batería de integración de extremo a extremo validando interacción real con el kernel, PAM y eBPF:
```bash
sudo scripts/correr_pruebas.sh
```

---

## 7. Demostración Guiada

Para ejecutar demostraciones preconfiguradas con inyección controlada de eventos:
- `scripts/demo.sh`: Demostración general de métricas, alertas, top 20 y logwatch.
- `scripts/demo_antivirus.sh`: Demostración de detección de malware y aislamiento automático en cuarentena.
