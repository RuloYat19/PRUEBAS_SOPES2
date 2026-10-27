# Revisión F02 — Motor de alertas y tiempo real

Fecha: 2026-10-18 · Revisor: Claude · Commits revisados: `e42be299b2`…`f9c93c19d3`

## Veredicto

**Aprobada.** B1 y las tres advertencias se corrigieron en `cccfdda5ae` y la validación en la VM con `sudo` pasó completa. Comprobación en el navegador confirmada por el usuario.

## Verificación independiente (sin root, en la VM)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all \| grep -ci warning` | 0 |
| `make -C ProgramaIntermedio pruebas` | código 0; `test_alertas` 9/9 (catálogo, ids, capacidad 205→6..205, truncado, JSON con cJSON, 4×100 hilos) |
| Árbol de trabajo tras recompilar | limpio (binario reproducible) |
| Catálogo (`alertas.c:16`) | los 10 textos, tipos y severidades coinciden con R1.1 |
| `insertar()` | `registrar` y `eventfd_write` fuera de `lock_alertas` ✓ |
| `alertas_desde` / `alertas_recientes` | ventana correcta al pasar de 200; orden ascendente/descendente ✓ |
| Dashboard | sin `innerHTML`; `addEventListener('alerta')`; duplicados descartados por `id`; recorte a 200 con resumen consistente |
| Comentarios en archivos nuevos o tocados | ninguno |

## Hallazgo bloqueante

### B1 — `pruebas_integracion.sh:207` ("Los eventos conservan el orden de escritura") va a fallar

- La verificación extrae `rafaga N` de cada trama `data:` con `grep -o 'rafaga [0-9]*'`. En P4 la trama tenía la línea una sola vez (`contenido`). Ahora la alerta la trae **dos veces**: en `descripcion` (`'INTRUSO' en …/b.log: INTRUSO rafaga N`) y en `contenido`.
- Resultado: la secuencia sale `1 1 2 2 3 3 …` y nunca es igual a `seq -s' ' 20`.
- Reproducido con una trama de ejemplo con el formato de `json_alerta`: `grep -o 'rafaga [0-9]*' | awk '{print $2}' | paste -sd' '` → `1 1`.
- **Corrección:** extraer solo del campo `contenido`, por ejemplo con `grep -o '"contenido":"[^"]*rafaga [0-9]*' | grep -o 'rafaga [0-9]*'`. Revisar con el mismo criterio cualquier otro `grep` sobre `sse.txt` que cuente coincidencias dentro de la trama.
- **Corregido:** En `scripts/pruebas_integracion.sh:207` se extrae `rafaga N` acotando la búsqueda al campo `contenido`: `grep -o '"contenido":"[^"]*rafaga [0-9]*' | grep -o 'rafaga [0-9]*'`. Ningún otro grep sobre `sse.txt` cuenta dentro de la trama. Evidencia: `bash -n scripts/pruebas_integracion.sh` código 0; `grep -c "comprobar\|aprobar"` se mantiene en 41.

## Advertencias (no bloquean)

1. `scripts/pruebas_alertas.sh:115, 131, 170` pasan la contraseña de prueba como argumento (`-d "$(login_json …)"`), así que queda visible en `ps` mientras corre `curl`. El diseño de F01 (§10.2) definió `login_json … | curl -d @-` justamente para evitarlo, y `pruebas_auth.sh` ya lo hace así. Usar stdin.
   - **Corregido:** En `scripts/pruebas_alertas.sh` se envían todos los payloads de login por stdin con `-d @-` (usando `login_json ... | curl -d @-` y `printf ... | curl -d @-`). Evidencia: `bash -n scripts/pruebas_alertas.sh` código 0; `grep -n "curl.*-d \"" scripts/pruebas_alertas.sh` sin resultados.
2. `scripts/pruebas_alertas.sh:132` espera la alerta SSE hasta **1.5 s**, pero R3.3 exige menos de **1 s**. Ajustar el límite a 1 s para que la prueba mida el requisito.
   - **Corregido:** En `scripts/pruebas_alertas.sh` fase 3 se ajustó el límite de espera SSE a exactamente 1 s (`t_fin=$((t_inicio + 1000000000))`). Evidencia: `bash -n scripts/pruebas_alertas.sh` código 0.
3. `alertas.c:124, 145`: `vsnprintf`/`snprintf` truncan la descripción en el byte 255 y pueden cortar un carácter UTF-8 a la mitad (por ejemplo, una línea de log con tildes). El JSON resultante tendría UTF-8 inválido. Hoy los productores casi no generan descripciones tan largas, pero `log_watch` sí puede. Retroceder el corte hasta el inicio del carácter (bytes `10xxxxxx`) y agregar un caso a `test_alertas.c`.
   - **Corregido:** En `ProgramaIntermedio/alertas.c` se implementó `truncar_utf8`, retrocediendo el corte hasta el inicio del carácter multi-byte incompleto para no dejar bytes de continuación `10xxxxxx` o encabezados huérfanos. Se agregó el Caso 10 a `test_alertas.c` probando líneas largas con tildes (`ñ`, `á`, `é`, `í`, `ó`, `ú`) y validación de UTF-8 y cJSON. Evidencia: `make -C ProgramaIntermedio clean all pruebas` con 0 warnings y código 0 (10/10 casos aprobados).

## Pendiente en VM (requiere `sudo`, después de corregir B1)

```bash
make -C ProgramaIntermedio clean all pruebas
sudo scripts/pruebas_alertas.sh          # 0 FALLIDAS
sudo scripts/pruebas_auth.sh             # 0 FALLIDAS
sudo scripts/pruebas_integracion.sh      # 0 FALLIDAS, 38 aprobadas
getent passwd | grep -c '^so2t_'         # 0
```

En el navegador:
- un login fallido hecho en otra pestaña aparece arriba, en ámbar y sin recargar;
- una línea con la palabra clave en el log aparece en ámbar con la palabra resaltada;
- el resumen `LOW · MEDIUM · HIGH` se actualiza.

## Segunda ronda — validación en VM (commit `cccfdda5ae`)

| Chequeo | Resultado |
|---|---|
| `make -C ProgramaIntermedio clean all pruebas` | 0 warnings; config 9, peticion 3, auth 11, alertas 10/10 |
| `sudo scripts/pruebas_alertas.sh` | 15 aprobadas, 0 fallidas (SSE con límite de 1 s) |
| `sudo scripts/pruebas_auth.sh` | 21 aprobadas, 0 fallidas |
| `sudo scripts/pruebas_integracion.sh` | 38 aprobadas, 0 fallidas (incluye el orden de la ráfaga, B1) |
| `getent passwd \| grep -c '^so2t_'` | 0 |
| `truncar_utf8` (`alertas.c:106`) | retrocede hasta el inicio del carácter incompleto; se aplica a `descripcion` y `contenido` ✓ |

Comprobación manual (usuario, 2026-10-18), en el navegador ✓:
- un login fallido en otra pestaña aparece arriba, en ámbar y sin recargar;
- una línea con la palabra clave aparece en ámbar con la palabra resaltada;
- el resumen `LOW · MEDIUM · HIGH` se actualiza.
