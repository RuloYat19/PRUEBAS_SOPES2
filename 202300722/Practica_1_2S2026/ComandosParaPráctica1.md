# Práctica 1 – Modificación de funciones del Kernel

## 1. Confirmar la versión del kernel

- [ ] Preguntar o revisar lo indicado por el auxiliar acerca de la **versión estable del kernel** que debe utilizarse.
- [ ] No escoger todavía una versión al azar si el auxiliar estableció una específica.

El documento solamente indica:

> Descargar una versión estable del Kernel de Linux, utilizando la versión indicada en el laboratorio.

Por lo tanto, el PDF no proporciona el número exacto de versión.

---

## 2. Preparar el entorno de trabajo

Como el equipo principal utiliza Fedora, se puede mantener Fedora como sistema anfitrión y crear una máquina virtual.

La práctica pide:

- [ ] GNU/Linux basado en Debian.
- [ ] Mínimo 2 núcleos para la máquina virtual.
- [ ] 4 GB de RAM.
- [ ] Aproximadamente 30 GB de almacenamiento disponible.

Una opción adecuada sería:

```text
Fedora
└── KVM / Virt-Manager
    └── Ubuntu o Debian
        └── Kernel modificado
```

Esto también evita dañar el kernel del sistema Fedora principal en caso de que exista un error durante la compilación o instalación.

---

## 3. Instalar las herramientas necesarias

Dentro de Ubuntu/Debian:

```bash
sudo apt update
```

Instalar las herramientas requeridas:

```bash
sudo apt install build-essential libncurses-dev flex bison libssl-dev libelf-dev
```

También es conveniente instalar:

```bash
sudo apt install bc dwarves git fakeroot
```

Verificar:

```bash
gcc --version
make --version
```

---

## 4. Descargar el código fuente del kernel

Cuando se confirme la versión indicada por el laboratorio:

```bash
wget https://cdn.kernel.org/pub/linux/kernel/...
```

El nombre exacto dependerá de la versión indicada.

Por ejemplo, conceptualmente se tendrá:

```text
linux-X.Y.Z.tar.xz
```

Descomprimir:

```bash
tar -xf linux-X.Y.Z.tar.xz
```

Entrar a la carpeta:

```bash
cd linux-X.Y.Z
```

---

## 5. Preparar la configuración del kernel

Antes de modificar el kernel, utilizar una configuración funcional como base.

Normalmente:

```bash
cp /boot/config-$(uname -r) .config
```

Después:

```bash
make olddefconfig
```

La práctica también solicita evidencia de `menuconfig`, así que ejecutar:

```bash
make menuconfig
```

- [ ] Tomar una captura de pantalla de `menuconfig`.

No es necesario modificar opciones arbitrariamente; la captura sirve como evidencia del proceso de configuración.

---

## 6. Localizar `getpid()` dentro del kernel

La práctica requiere localizar la implementación real de:

```c
sys_getpid()
```

Se puede buscar utilizando:

```bash
grep -R "SYSCALL_DEFINE0(getpid)" -n .
```

o:

```bash
grep -R "getpid" -n kernel/
```

El archivo exacto puede cambiar dependiendo de la versión del kernel.

Por eso **la versión elegida sí importa**.

---

## 7. Crear el contador global

La práctica solicita modificar `getpid()` para llevar un contador acumulativo.

Conceptualmente:

```text
Usuario llama getpid()
        ↓
Kernel ejecuta sys_getpid()
        ↓
contador++
        ↓
Kernel devuelve PID
```

Se debe:

- [ ] Crear una variable global.
- [ ] Inicializarla.
- [ ] Incrementarla cada vez que se invoque `getpid()`.

El contador **no debe ser una variable local**, porque se reiniciaría en cada llamada.

---

## 8. Modificar `getpid()`

Dentro de la implementación correspondiente:

```c
SYSCALL_DEFINE0(getpid)
{
    // incrementar contador

    // comportamiento original de getpid
}
```

La modificación no debe alterar el funcionamiento normal de `getpid()`.

Es decir:

```text
getpid() antes
→ devuelve PID

getpid() modificado
→ incrementa contador
→ devuelve PID
```

---

## 9. Crear la nueva syscall `getpid_counter()`

Además de modificar `getpid()`, la práctica exige crear una nueva llamada al sistema:

```text
getpid_counter()
```

Esta nueva syscall deberá permitir comprobar el valor acumulado del contador.

Debe utilizar:

```c
printk(...)
```

para escribir el valor en los logs del kernel.

Por ejemplo, conceptualmente:

```text
getpid_counter()
       ↓
lee contador global
       ↓
printk(...)
       ↓
dmesg
```

---

## 10. Agregar la syscall a la tabla de llamadas

Se deberá localizar la tabla de llamadas al sistema correspondiente a la arquitectura.

El propio documento menciona como ejemplo:

```text
syscall_64.tbl
```

Se tendrá que:

- [ ] Elegir un número de syscall disponible.
- [ ] Registrar `getpid_counter`.
- [ ] Asociarla con su implementación.

Ejemplo conceptual:

```text
NNN    common    getpid_counter    sys_getpid_counter
```

El número exacto deberá comprobarse en la versión del kernel utilizada.

---

## 11. Realizar los cambios adicionales requeridos

Dependiendo de la versión del kernel, probablemente habrá que modificar más de un archivo.

La práctica pide conservar **todos los archivos del kernel que hayan sido modificados**, manteniendo su estructura original.

Por ejemplo:

```text
Practica_1_2S2026/
├── arch/
│   └── x86/
│       └── entry/
│           └── syscalls/
│               └── syscall_64.tbl
│
├── kernel/
│   └── ...
│
└── Programa_Intermedio/
```

No se debe subir todo el kernel al repositorio.

---

## 12. Compilar el kernel

Primero se puede utilizar:

```bash
make -j$(nproc)
```

La compilación puede tardar bastante.

La práctica exige:

- [ ] Kernel compilado correctamente.
- [ ] Sin errores que impidan generar el kernel.
- [ ] Captura del proceso de compilación.

---

## 13. Instalar los módulos

Después de compilar:

```bash
sudo make modules_install
```

---

## 14. Instalar el nuevo kernel

Posteriormente:

```bash
sudo make install
```

Esto debería instalar:

- Imagen del kernel.
- System.map.
- Configuración correspondiente.

---

## 15. Actualizar GRUB

Dependiendo de Debian/Ubuntu:

```bash
sudo update-grub
```

Después verificar que aparezca el kernel nuevo.

---

## 16. Reiniciar la máquina virtual

```bash
sudo reboot
```

Al arrancar, seleccionar el kernel compilado si fuera necesario mediante las opciones avanzadas de GRUB.

---

## 17. Verificar que realmente arrancó el kernel modificado

Después de iniciar:

```bash
uname -r
```

- [ ] Confirmar que corresponde al kernel compilado.

Este punto es crítico.

La rúbrica indica que si el kernel modificado **no puede arrancar**, no se procede con la revisión técnica.

---

## 18. Crear el programa de prueba en C

Crear:

```text
Programa_Intermedio/
└── prueba.c
```

Este programa deberá ejecutar repetidas veces:

```c
getpid();
```

Por ejemplo:

```text
getpid()
getpid()
getpid()
getpid()
getpid()
```

Después deberá invocar la syscall personalizada:

```text
getpid_counter()
```

---

## 19. Compilar el programa de usuario

Por ejemplo:

```bash
gcc prueba.c -o prueba
```

Ejecutar:

```bash
./prueba
```

---

## 20. Revisar el contador mediante `dmesg`

Después de ejecutar el programa:

```bash
sudo dmesg | tail
```

O filtrar el mensaje:

```bash
sudo dmesg | grep getpid
```

Se deberá poder observar algo equivalente a:

```text
getpid_counter: contador = 5
```

Ejecutar nuevamente el programa y comprobar que el contador aumentó:

```text
getpid_counter: contador = 10
```

La práctica específicamente evalúa que el incremento sea **secuencial y consistente**.

---

## 21. Obtener las evidencias

Guardar capturas de:

- [ ] Sistema operativo utilizado.
- [ ] Versión del kernel descargado.
- [ ] Archivos modificados.
- [ ] `make menuconfig`.
- [ ] Proceso de compilación.
- [ ] Instalación del kernel.
- [ ] `uname -r` después de arrancar.
- [ ] Programa de prueba.
- [ ] Ejecución de `getpid()`.
- [ ] Ejecución de `getpid_counter()`.
- [ ] Resultado de `dmesg`.
- [ ] Incremento secuencial del contador.

---

## 22. Crear el informe técnico

Debe ser un archivo **Markdown dentro del repositorio**.

Debe contener:

### Introducción

Explicar qué se realizó y por qué se modificó el kernel.

### Objetivos

Objetivo general y objetivos específicos.

### Procedimiento

Explicar paso a paso:

```text
Descarga del kernel
        ↓
Configuración
        ↓
Modificación getpid()
        ↓
Creación getpid_counter()
        ↓
Modificación syscall_64.tbl
        ↓
Compilación
        ↓
Instalación
        ↓
Arranque
        ↓
Programa de prueba
        ↓
dmesg
```

### Evidencias

Agregar capturas.

### Análisis

Explicar:

- Diferencia entre espacio de usuario y kernel.
- Qué ocurre cuando un programa ejecuta `getpid()`.
- Para qué sirve una syscall.
- Por qué `printk` aparece en `dmesg`.
- Por qué el contador debe permanecer en espacio de kernel.

### Conclusiones

Agregar conclusiones personales y técnicas.

---

## 23. Preparar la estructura del repositorio

La práctica pide que todo quede dentro de:

```text
Practica_1_2S2026/
```

La rama debe llevar el **carné del estudiante**.

Ejemplo:

```bash
git checkout -b TU_CARNE
```

La estructura podría terminar así:

```text
Practica_1_2S2026/
│
├── README.md
│
├── Programa_Intermedio/
│   └── prueba.c
│
├── arch/
│   └── x86/
│       └── entry/
│           └── syscalls/
│               └── syscall_64.tbl
│
└── kernel/
    └── [archivo modificado]
```

Solamente deben subirse los archivos modificados del kernel, conservando su ruta original.

---

## 24. Subir al repositorio

```bash
git status
```

Luego:

```bash
git add Practica_1_2S2026
```

Crear el commit:

```bash
git commit -m "Practica 1: modificación de funciones del kernel"
```

Y subir la rama:

```bash
git push origin TU_CARNE
```

---

## 25. Verificación final antes de entregar

- [ ] Estoy usando Debian/Ubuntu/Linux Mint o derivado.
- [ ] Estoy usando la versión del kernel indicada por el laboratorio.
- [ ] El kernel compila.
- [ ] El kernel arranca.
- [ ] `getpid()` continúa funcionando.
- [ ] Cada llamada a `getpid()` incrementa el contador.
- [ ] `getpid_counter()` funciona.
- [ ] Utilicé `printk`.
- [ ] El resultado aparece en `dmesg`.
- [ ] El programa C de prueba funciona.
- [ ] Guardé los archivos modificados manteniendo las rutas del kernel.
- [ ] No subí todo el código fuente del kernel.
- [ ] Existe la carpeta `Programa_Intermedio`.
- [ ] El informe está escrito en Markdown.
- [ ] Incluí capturas.
- [ ] La rama tiene como nombre mi carné.
- [ ] Subí todo a GitLab.
