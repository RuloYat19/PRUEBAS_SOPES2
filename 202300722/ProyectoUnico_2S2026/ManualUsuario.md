# Manual de usuario — Monitor integral de seguridad

## 1. Iniciar el sistema

1. Arranca el kernel Linux 6.18.48 que contiene las syscalls integradas.
2. Entra a `ProgramaIntermedio` y compila con `make`.
3. Para depuración usa `sudo ./daemon --foreground`. Para el modo separado de la terminal usa `sudo ./daemon`.
4. Abre `http://127.0.0.1:8080/` en el navegador.
5. Si la página no responde, revisa `/tmp/proyecto_unico.log`.

## 2. Iniciar sesión

1. Escribe el nombre de una cuenta local de Linux.
2. Escribe su contraseña; se valida con PAM.
3. La cuenta debe pertenecer a `admin_user` o `common_user`.
4. Si pertenece a ambos grupos, el sistema asigna rol de administrador.

## 3. Usuario común

Puede consultar las métricas, el estado de procesos, las alertas y los archivos analizados. No debe tener acceso a controles administrativos.

## 4. Administrador

Puede utilizar los controles administrativos disponibles en la interfaz para activar/desactivar las métricas y el escaneo, y recargar la blacklist. Las operaciones que se implementen posteriormente —configurar rutas, administrar logwatch, ver cuarentena, consultar un PID, terminar procesos y poner archivos en cuarentena manualmente— deben confirmarse en pantalla y registrarse como alertas.

## 5. Interpretación de alertas

- **LOW:** evento informativo, como activar el monitoreo o un inicio de sesión exitoso.
- **MEDIUM:** advertencia, como palabra clave en un log, inicio de sesión fallido o desactivar el monitoreo.
- **HIGH:** evento crítico, como hash malicioso detectado o archivo puesto en cuarentena.

Las alertas muestran hora, tipo, severidad y descripción.

## 6. Archivos analizados

La tabla muestra ruta, SHA-256, estado benigno/infectado y timestamp de modificación. Una detección de prueba debe hacerse solamente con archivos descartables creados para el laboratorio. El cambio de permisos a `0000` impide el acceso ordinario; no borres archivos del sistema para probarlo.

## 7. Cerrar sesión y detener el daemon

El botón **Cerrar sesión** cierra la sesión del dashboard. No detiene el daemon.

Para detener el daemon desde terminal:

```bash
sudo kill -TERM "$(cat /tmp/proyecto_unico.pid)"
```

## 8. Diagnóstico

```bash
cat /tmp/proyecto_unico.log
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool
```

Si no aparecen archivos, confirma que eBPF esté habilitado, que el daemon pueda cargar el programa y que el archivo se cree directamente dentro del directorio configurado. Si PAM rechaza un usuario, comprueba su contraseña, los grupos y la política del servicio PAM.

> Las capturas de pantalla de la instalación, el login de cada rol, las métricas, las alertas y el escaneo deben añadirse después de realizar las pruebas en tu VM.
