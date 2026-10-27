#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comun.h"

static int buf_reservar(struct buf *b, size_t extra)
{
	size_t nueva;
	char *p;

	if (b->fallo)
		return -1;
	if (b->largo + extra + 1 <= b->capacidad)
		return 0;

	nueva = b->capacidad ? b->capacidad : 1024;
	while (nueva < b->largo + extra + 1)
		nueva *= 2;

	p = realloc(b->datos, nueva);
	if (!p) {
		b->fallo = 1;
		return -1;
	}
	b->datos = p;
	b->capacidad = nueva;
	return 0;
}

void buf_agregar(struct buf *b, const void *datos, size_t largo)
{
	if (buf_reservar(b, largo) < 0)
		return;
	memcpy(b->datos + b->largo, datos, largo);
	b->largo += largo;
	b->datos[b->largo] = '\0';
}

void buf_printf(struct buf *b, const char *formato, ...)
{
	va_list args;
	va_list copia;
	int necesarios;

	va_start(args, formato);
	va_copy(copia, args);
	necesarios = vsnprintf(NULL, 0, formato, copia);
	va_end(copia);

	if (necesarios >= 0 && buf_reservar(b, (size_t)necesarios) == 0) {
		vsnprintf(b->datos + b->largo, (size_t)necesarios + 1, formato, args);
		b->largo += (size_t)necesarios;
	}
	va_end(args);
}

void buf_liberar(struct buf *b)
{
	free(b->datos);
	memset(b, 0, sizeof(*b));
}

void json_cadena(struct buf *b, const char *texto)
{
	const unsigned char *c;

	buf_agregar(b, "\"", 1);
	for (c = (const unsigned char *)texto; *c; c++) {
		if (*c == '"' || *c == '\\')
			buf_printf(b, "\\%c", *c);
		else if (*c < 0x20)
			buf_printf(b, "\\u%04x", *c);
		else
			buf_agregar(b, c, 1);
	}
	buf_agregar(b, "\"", 1);
}

void json_alerta(struct buf *b, const struct alerta *a)
{
	buf_printf(b, "{\"id\":%lu,\"tipo\":", a->id);
	json_cadena(b, alerta_tipo_texto(a->tipo));
	buf_printf(b, ",\"motivo\":");
	json_cadena(b, alerta_motivo_texto(a->motivo));
	buf_printf(b, ",\"descripcion\":");
	json_cadena(b, a->descripcion);
	buf_printf(b, ",\"severidad\":");
	json_cadena(b, alerta_severidad_texto(a->severidad));
	buf_printf(b, ",\"timestamp\":");
	json_cadena(b, a->timestamp);

	if (a->tipo == TIPO_LOGS) {
		buf_printf(b, ",\"archivo_origen\":");
		json_cadena(b, a->archivo);
		buf_printf(b, ",\"palabra_detectada\":");
		json_cadena(b, a->palabra);
		buf_printf(b, ",\"contenido\":");
		json_cadena(b, a->contenido);
	}

	buf_agregar(b, "}", 1);
}

void json_estado(struct buf *b)
{
	size_t i;

	pthread_mutex_lock(&estado.lock);

	buf_printf(b,
		   "{\"memoria_usada\":%lu,\"memoria_libre\":%lu,"
		   "\"memoria_cache\":%lu,\"swap_usada\":%lu,"
		   "\"resumen_procesos\":{\"total\":%u,\"running\":%u,"
		   "\"sleeping\":%u,\"zombie\":%u},\"procesos\":[",
		   estado.memoria_usada, estado.memoria_libre,
		   estado.memoria_cache, estado.swap_usada,
		   estado.resumen.total, estado.resumen.running,
		   estado.resumen.sleeping, estado.resumen.zombie);

	for (i = 0; i < estado.n_procesos; i++) {
		buf_printf(b, "%s{\"nombre\":", i ? "," : "");
		json_cadena(b, estado.procesos[i].nombre);
		buf_printf(b, ",\"pid\":%d,\"estado\":\"%c\"}",
			   estado.procesos[i].pid, estado.procesos[i].estado);
	}

	buf_agregar(b, "]}", 2);

	pthread_mutex_unlock(&estado.lock);
}

void json_muestra(struct buf *b, const struct muestra *m)
{
	buf_printf(b,
		   "{\"id\":%lu,\"timestamp\":",
		   m->id);
	json_cadena(b, m->timestamp);
	buf_printf(b,
		   ",\"primera\":%s,"
		   "\"mem_total\":%lu,\"mem_libre\":%lu,\"mem_disponible\":%lu,"
		   "\"buffers\":%lu,\"cached\":%lu,\"activas\":%lu,\"inactivas\":%lu,"
		   "\"swap_total\":%lu,\"swap_libre\":%lu,"
		   "\"usada\":%lu,\"cache\":%lu,\"libre\":%lu,\"swap_usada\":%lu,"
		   "\"pgfault\":%llu,\"pgmajfault\":%llu,"
		   "\"pgfault_delta\":%llu,\"pgmajfault_delta\":%llu}",
		   m->primera ? "true" : "false",
		   m->mem_total, m->mem_libre, m->mem_disponible,
		   m->buffers, m->cached, m->activas, m->inactivas,
		   m->swap_total, m->swap_libre,
		   m->usada, m->cache, m->libre, m->swap_usada,
		   m->pgfault, m->pgmajfault,
		   m->pgfault_delta, m->pgmajfault_delta);
}

void json_monitoreo(struct buf *b)
{
	struct estado_sensor est;

	analisis_estado_sensor(&est);
	buf_printf(b, "{\"metricas\":%s,\"procesos\":%s,\"archivos\":%s,\"ruta_vigilada\":",
		   metricas_activas() ? "true" : "false",
		   procesos_activos() ? "true" : "false",
		   est.activo ? "true" : "false");
	json_cadena(b, est.ruta);
	buf_printf(b, ",\"sensor_error\":");
	json_cadena(b, est.error);
	buf_agregar(b, "}", 1);
}

void json_archivo(struct buf *b, const struct archivo_analizado *a)
{
	const char *slash;
	const char *nombre;
	const char *estado_str;
	const char *accion_str;
	char mtime_str[32];
	struct tm tm_mtime;

	if (!b || !a)
		return;

	slash = strrchr(a->ruta, '/');
	nombre = slash ? slash + 1 : a->ruta;

	localtime_r(&a->mtime.tv_sec, &tm_mtime);
	strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M:%S", &tm_mtime);

	estado_str = (a->estado == ARCHIVO_INFECTADO) ? "infectado" : "benigno";

	if (a->accion == ACCION_CREADO)
		accion_str = "creado";
	else if (a->accion == ACCION_MODIFICADO)
		accion_str = "modificado";
	else
		accion_str = "inicial";

	buf_printf(b, "{\"ruta\":");
	json_cadena(b, a->ruta);
	buf_printf(b, ",\"nombre\":");
	json_cadena(b, nombre);
	buf_printf(b, ",\"sha256\":");
	json_cadena(b, a->sha256);
	buf_printf(b, ",\"firma\":");
	json_cadena(b, a->firma);
	buf_printf(b, ",\"tamano\":%lld,\"mtime\":", a->tamano);
	json_cadena(b, mtime_str);
	buf_printf(b, ",\"estado\":");
	json_cadena(b, estado_str);
	buf_printf(b, ",\"cuarentena\":%s", a->cuarentena ? "true" : "false");
	buf_printf(b, ",\"accion\":");
	json_cadena(b, accion_str);
	buf_printf(b, ",\"pid\":%u,\"proceso\":", a->pid);
	json_cadena(b, a->proceso);
	buf_printf(b, ",\"analizado\":");
	json_cadena(b, a->analizado);
	buf_agregar(b, "}", 1);
}

unsigned long json_archivos(struct buf *b)
{
	struct estado_sensor est;
	struct archivo_analizado *arr;
	size_t n;
	unsigned long mayor_seq = 0;

	if (!b)
		return 0;

	analisis_estado_sensor(&est);

	arr = malloc(sizeof(struct archivo_analizado) * MAX_ARCHIVOS);
	if (!arr)
		return 0;

	n = analisis_listado(arr, MAX_ARCHIVOS);

	buf_printf(b, "{\"activo\":%s,\"ruta\":", est.activo ? "true" : "false");
	json_cadena(b, est.ruta);
	buf_agregar(b, ",\"archivos\":[", 13);
	for (size_t i = 0; i < n; i++) {
		if (i > 0)
			buf_agregar(b, ",", 1);
		json_archivo(b, &arr[i]);
		if (arr[i].seq > mayor_seq)
			mayor_seq = arr[i].seq;
	}
	buf_agregar(b, "]}", 2);

	free(arr);
	return mayor_seq;
}

void json_procesos(struct buf *b, const struct instantanea_procesos *p)
{
	size_t i;

	buf_printf(b, "{\"activo\":%s,\"timestamp\":",
		   procesos_activos() ? "true" : "false");
	json_cadena(b, (p && p->valida) ? p->timestamp : "");
	buf_printf(b, ",\"resumen\":{\"total\":%u,\"running\":%u,\"sleeping\":%u,\"zombie\":%u},\"top\":[",
		   (p && p->valida) ? p->resumen.total : 0,
		   (p && p->valida) ? p->resumen.running : 0,
		   (p && p->valida) ? p->resumen.sleeping : 0,
		   (p && p->valida) ? p->resumen.zombie : 0);

	if (p && p->valida) {
		for (i = 0; i < p->n_top; i++) {
			double pct = procesos_porcentaje(p->top[i].rss_kb, p->mem_total_kb);
			buf_printf(b, "%s{\"pid\":%d,\"nombre\":",
				   i ? "," : "", (int)p->top[i].pid);
			json_cadena(b, p->top[i].nombre);
			buf_printf(b, ",\"estado\":\"%c\",\"rss_kb\":%lu,\"porcentaje_memoria\":%.2f}",
				   p->top[i].estado, p->top[i].rss_kb, pct);
		}
	}

	buf_agregar(b, "]}", 2);
}

void json_amenaza(struct buf *b, const struct amenaza *am)
{
	if (!b || !am)
		return;

	buf_printf(b, "{\"id\":%lu,\"firma\":", am->id);
	json_cadena(b, am->firma);
	buf_printf(b, ",\"descripcion\":");
	json_cadena(b, am->descripcion);
	buf_printf(b, ",\"ruta\":");
	json_cadena(b, am->ruta);
	buf_printf(b, ",\"sha256\":");
	json_cadena(b, am->sha256);
	buf_printf(b, ",\"pid\":%u,\"proceso\":", am->pid);
	json_cadena(b, am->proceso);
	buf_printf(b, ",\"fecha\":");
	json_cadena(b, am->fecha);
	buf_printf(b, ",\"cuarentena\":%s}", am->cuarentena ? "true" : "false");
}

void json_registro_cuarentena(struct buf *b, const struct registro_cuarentena *rc)
{
	if (!b || !rc)
		return;

	buf_printf(b, "{\"ruta\":");
	json_cadena(b, rc->ruta);
	buf_printf(b, ",\"sha256\":");
	json_cadena(b, rc->sha256);
	buf_printf(b, ",\"tipo\":");
	json_cadena(b, rc->tipo);
	buf_printf(b, ",\"motivo\":");
	json_cadena(b, rc->motivo);
	buf_printf(b, ",\"usuario\":");
	json_cadena(b, rc->usuario);
	buf_printf(b, ",\"fecha\":");
	json_cadena(b, rc->fecha);
	buf_printf(b, ",\"permisos_anteriores\":");
	json_cadena(b, rc->permisos_anteriores);
	buf_agregar(b, "}", 1);
}

void json_blacklist(struct buf *b)
{
	struct info_blacklist info;
	size_t n;
	struct firma *lista = NULL;

	if (!b)
		return;

	blacklist_info(&info);
	n = info.firmas;
	if (n > 0) {
		lista = malloc(n * sizeof(struct firma));
		if (lista)
			n = blacklist_lista(lista, n);
		else
			n = 0;
	}

	buf_printf(b, "{\"nombre\":");
	json_cadena(b, info.nombre);
	buf_printf(b, ",\"version\":%d,\"actualizado\":", info.version);
	json_cadena(b, info.actualizado);
	buf_printf(b, ",\"firmas\":%zu,\"origen\":", info.firmas);
	json_cadena(b, info.origen);
	buf_printf(b, ",\"actualizada_por\":");
	json_cadena(b, info.actualizada_por);
	buf_printf(b, ",\"cargada\":");
	json_cadena(b, info.cargada);
	buf_printf(b, ",\"error\":");
	json_cadena(b, info.error);
	buf_printf(b, ",\"lista\":[");
	for (size_t i = 0; i < n; i++) {
		if (i > 0)
			buf_agregar(b, ",", 1);
		buf_printf(b, "{\"sha256\":");
		json_cadena(b, lista[i].sha256);
		buf_printf(b, ",\"nombre\":");
		json_cadena(b, lista[i].nombre);
		buf_printf(b, ",\"descripcion\":");
		json_cadena(b, lista[i].descripcion);
		buf_agregar(b, "}", 1);
	}
	buf_agregar(b, "]}", 2);
	free(lista);
}

void json_logwatch(struct buf *b, const struct logwatch *lw)
{
	if (!b || !lw)
		return;

	buf_printf(b, "{\"id\":%d,\"palabra\":", lw->id);
	json_cadena(b, lw->palabra);
	buf_printf(b, ",\"central\":");
	json_cadena(b, lw->central);
	buf_printf(b, ",\"rutas\":[");
	for (int i = 0; i < lw->n_rutas; i++) {
		if (i > 0)
			buf_agregar(b, ",", 1);
		json_cadena(b, lw->rutas[i]);
	}
	buf_printf(b, "],\"origen\":");
	json_cadena(b, lw->origen);
	buf_printf(b, ",\"usuario\":");
	json_cadena(b, lw->usuario);
	buf_printf(b, ",\"creado\":");
	json_cadena(b, lw->creado);
	buf_agregar(b, "}", 1);
}
