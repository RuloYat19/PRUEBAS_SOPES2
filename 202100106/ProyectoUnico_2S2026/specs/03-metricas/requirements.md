# F03 — Métricas del sistema · Requisitos

## Introducción

Completar el monitoreo de memoria que pide el enunciado (§2.2.3.1 y §2.2.4.1): leer las 9 claves de `/proc/meminfo` y los fallos de página de `/proc/vmstat` con su delta entre muestras, enviarlos al dashboard en tiempo real por SSE y dibujar las 4 gráficas requeridas. El administrador puede activar y desactivar el muestreo de métricas, y cada cambio genera la alerta del catálogo de F02.

Fuera de alcance (F04): top 20 de procesos, % de memoria por proceso, activar/desactivar el escaneo de procesos y matar procesos. El listado de procesos de P4 sigue igual en esta feature.

## Glosario

- **Muestra:** lectura de `/proc/meminfo` + `/proc/vmstat` en un instante, con `id` creciente.
- **Delta:** diferencia de `pgfault`/`pgmajfault` contra la muestra anterior del mismo período activo.
- **Período activo:** tramo entre una activación del muestreo y la siguiente desactivación.

Formato de los criterios: *CUANDO / SI / MIENTRAS … EL <componente> DEBERÁ …*

---

## R1 — Lectura de métricas

1. EL hilo `monitoreo` DEBERÁ leer de `/proc/meminfo`, en kB: `MemTotal`, `MemFree`, `MemAvailable`, `Buffers`, `Cached`, `Active`, `Inactive`, `SwapTotal` y `SwapFree`.
2. EL hilo `monitoreo` DEBERÁ leer de `/proc/vmstat` los contadores `pgfault` y `pgmajfault`.
3. Cada muestra DEBERÁ incluir `pgfault_delta` y `pgmajfault_delta`, calculados contra la muestra anterior del mismo período activo.
4. La primera muestra de un período activo (al arrancar el daemon o al reactivar el muestreo) DEBERÁ reportar delta `0` y `"primera":true`.
5. SI un contador actual es menor que el anterior, su delta DEBERÁ ser `0`.
6. Cada muestra DEBERÁ incluir estos derivados, en kB, que suman `MemTotal`:
   - `usada` = `MemTotal − MemFree − Buffers − Cached` (con piso en 0);
   - `cache` = `Buffers + Cached`;
   - `libre` = `MemFree`;
   - y además `swap_usada` = `SwapTotal − SwapFree`.
7. SI falta una clave o un archivo no se puede leer, EL daemon DEBERÁ registrar el error, descartar esa muestra y seguir; no DEBERÁ terminar.
8. La muestra DEBERÁ tomarse con el `intervalo` de `daemon.conf`, como en F00.

## R2 — Historial y API

1. EL daemon DEBERÁ guardar las últimas **150** muestras (5 minutos con intervalo 2 s), protegidas con su propio mutex.
2. `GET /api/metricas` (rol `common_user`) DEBERÁ responder `200` con:

   ```json
   {"activo":true,"intervalo_s":2,
    "muestras":[{"id":1,"timestamp":"2026-10-18 14:00:00","primera":true,
      "mem_total":…,"mem_libre":…,"mem_disponible":…,"buffers":…,"cached":…,
      "activas":…,"inactivas":…,"swap_total":…,"swap_libre":…,
      "usada":…,"cache":…,"libre":…,"swap_usada":…,
      "pgfault":…,"pgmajfault":…,"pgfault_delta":…,"pgmajfault_delta":…}, …]}
   ```

   con las muestras de la más antigua a la más reciente.
3. CUANDO se tome una muestra, EL daemon DEBERÁ enviarla a los clientes SSE como `event: metricas` con `id: <id>` y la muestra en `data:`.
4. `GET /api/estado` DEBERÁ conservar su contrato de F02 (`memoria_usada`, `memoria_libre`, `memoria_cache`, `swap_usada`, `resumen_procesos`, `procesos`).

## R3 — Activar y desactivar métricas

1. `POST /api/monitoreo/metricas` (rol `admin_user`) DEBERÁ recibir `{"activo":true}` o `{"activo":false}`.
2. SI el cuerpo no es JSON válido o `activo` no es booleano, EL daemon DEBERÁ responder `400` con `{"error":"Solicitud invalida"}`.
3. CUANDO el estado cambie, EL daemon DEBERÁ responder `200` con `{"metricas":<estado>,"cambio":true}` y emitir:
   - al desactivar: `Monitoreo del sistema desactivado` (`MEDIUM`) con la descripción `Métricas del sistema desactivadas por <usuario>`;
   - al activar: `Monitoreo del sistema activado` (`LOW`) con `Métricas del sistema activadas por <usuario>`.
4. SI el estado pedido ya es el actual, EL daemon DEBERÁ responder `200` con `"cambio":false` y NO DEBERÁ emitir alerta.
5. MIENTRAS el muestreo esté desactivado, EL daemon NO DEBERÁ leer `/proc/meminfo` ni `/proc/vmstat`, ni emitir `event: metricas`. El historial se conserva.
6. CUANDO se reactive, la siguiente muestra DEBERÁ tomarse en menos de 1 s y ser la primera de un período nuevo (R1.4).
7. `GET /api/monitoreo` (rol `common_user`) DEBERÁ responder `{"metricas":true|false}`. F04 y F05 agregarán sus claves.
8. CUANDO el estado cambie, EL daemon DEBERÁ enviar a los clientes SSE `event: monitoreo` con el mismo cuerpo de `GET /api/monitoreo`.
9. El listado de procesos de P4 NO DEBERÁ detenerse al desactivar las métricas.
10. Al arrancar el daemon, el muestreo empieza activo y no se emite alerta.

## R4 — Dashboard

1. EL dashboard DEBERÁ mostrar 4 gráficas alimentadas por `event: metricas`, sin sondeo de métricas:
   1. **Páginas activas vs. inactivas** — `activas` e `inactivas` en el tiempo.
   2. **Memoria y swap en el tiempo** — `usada` y `swap_usada` en el tiempo.
   3. **Memoria libre vs. caché vs. utilizada** — dona con `libre`, `cache` y `usada` de la última muestra, en GB y % de `mem_total`.
   4. **Fallos de página menores vs. mayores** — `pgfault_delta` y `pgmajfault_delta` en el tiempo, cada uno con su propia escala (dos gráficas de líneas apiladas con el mismo eje de tiempo).
2. CUANDO se abra o reabra el SSE, el dashboard DEBERÁ cargar el historial con `GET /api/metricas` para no empezar con gráficas vacías, y descartar los `id` repetidos.
3. Las series de tiempo DEBERÁN mostrar como máximo las últimas 150 muestras.
4. EL administrador DEBERÁ ver un interruptor **Métricas del sistema** que llama a `POST /api/monitoreo/metricas` y refleja el estado real según `event: monitoreo`.
5. EL usuario común NO DEBERÁ ver el interruptor; DEBERÁ ver el estado como texto (`Métricas activas` / `Métricas desactivadas`).
6. MIENTRAS las métricas estén desactivadas, las 4 gráficas DEBERÁN mostrar el aviso `Monitoreo de métricas desactivado` y conservar los últimos datos.
7. Los valores de memoria se muestran en GB con 2 decimales; los fallos de página, como enteros por intervalo.

## R5 — Pruebas y paridad

1. `make -C ProgramaIntermedio pruebas` DEBERÁ incluir `pruebas/test_metricas.c` (parser de meminfo/vmstat con archivos de ejemplo, deltas, primera muestra, contador que retrocede, derivados que suman `MemTotal`, historial de 150, JSON) y terminar con código 0, sin root.
2. EL proyecto DEBERÁ incluir `scripts/pruebas_metricas.sh` (`sudo`, VM) que verifique R1–R3 contra el daemon real.
3. `pruebas_integracion.sh`, `pruebas_auth.sh` y `pruebas_alertas.sh` DEBERÁN seguir terminando con `0 FALLIDAS`.
4. `make -C ProgramaIntermedio clean all` sin warnings y `daemon_monitor` versionado.
5. El hilo `ipc` DEBERÁ seguir con 0 tics de CPU en reposo (fase 6 de `pruebas_integracion.sh`).
