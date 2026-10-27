# Revisión F05 — Sensor eBPF e integridad de archivos

Fecha: 2026-10-21 · Revisor: Claude · Commits revisados: `773358dd11`…`928e205b6c`

## Veredicto

**Aprobada** (cuarta ronda). B1–B4 y todas las advertencias corregidas; `sudo scripts/correr_pruebas.sh` en verde (F05 44/44); comprobación en el navegador T8.2 confirmada por el usuario el 2026-10-21.

> Primera ronda: cambios requeridos (2 bloqueantes, ambos en `scripts/pruebas_sensor.sh`). El sensor, el daemon y el dashboard cumplen el spec. La suite completa pasó en la VM (F05 41/41 y todas las anteriores). Sin embargo, el script de pruebas **borra el `~/so2_vigilado` real del usuario**, y algunas fases aprueban sin comprobar lo que dicen. Falta además la comprobación en el navegador (T8.2).

## Verificación independiente (sin root, en la VM)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all \| grep -ci warning` | 0 |
| `make -C ProgramaIntermedio pruebas` | código 0 (58 casos ✓, incluidos los 8 de `test_analisis`) |
| Árbol tras recompilar | limpio (binario reproducible); ningún archivo de root |
| `ldd daemon_monitor` | `libbpf.so.1`, `libcrypto.so.3` ✓ |
| `sensor.bpf.c` | solo `ruta_en_directorio` y el TGID del daemon deciden; mapas y programas con los nombres del diseño; 8 secciones; licencia GPL ✓ |
| `filtro_ruta.h` | un bucle con límite constante; rechaza nombre vacío, `/` posterior, prefijo hermano y falta de `'\0'` ✓ |
| `sensor.c` | todo libbpf bajo `lock_sensor`; `pid_daemon` y `config_sensor` antes de `attach`; desactivar = `ring_buffer__free` + `destroy`; `fd_control` se escribe después de soltar el lock; el callback solo copia ✓ |
| `analisis.c` | `O_NOFOLLOW\|O_NONBLOCK`, `S_ISREG`, hash fuera del mutex, generación comprobada antes de insertar, desalojo por menor `seq`, línea base sin alertas ✓ |
| `http.c` | `leer_activo` compartido por los 3 interruptores sin cambiar respuestas; mensajes y códigos de R2.3 y R3.2 ✓ |
| `main.c` | sensor después de `daemon(3)`; `sensor_finalizar` después del `join` ✓ |
| Dashboard | sin `innerHTML`; controles con `data-rol="admin_user"`; `sensor_error` visible ✓ |
| Comentarios en archivos nuevos o tocados | ninguno |

## Hallazgos bloqueantes

### B1 — `pruebas_sensor.sh` borra `~/so2_vigilado` completo

`:97` (preparación) y `:65` (`limpiar`, en el `trap EXIT`) ejecutan `rm -rf "${USER_HOME}/so2_vigilado"`. Ese es el directorio vigilado **real** (`ruta_vigilada` de `daemon.conf`), no uno de prueba. Cada `sudo scripts/correr_pruebas.sh` lo borra con todo su contenido.

- **Comprobado:** `/home/sudockentucky/so2_vigilado` ya no existe en la VM.
- **Consecuencias:**
  - el daemon real arranca con `sensor eBPF no disponible: La ruta vigilada no es un directorio existente` hasta que alguien recree la carpeta;
  - en F06 se borrarían las muestras de la demo y los archivos en cuarentena.
- Diseño §8.2: el script crea `pruebas_sensor` y `pruebas_sensor_b` **dentro** de `~/so2_vigilado`, y el `trap` borra solo esos dos.

**Corrección:**
1. Crear `~/so2_vigilado` si no existe (`sudo -u "$SUDO_USER" mkdir -p`), sin borrarlo nunca.
2. Si `DIR_A` o `DIR_B` ya existen al empezar, abortar con un mensaje en vez de reutilizarlos.
3. Borrar solo `DIR_A` y `DIR_B` en la preparación y en `limpiar`.
4. Revisar los demás scripts por el mismo patrón: ningún `rm -rf` sobre una ruta que la prueba no creó.

- **Corregido:** En `scripts/pruebas_sensor.sh` se crea `~/so2_vigilado` con `sudo -u "$REAL_USER" mkdir -p` si no existe, se aborta con `fallar` y `trap - EXIT; exit 1` si `DIR_A` o `DIR_B` ya existen antes de iniciar, y se borran únicamente `$DIR_A` y `$DIR_B` en la preparación y en `limpiar()`. Revisados todos los scripts en `scripts/`: ninguno contiene `rm -rf` sobre rutas externas a la prueba. Evidencia: `bash -n scripts/pruebas_sensor.sh` código 0.

### B2 — Fases que aprueban sin comprobar lo que dice §8.2

| Fase | Problema |
|---|---|
| 17 (`:771–772`) | si no encuentra el TID del hilo `archivos`, llama a `aprobar`. Debe `fallar`: la prueba no midió nada. |
| 15 (`:668–676`) | la "ruta larga" es `/tmp/aaa…` y **no existe**, así que el daemon responde `La ruta no es un directorio existente` y la fase solo mira el código 400. `Ruta demasiado larga` (R3.2) nunca se prueba contra el daemon real. Hay que crear, como `$SUDO_USER` y dentro de `DIR_A`, un anidamiento de directorios cuya canónica pase de 200 caracteres, y comprobar el mensaje exacto. Nada en `/tmp` (AGENTS.md §6). |
| 7 | §8.2 pide exactamente 1 alerta **y 1 `event: archivo`** para `base1.txt`; solo cuenta alertas. |
| 8 | guarda `SSE_LEN_PREV` pero no lo usa: no comprueba que no llegue `event: archivo` con las lecturas. |
| 12, 14, 16 | no verifican `event: monitoreo` (12 y 16) ni `event: archivos` (14 y 16), que pide §8.2. El SSE abierto en la fase 5 sigue vivo y sirve para comprobarlo. |
| 5 | dice "≤ 1 s", pero espera 1,5 s antes de mirar. Hay que medir desde la escritura hasta la primera línea con `creado.txt` en el log del SSE, o cambiar el texto a lo que realmente se prueba. |

- **Corregido:** En `scripts/pruebas_sensor.sh`:
  - Fase 17: Ante ausencia de `TID_ARCHIVOS`, llama explícitamente a `fallar`.
  - Fase 15: Estructura anidada creada dentro de `DIR_A` como `$REAL_USER` con canónica > 200 chars, exigiendo 400 y `"Ruta demasiado larga"`; pruebas de ruta inexistente cambiadas a `${DIR_A}/no_existe_so2_test` (nada en `/tmp`).
  - Fase 7: Exige exactamente 1 alerta y 1 `event: archivo` para `base1.txt` en `$LOG_SSE`.
  - Fase 8: Exige 0 alertas y 0 `event: archivo` nuevos usando `SSE_LEN_PREV`.
  - Fases 12 y 16: Validan `event: monitoreo` con `archivos: false` (12) y `ruta_vigilada` nueva (16).
  - Fases 14 y 16: Validan `event: archivos` tras reactivación (14) y cambio de ruta (16).
  - Fase 5: Sondea el log SSE cada 50 ms calculando la latencia real desde la creación y verificando `<= 1.0 s`.
  - Evidencia: `bash -n scripts/pruebas_sensor.sh` código 0.

## Advertencias (no bloquean)

1. **`difundir_archivos` (`http.c`) puede perder una entrada al cambiar la generación.** `json_archivos` toma una instantánea y `analisis_listado` otra, después, para calcular `mayor_seq`. Si la línea base inserta un archivo entre las dos llamadas, ese archivo no va en `event: archivos` y su `seq` ya cuenta como enviado, así que tampoco llega como `event: archivo`. El dashboard no lo muestra hasta reconectar.
   - **Corrección:** calcular `mayor_seq` con la misma instantánea que se serializa (por ejemplo, que `json_archivos` lo devuelva o reciba el arreglo).
   - **Corregido:** `json_archivos` en `json.c` retorna `unsigned long mayor_seq` de la misma instantánea `arr` serializada. `difundir_archivos` en `http.c` consume este retorno directo eliminando el segundo `analisis_listado` y el segundo `malloc`. Evidencia: `make -C ProgramaIntermedio clean all` con 0 warnings; `make -C ProgramaIntermedio pruebas` 58/58 OK.
2. **`hilo_archivos` termina en silencio** si `poll` falla con algo distinto de `EINTR` (`break`). El escaneo queda muerto con `archivos: true`.
   - **Corrección:** `registrar` y seguir (o salir solo si `!ejecutando`).
   - **Corregido:** `sensor.c` registra el error de `poll` vía `registrar` y continúa el bucle con `continue`, saliendo solo si `!ejecutando`. Evidencia: compilación con 0 warnings y pruebas unitarias OK.
3. **El binario creció de 108 KB a 799 KB** porque el skeleton incrusta `sensor.bpf.o` con DWARF (`-g`): `.debug_info` + `.debug_str` + relocaciones ≈ 650 KB. El BTF que necesita CO-RE está en `.BTF`, no en DWARF.
   - **Corrección:** `llvm-strip -g sensor.bpf.o` después de `clang` en `ebpf/Makefile`; el skeleton y la carga no cambian.
   - **Corregido:** Añadido `llvm-strip -g $@` en `ebpf/Makefile`. El tamaño nuevo de `daemon_monitor` es de 137 KB (reducido de 781 KB, ahorro de 644 KB / 82%). Evidencia: `ls -lh ProgramaIntermedio/daemon_monitor` reporta 137 KB; compilación limpia con 0 warnings.

## Pendiente en VM (requiere `sudo`, después de corregir B3)

```bash
mkdir -p ~/so2_vigilado                 # una vez: el script anterior lo borró
sudo scripts/correr_pruebas.sh          # todas las suites con 0 FALLIDAS, 0 usuarios so2t_*
ls -d ~/so2_vigilado                    # debe seguir existiendo después de la corrida
```

Y la comprobación en el navegador de T8.2.

## Segunda ronda — commit `2124b1edd0`

| Chequeo | Resultado |
|---|---|
| B1 | ya no hay `rm -rf` sobre `~/so2_vigilado`; solo se borran `DIR_A` y `DIR_B` ✓ (pero ver B3) |
| B2 | la fase 17 falla sin TID; la fase 15 crea una canónica de 234 caracteres dentro de `DIR_A` y exige `Ruta demasiado larga`; las fases 7, 8, 12, 14 y 16 revisan el SSE; la fase 5 mide la latencia ✓ |
| Adv 1 | `json_archivos` devuelve `mayor_seq` de la misma instantánea ✓ |
| Adv 2 | `poll` con error → `registrar` y continúa ✓ |
| Adv 3 | `llvm-strip -g`; `daemon_monitor` 139 448 bytes ✓ |
| `make clean all` / `make pruebas` (revisor, sin root) | 0 warnings / código 0; árbol limpio después de recompilar |

### B3 — Si se aborta por `DIR_A`/`DIR_B`, quedan usuarios de prueba

`:94` crea los usuarios `so2t_*` y **después** `:101–104` comprueba si `DIR_A` o `DIR_B` existen. Si existen, hace `trap - EXIT` y `exit 1`, así que `limpiar` no corre y `usuarios_prueba_borrar` nunca se ejecuta.

El caso es realista: una corrida interrumpida con `SIGKILL` o `Ctrl+Z` deja las carpetas, y la siguiente aborta dejando cuentas de `admin_user` en el sistema.

**Corrección:**
- mover la comprobación (y la creación de `~/so2_vigilado`) **antes** de `usuarios_prueba_crear`;
- el mensaje de aborto debe decir qué borrar a mano (`rm -rf <DIR_A> <DIR_B>`).

Probar el camino: crear `DIR_A` a mano, correr el script, y comprobar el código 1 y `getent passwd | grep -c '^so2t_'` → 0.

- **Corregido:** En `scripts/pruebas_sensor.sh` se movieron la creación de `~/so2_vigilado` y la comprobación de existencia de `DIR_A`/`DIR_B` antes de llamar a `usuarios_prueba_condiciones` y `usuarios_prueba_crear`. El mensaje de aborto indica `elimina a mano con rm -rf ${DIR_A} ${DIR_B}`. Se agregó la bandera `PREPARADO` (inicia en 0 y pasa a 1 solo tras crear la estructura) para que `limpiar()` no borre las carpetas preexistentes si aborta al inicio.
- Evidencia: `bash -n scripts/pruebas_sensor.sh` código 0. Pendiente en VM: prueba con sudo (`mkdir -p ~/so2_vigilado/pruebas_sensor && sudo scripts/pruebas_sensor.sh` → código 1 y `getent passwd | grep -c '^so2t_'` → 0).

### Advertencias de la segunda ronda (no bloquean)

1. **Las fases 14 y 16 leen el log del SSE justo después de que responde el `POST`.**
   - `event: monitoreo` se envía antes de la respuesta, pero `event: archivos` sale en la siguiente vuelta del bucle HTTP (`fd_aviso`). Ninguna de las dos fases espera, así que pueden fallar de vez en cuando.
   - **Corrección:** esperar hasta 2 s sondeando el log, como ya hace la fase 5.
   - **Corregido:** Las fases 14 y 16 usan `esperar_evento` con sondeo cada 50 ms hasta un timeout de 2 s para esperar y confirmar `event: monitoreo` y `event: archivos`. Evidencia: `bash -n scripts/pruebas_sensor.sh` código 0.
2. **La fase 5 mide la latencia con `grep "creado.txt"`**, y ese texto también aparece en `event: alerta` (`difundir_alertas` va antes que `difundir_archivos`).
   - **Corrección:** buscar la trama `event: archivo` cuyo `data` tenga `"nombre":"creado.txt"`.
   - **Corregido:** La fase 5 usa `contar_eventos 0 "archivo" "d.get('nombre') == 'creado.txt'"` en su bucle de medición, evitando medir la alerta temprana y cronometrando la llegada real de `event: archivo`. Evidencia: `bash -n scripts/pruebas_sensor.sh` código 0.
3. **El parseo de tramas SSE está copiado 5 veces** (fases 7, 8, 12, 14 y 16).
   - **Corrección:** una función `contar_eventos <offset> <evento> <filtro>` en el script.
   - **Corregido:** Se implementaron las funciones centralizadas `contar_eventos <offset> <evento> <filtro>` y `esperar_evento <offset> <evento> <filtro> [segundos]` en `scripts/pruebas_sensor.sh`, reemplazando todas las copias de código en las fases 5, 7, 8, 12, 14 y 16. Evidencia: `bash -n scripts/pruebas_sensor.sh` código 0.

## Tercera ronda — commit `3764d7b588`

| Chequeo | Resultado |
|---|---|
| B3 | la creación de `~/so2_vigilado` y la comprobación de `DIR_A`/`DIR_B` van antes de `usuarios_prueba_crear`; el aborto indica el `rm -rf` exacto; `PREPARADO` evita borrar carpetas que el script no creó ✓ |
| Adv 1 | las fases 12, 14 y 16 usan `esperar_evento` (hasta 2 s, cada 50 ms) ✓ |
| Adv 2 | la fase 5 mide hasta `event: archivo` con `nombre == "creado.txt"` ✓ |
| Adv 3 | `contar_eventos`/`esperar_evento` reemplazan las 5 copias; los filtros son expresiones fijas del script ✓ |
| `bash -n scripts/pruebas_sensor.sh` (revisor) | código 0 |

Código y pruebas sin hallazgos abiertos. Para aprobar falta: la salida de `sudo scripts/correr_pruebas.sh` con este commit (todas en 0 FALLIDAS, `~/so2_vigilado` existe al final y `getent passwd | grep -c '^so2t_'` → 0) y la comprobación en el navegador de T8.2.

## Validación en VM — `sudo scripts/correr_pruebas.sh` (commit `3764d7b588`, usuario)

| Suite | Resultado |
|---|---|
| Unitarias en C | OK (`test_analisis` 8/8; `test_auth` con 16 filas) |
| F05 Sensor eBPF | **44/44**; latencia `event: archivo` 0,271 s; 0 ticks del hilo `archivos` en reposo; sensor descargado tras `SIGTERM` |
| F04 Procesos | **19/1**: fase 15, `Hilo monitoreo consumió tics en reposo (de 59 a 60)` |
| F03, F02, F01, integración | 18/0, 15/0, 21/0, 38/0 |
| Usuarios `so2t_*` residuales | 0 |
| Aborto de B3 (corrida a mano con `pruebas_sensor` preexistente) | sale con el mensaje y el `rm -rf` exacto; `getent passwd \| grep -c '^so2t_'` → 0 ✓ |

### B4 — Fase 15 de `pruebas_procesos.sh`: se mide durante la última espera legítima (no es una regresión del daemon)

**Causa:**
- `metricas_cambiar_estado` y `procesos_cambiar_estado` despiertan el hilo solo al **activar**. Al desactivar, el hilo sigue en `pthread_cond_timedwait` hasta que vence el intervalo (`intervalo = 3`). Ahí despierta una vez, ve todo apagado y entra en la espera sin límite. Es el comportamiento del diseño de F04 §4.
- La fase espera 1 s después de desactivar y mide 5 s, así que ese despertar puede caer dentro de la ventana y a veces el kernel le carga un tick.
- Con F05 el daemon carga más cosas al arrancar, y eso solo movió la fase del ciclo en que cae la medición.

**Lo que no es corrección:** el commit `798d7d3c2d` (hecho después de esta corrida) aceptó `diff_tics <= 1`. Eso debilita la prueba: tolera un despertar periódico por tiempo, que es justo lo que R3.6 de F04 prohíbe. La fase 17 de `pruebas_sensor.sh` tiene la misma tolerancia `<= 1`.

**Corrección** (en las dos fases):
1. Esperar `intervalo + 1` s (4 s en F04) después de desactivar y antes de la primera lectura. En F05 basta 1 s después de la fase 16.
2. Exigir **0** ticks (`utime + stime`) **y** 0 cambios en `voluntary_ctxt_switches` de `/proc/<pid>/task/<tid>/status`, como hace `pruebas_integracion.sh` en su fase 6. Contar despertares es exacto; los ticks se cargan por muestreo.
3. Quitar la tolerancia `<= 1` de `798d7d3c2d` y la de la fase 17 de F05.

- **Corregido:**
  - En `scripts/pruebas_procesos.sh` (fase 15): se amplió la espera tras desactivar a 4 s (`sleep 4`), se eliminó la tolerancia `<= 1` y se exige estrictamente 0 ticks (`utime + stime`) y 0 cambios en `voluntary_ctxt_switches` de `/proc/<pid>/task/<tid>/status` durante los 5 s de reposo.
  - En `scripts/pruebas_sensor.sh` (fase 17): se agregó 1 s de espera previa (`sleep 1`), se eliminó la tolerancia `<= 1` y se exige estrictamente 0 ticks y 0 cambios en `voluntary_ctxt_switches` durante 5 s.
  - Daemon sin modificaciones.
  - Evidencia: `bash -n scripts/pruebas_procesos.sh scripts/pruebas_sensor.sh` código 0.

## Pendiente para aprobar

1. B4 corregido y una corrida de `sudo scripts/correr_pruebas.sh` con todas las suites en 0 FALLIDAS.
2. La comprobación en el navegador de T8.2, confirmada por el usuario.

## Cuarta ronda — commits `229d21959d`, `928e205b6c`

| Chequeo | Resultado |
|---|---|
| B4, F04 fase 15 | espera 4 s (intervalo + 1); exige 0 ticks y 0 `voluntary_ctxt_switches`; sin tolerancia ✓ |
| B4, F05 fase 17 | espera 1 s; mismo criterio, sin tolerancia ✓ |
| `928e205b6c` (fuera de ronda) | solo CSS: `#panel-archivos` a ancho completo, estilos del formulario de ruta, `#sensor-error` y avisos compartidos con procesos ✓ |
| `sudo scripts/correr_pruebas.sh` (usuario, con `229d21959d`) | unitarias OK, F05 **44/44**, F04 **20/20**, F03 18/18, F02 15/15, F01 21/21, integración 38/38, 0 usuarios `so2t_*` |

Sin hallazgos abiertos. Solo falta la confirmación del usuario de la comprobación en el navegador (T8.2) para marcar F05 como **Aprobada**.

## Cierre

Comprobación en el navegador (T8.2) confirmada por el usuario el 2026-10-21: archivos creados y modificados en vivo con alerta azul, interruptor con aviso y alertas ámbar/azul, cambio de ruta y error de ruta inválida, y vista del usuario común sin controles. **F05 aprobada.**
