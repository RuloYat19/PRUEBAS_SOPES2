#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>

#include "comun.h"

struct entrada_catalogo {
	const char *texto;
	enum tipo_alerta tipo;
	enum severidad severidad;
};

static const struct entrada_catalogo catalogo[MOT_TOTAL] = {
	[MOT_ARCHIVO_MODIFICADO]    = { "Archivo modificado",                 TIPO_ARCHIVOS, SEV_LOW },
	[MOT_MONITOREO_ACTIVADO]    = { "Monitoreo del sistema activado",     TIPO_SISTEMA,  SEV_LOW },
	[MOT_SESION_EXITOSA]        = { "Inicio de sesión exitoso",           TIPO_SESION,   SEV_LOW },
	[MOT_MONITOREO_DESACTIVADO] = { "Monitoreo del sistema desactivado",  TIPO_SISTEMA,  SEV_MEDIUM },
	[MOT_SESION_FALLIDA]        = { "Inicio de sesión fallido",           TIPO_SESION,   SEV_MEDIUM },
	[MOT_PALABRA_EN_LOG]        = { "Palabra clave detectada en log",     TIPO_LOGS,     SEV_MEDIUM },
	[MOT_BLACKLIST_ACTUALIZADA] = { "Actualización base de datos local",  TIPO_ARCHIVOS, SEV_MEDIUM },
	[MOT_PROCESO_MATADO]        = { "Proceso matado",                     TIPO_SISTEMA,  SEV_MEDIUM },
	[MOT_HASH_MALICIOSO]        = { "Hash malicioso detectado",           TIPO_ARCHIVOS, SEV_HIGH },
	[MOT_ARCHIVO_CUARENTENA]    = { "Archivo puesto en cuarentena",       TIPO_ARCHIVOS, SEV_HIGH },
};

static struct alerta historial[MAX_ALERTAS];
static unsigned long total_alertas;
static pthread_mutex_t lock_alertas = PTHREAD_MUTEX_INITIALIZER;

const char *alerta_motivo_texto(enum motivo_alerta m)
{
	if (m < 0 || m >= MOT_TOTAL)
		return "";
	return catalogo[m].texto;
}

enum tipo_alerta alerta_tipo(enum motivo_alerta m)
{
	if (m < 0 || m >= MOT_TOTAL)
		return TIPO_SISTEMA;
	return catalogo[m].tipo;
}

enum severidad alerta_severidad(enum motivo_alerta m)
{
	if (m < 0 || m >= MOT_TOTAL)
		return SEV_LOW;
	return catalogo[m].severidad;
}

const char *alerta_tipo_texto(enum tipo_alerta t)
{
	switch (t) {
	case TIPO_ARCHIVOS:
		return "archivos";
	case TIPO_SISTEMA:
		return "sistema";
	case TIPO_SESION:
		return "sesion";
	case TIPO_LOGS:
		return "logs";
	default:
		return "";
	}
}

const char *alerta_severidad_texto(enum severidad s)
{
	switch (s) {
	case SEV_LOW:
		return "LOW";
	case SEV_MEDIUM:
		return "MEDIUM";
	case SEV_HIGH:
		return "HIGH";
	default:
		return "";
	}
}

static unsigned long insertar(const struct alerta *a)
{
	struct alerta copia;

	pthread_mutex_lock(&lock_alertas);
	total_alertas++;
	copia = *a;
	copia.id = total_alertas;
	historial[(total_alertas - 1) % MAX_ALERTAS] = copia;
	pthread_mutex_unlock(&lock_alertas);

	if (fd_aviso >= 0) {
		eventfd_t val = 1;
		eventfd_write(fd_aviso, val);
	}
	registrar("ALERTA [%s] %s: %s",
		  alerta_severidad_texto(copia.severidad),
		  alerta_motivo_texto(copia.motivo),
		  copia.descripcion);
	return copia.id;
}

static void truncar_utf8(char *buf, size_t cap)
{
	size_t len;
	size_t i;
	size_t esperados;
	size_t disponibles;
	unsigned char c;

	if (!buf || cap == 0)
		return;
	buf[cap - 1] = '\0';
	len = strlen(buf);
	if (len == 0)
		return;

	i = len;
	while (i > 0 && ((unsigned char)buf[i - 1] & 0xC0) == 0x80)
		i--;

	if (i == 0) {
		buf[0] = '\0';
		return;
	}

	c = (unsigned char)buf[i - 1];
	if ((c & 0x80) == 0) {
		return;
	} else if ((c & 0xE0) == 0xC0) {
		esperados = 2;
	} else if ((c & 0xF0) == 0xE0) {
		esperados = 3;
	} else if ((c & 0xF8) == 0xF0) {
		esperados = 4;
	} else {
		buf[i - 1] = '\0';
		return;
	}

	disponibles = len - (i - 1);
	if (disponibles < esperados)
		buf[i - 1] = '\0';
}

unsigned long alerta_emitir(enum motivo_alerta m, const char *formato, ...)
{
	struct alerta a;
	va_list ap;
	time_t ahora;
	struct tm tm_info;

	if (m < 0 || m >= MOT_TOTAL) {
		registrar("motivo de alerta invalido: %d", (int)m);
		return 0;
	}

	memset(&a, 0, sizeof(a));
	a.motivo = m;
	a.tipo = catalogo[m].tipo;
	a.severidad = catalogo[m].severidad;

	va_start(ap, formato);
	vsnprintf(a.descripcion, sizeof(a.descripcion), formato, ap);
	va_end(ap);
	truncar_utf8(a.descripcion, sizeof(a.descripcion));

	ahora = time(NULL);
	localtime_r(&ahora, &tm_info);
	strftime(a.timestamp, sizeof(a.timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);

	return insertar(&a);
}

unsigned long alerta_emitir_log(const char *palabra, const char *archivo,
				const char *contenido, time_t epoca)
{
	struct alerta a;
	struct tm tm_info;

	memset(&a, 0, sizeof(a));
	a.motivo = MOT_PALABRA_EN_LOG;
	a.tipo = catalogo[MOT_PALABRA_EN_LOG].tipo;
	a.severidad = catalogo[MOT_PALABRA_EN_LOG].severidad;

	snprintf(a.descripcion, sizeof(a.descripcion), "'%s' en %s: %s",
		 palabra ? palabra : "", archivo ? archivo : "", contenido ? contenido : "");
	truncar_utf8(a.descripcion, sizeof(a.descripcion));

	if (archivo)
		snprintf(a.archivo, sizeof(a.archivo), "%s", archivo);
	if (palabra)
		snprintf(a.palabra, sizeof(a.palabra), "%s", palabra);
	if (contenido) {
		snprintf(a.contenido, sizeof(a.contenido), "%s", contenido);
		truncar_utf8(a.contenido, sizeof(a.contenido));
	}

	localtime_r(&epoca, &tm_info);
	strftime(a.timestamp, sizeof(a.timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);

	return insertar(&a);
}

size_t alertas_desde(unsigned long ultimo_id, struct alerta *destino, size_t max)
{
	size_t n = 0;

	pthread_mutex_lock(&lock_alertas);
	if (total_alertas > 0 && max > 0) {
		unsigned long inicio = (total_alertas > MAX_ALERTAS) ? (total_alertas - MAX_ALERTAS + 1) : 1;
		unsigned long desde = (ultimo_id < inicio) ? inicio : (ultimo_id + 1);
		while (desde <= total_alertas && n < max) {
			destino[n++] = historial[(desde - 1) % MAX_ALERTAS];
			desde++;
		}
	}
	pthread_mutex_unlock(&lock_alertas);
	return n;
}

size_t alertas_recientes(struct alerta *destino, size_t max)
{
	size_t n = 0;

	pthread_mutex_lock(&lock_alertas);
	if (total_alertas > 0 && max > 0) {
		unsigned long inicio = (total_alertas > MAX_ALERTAS) ? (total_alertas - MAX_ALERTAS + 1) : 1;
		unsigned long cur = total_alertas;
		while (cur >= inicio && n < max) {
			destino[n++] = historial[(cur - 1) % MAX_ALERTAS];
			if (cur == inicio)
				break;
			cur--;
		}
	}
	pthread_mutex_unlock(&lock_alertas);
	return n;
}

void alertas_reiniciar(void)
{
	pthread_mutex_lock(&lock_alertas);
	memset(historial, 0, sizeof(historial));
	total_alertas = 0;
	pthread_mutex_unlock(&lock_alertas);
}
