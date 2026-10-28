# Puesta en marcha con el kernel ya compilado

Para una máquina donde **el kernel `6.12.69-202200389-so2-final` ya está compilado, instalado y arrancado**. Cubre lo que falta:
compilar el daemon y el sensor eBPF, preparar el sistema, crear los usuarios e iniciar el dashboard.

Si todavía no tienes el kernel, empieza por la [Guía de instalación desde cero](Guia_Instalacion_Desde_Cero.md).

---

## 0. Comprobar el kernel

```bash
uname -r
```

Debe decir `6.12.69-202200389-so2-final`. Si dice otro, reinicia y elige ese kernel en GRUB (*Advanced options for Ubuntu*).
Con otro kernel el daemon no inicia, porque el sensor eBPF necesita el BTF de este.

## 1. Entrar a la carpeta del proyecto

```bash
cd ~/SO2/ProyectoUnico_2S2026/ProgramaIntermedio
```

Cambia `~/SO2` por la carpeta donde clonaste el repositorio. Si todavía no lo tienes:

```bash
git clone https://github.com/jRodas497/SO2_2S2026_LAB.git ~/SO2
```

## 2. Instalar las dependencias de compilación

Solo si no las instalaste al preparar el kernel. Desde la carpeta `ProgramaIntermedio`:

```bash
../scripts/02_dependencias.sh
```

Instala `clang`, `llvm`, `libbpf-dev`, `libssl-dev`, `libpam0g-dev`, `bpftool` y las herramientas de compilación.

## 3. Compilar el daemon y el sensor eBPF

```bash
make
```

Debe terminar con la línea `cc -o so2monitor …` y **sin** líneas `warning`.

## 4. Preparar el sistema (una sola vez)

```bash
sudo ./scripts/instalar_sistema.sh
```

| Acción | Para qué |
|---|---|
| Crea los grupos `admin_user` y `common_user` | Definen el rol de cada usuario en el dashboard |
| Instala el servicio PAM `/etc/pam.d/so2-monitor` | El daemon autentica contra las cuentas del sistema |
| Crea `/tmp/monitor` (`1777`) | Carpeta vigilada por el sensor |
| Crea `/var/log/so2_app.log` y `/var/log/so2_central.log` | Logs de ejemplo del monitoreo de palabras clave |

Debe mostrar `grupo creado: admin_user`, `grupo creado: common_user` y `servicio PAM instalado`.

## 5. Crear los usuarios

```bash
sudo ./scripts/crear_usuario.sh admin1 admin
sudo ./scripts/crear_usuario.sh usuario1 common
```

Cada comando pide la contraseña del usuario. Elígela y anótala. Comprueba los grupos:

```bash
id admin1       # debe incluir admin_user
id usuario1     # debe incluir common_user
```

- Un usuario en **ambos** grupos es administrador; uno en **ninguno** no puede entrar al dashboard.
- Se pueden crear más usuarios en cualquier momento, incluso con el daemon en marcha.
- Para dar un rol a una cuenta que ya existe: `sudo usermod -aG admin_user NOMBRE` (o `common_user`).
- Para cambiar una contraseña: `sudo passwd NOMBRE`.

## 6. Iniciar el daemon

```bash
sudo ./so2monitor -f
```

Déjalo corriendo en esa terminal. Debe mostrar, sin errores:

```text
blacklist: 6 firmas cargadas
sensor eBPF cargado (PID del daemon registrado: …)
ruta a monitorear: /tmp/monitor (generacion 1)
HTTP: escuchando en el puerto 8080
logwatch 1 iniciado por configuracion: central=/var/log/so2_central.log palabra=ERROR …
```

## 7. Abrir el dashboard

En otra terminal:

```bash
hostname -I
```

Abre `http://IP:8080` en el navegador (`http://127.0.0.1:8080` si es la misma máquina).

- `admin1`: ve todos los controles.
- `usuario1`: solo lectura.

Arriba a la derecha debe decir **En vivo**. Si no carga desde otra máquina, abre el puerto: `sudo ufw allow 8080/tcp`.

## 8. Verificar todo (opcional)

Con el daemon corriendo, en una tercera terminal:

```bash
cd ~/SO2/ProyectoUnico_2S2026/ProgramaIntermedio
sudo ./scripts/probar_api.sh
```

Pide el usuario y la contraseña de `admin1` y de `usuario1`. Debe terminar en **58 de 58** pruebas correctas.

## Detener el daemon

`Ctrl+C` en la terminal donde corre. Para ejecutarlo en segundo plano, inícialo sin `-f`: el registro queda en
`/var/log/so2monitor.log` y se detiene con `sudo kill $(cat /run/so2monitor.pid)`.

## Si algo falla

| Síntoma | Causa y solución |
|---|---|
| `crear_usuario.sh` dice "Falta el grupo" | No se ejecutó el paso 4: corre `sudo ./scripts/instalar_sistema.sh` |
| El daemon no inicia o no carga el sensor | El kernel arrancado no es el `-so2-final` (paso 0) |
| `Address already in use` en el puerto 8080 | Hay otro servicio usándolo (por ejemplo el daemon de la práctica 4): deténlo |
| `make` falla por `bpftool` o `clang` | Faltan dependencias (paso 2) |
| El login falla con la contraseña correcta | El usuario no pertenece a `admin_user` ni a `common_user` (paso 5) |
