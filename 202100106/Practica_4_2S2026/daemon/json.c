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

void json_evento(struct buf *b, const struct evento *e)
{
	buf_printf(b, "{\"id\":%lu,\"archivo_origen\":", e->id);
	json_cadena(b, e->archivo);
	buf_printf(b, ",\"palabra_detectada\":");
	json_cadena(b, e->palabra);
	buf_printf(b, ",\"contenido\":");
	json_cadena(b, e->contenido);
	buf_printf(b, ",\"timestamp\":");
	json_cadena(b, e->timestamp);
	buf_agregar(b, "}", 1);
}

void json_estado(struct buf *b)
{
	unsigned long desde;
	unsigned long s;
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

	buf_printf(b, "],\"eventos_seguridad\":[");
	desde = estado.total_eventos > MAX_EVENTOS ?
		estado.total_eventos - MAX_EVENTOS : 0;
	for (s = desde; s < estado.total_eventos; s++) {
		if (s > desde)
			buf_agregar(b, ",", 1);
		json_evento(b, &estado.eventos[s % MAX_EVENTOS]);
	}
	buf_agregar(b, "]}", 2);

	pthread_mutex_unlock(&estado.lock);
}
