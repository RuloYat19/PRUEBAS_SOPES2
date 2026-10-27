#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../comun.h"
#include "../lib/cJSON.h"

int fd_aviso = -1;
volatile sig_atomic_t ejecutando = 1;
struct estado estado = { .lock = PTHREAD_MUTEX_INITIALIZER };

void registrar(const char *formato, ...)
{
	(void)formato;
}

struct motivo_esperado {
	enum motivo_alerta motivo;
	const char *texto;
	const char *tipo;
	const char *severidad;
};

static const struct motivo_esperado motivos_esperados[] = {
	{ MOT_ARCHIVO_MODIFICADO, "Archivo modificado", "archivos", "LOW" },
	{ MOT_MONITOREO_ACTIVADO, "Monitoreo del sistema activado", "sistema", "LOW" },
	{ MOT_SESION_EXITOSA, "Inicio de sesión exitoso", "sesion", "LOW" },
	{ MOT_MONITOREO_DESACTIVADO, "Monitoreo del sistema desactivado", "sistema", "MEDIUM" },
	{ MOT_SESION_FALLIDA, "Inicio de sesión fallido", "sesion", "MEDIUM" },
	{ MOT_PALABRA_EN_LOG, "Palabra clave detectada en log", "logs", "MEDIUM" },
	{ MOT_BLACKLIST_ACTUALIZADA, "Actualización base de datos local", "archivos", "MEDIUM" },
	{ MOT_PROCESO_MATADO, "Proceso matado", "sistema", "MEDIUM" },
	{ MOT_HASH_MALICIOSO, "Hash malicioso detectado", "archivos", "HIGH" },
	{ MOT_ARCHIVO_CUARENTENA, "Archivo puesto en cuarentena", "archivos", "HIGH" },
};

static void test_catalogo(void)
{
	for (size_t i = 0; i < sizeof(motivos_esperados) / sizeof(motivos_esperados[0]); i++) {
		enum motivo_alerta m = motivos_esperados[i].motivo;
		assert(strcmp(alerta_motivo_texto(m), motivos_esperados[i].texto) == 0);
		assert(strcmp(alerta_tipo_texto(alerta_tipo(m)), motivos_esperados[i].tipo) == 0);
		assert(strcmp(alerta_severidad_texto(alerta_severidad(m)), motivos_esperados[i].severidad) == 0);
	}
	printf("\033[0;32m✓ Caso 1: Catalogo de 10 motivos con tipos y severidades exactos\033[0m\n");
}

static void test_motivo_invalido(void)
{
	struct alerta destino[10];

	alertas_reiniciar();
	assert(alerta_emitir(MOT_TOTAL, "invalido") == 0);
	assert(alerta_emitir((enum motivo_alerta)-1, "negativo") == 0);
	assert(alertas_recientes(destino, 10) == 0);

	printf("\033[0;32m✓ Caso 2: Motivo invalido no emite y no altera el historial\033[0m\n");
}

static void test_tres_emisiones(void)
{
	alertas_reiniciar();
	assert(alerta_emitir(MOT_MONITOREO_ACTIVADO, "uno") == 1);
	assert(alerta_emitir(MOT_MONITOREO_ACTIVADO, "dos") == 2);
	assert(alerta_emitir(MOT_MONITOREO_ACTIVADO, "tres") == 3);

	printf("\033[0;32m✓ Caso 3: Tres emisiones devuelven ids 1, 2 y 3\033[0m\n");
}

static void test_orden_recientes_y_desde(void)
{
	struct alerta rec[10];
	struct alerta desde[10];
	size_t n_rec;
	size_t n_desde;

	n_rec = alertas_recientes(rec, 10);
	assert(n_rec == 3);
	assert(rec[0].id == 3);
	assert(rec[1].id == 2);
	assert(rec[2].id == 1);

	n_desde = alertas_desde(1, desde, 10);
	assert(n_desde == 2);
	assert(desde[0].id == 2);
	assert(desde[1].id == 3);

	printf("\033[0;32m✓ Caso 4: alertas_recientes devuelve orden descendente y alertas_desde orden ascendente\033[0m\n");
}

static void test_capacidad_circular(void)
{
	struct alerta rec200[250];
	struct alerta desde0[250];
	size_t n200;
	size_t nd0;

	alertas_reiniciar();
	for (int i = 0; i < 205; i++) {
		unsigned long id = alerta_emitir(MOT_MONITOREO_ACTIVADO, "alerta %d", i);
		assert(id == (unsigned long)(i + 1));
	}

	n200 = alertas_recientes(rec200, 250);
	assert(n200 == 200);
	assert(rec200[0].id == 205);
	assert(rec200[199].id == 6);

	nd0 = alertas_desde(0, desde0, 250);
	assert(nd0 == 200);
	assert(desde0[0].id == 6);
	assert(desde0[199].id == 205);

	printf("\033[0;32m✓ Caso 5: Capacidad de 200 descarta antiguos al llegar a 205 (ventana 6 a 205)\033[0m\n");
}

static void test_descripcion_truncada(void)
{
	char larga[1001];
	struct alerta r[1];
	unsigned long id;

	memset(larga, 'a', 1000);
	larga[1000] = '\0';
	alertas_reiniciar();
	id = alerta_emitir(MOT_MONITOREO_ACTIVADO, "%s", larga);
	assert(id == 1);
	assert(alertas_recientes(r, 1) == 1);
	assert(strlen(r[0].descripcion) == 255);
	assert(r[0].descripcion[254] == 'a');
	assert(r[0].descripcion[255] == '\0');

	printf("\033[0;32m✓ Caso 6: Descripcion truncada a 255 bytes con terminador nulo\033[0m\n");
}

static void test_emision_log(void)
{
	struct alerta r[1];
	unsigned long id;

	alertas_reiniciar();
	id = alerta_emitir_log("ERROR", "/x/a.log", "linea \"con\" comillas", 0);
	assert(id == 1);
	assert(alertas_recientes(r, 1) == 1);
	assert(r[0].tipo == TIPO_LOGS);
	assert(r[0].severidad == SEV_MEDIUM);
	assert(strcmp(r[0].descripcion, "'ERROR' en /x/a.log: linea \"con\" comillas") == 0);
	assert(strcmp(r[0].palabra, "ERROR") == 0);
	assert(strcmp(r[0].archivo, "/x/a.log") == 0);
	assert(strcmp(r[0].contenido, "linea \"con\" comillas") == 0);
	assert(strlen(r[0].timestamp) == 19);

	printf("\033[0;32m✓ Caso 7: alerta_emitir_log genera tipo logs, severidad MEDIUM y campos especificos\033[0m\n");
}

static void test_caso_8(void)
{
	struct alerta a_log;
	struct alerta a_sesion;
	struct buf b = { 0 };
	cJSON *root;
	cJSON *item;

	alertas_reiniciar();
	alerta_emitir_log("ERROR", "/x/a.log", "linea \"con\" comillas", 0);
	assert(alertas_recientes(&a_log, 1) == 1);

	json_alerta(&b, &a_log);
	assert(b.largo > 0 && !b.fallo);

	root = cJSON_Parse(b.datos);
	assert(root != NULL);

	item = cJSON_GetObjectItem(root, "id");
	assert(item && item->type == cJSON_Number && item->valuedouble == 1);

	item = cJSON_GetObjectItem(root, "tipo");
	assert(item && item->type == cJSON_String && strcmp(item->valuestring, "logs") == 0);

	item = cJSON_GetObjectItem(root, "motivo");
	assert(item && item->type == cJSON_String && strcmp(item->valuestring, "Palabra clave detectada en log") == 0);

	item = cJSON_GetObjectItem(root, "descripcion");
	assert(item && item->type == cJSON_String && strcmp(item->valuestring, "'ERROR' en /x/a.log: linea \"con\" comillas") == 0);

	item = cJSON_GetObjectItem(root, "severidad");
	assert(item && item->type == cJSON_String && strcmp(item->valuestring, "MEDIUM") == 0);

	item = cJSON_GetObjectItem(root, "timestamp");
	assert(item && item->type == cJSON_String && strlen(item->valuestring) == 19);

	item = cJSON_GetObjectItem(root, "archivo_origen");
	assert(item && item->type == cJSON_String && strcmp(item->valuestring, "/x/a.log") == 0);

	item = cJSON_GetObjectItem(root, "palabra_detectada");
	assert(item && item->type == cJSON_String && strcmp(item->valuestring, "ERROR") == 0);

	item = cJSON_GetObjectItem(root, "contenido");
	assert(item && item->type == cJSON_String && strcmp(item->valuestring, "linea \"con\" comillas") == 0);

	cJSON_Delete(root);
	buf_liberar(&b);

	alerta_emitir(MOT_SESION_EXITOSA, "Usuario ana");
	assert(alertas_recientes(&a_sesion, 1) == 1);

	json_alerta(&b, &a_sesion);
	assert(b.largo > 0 && !b.fallo);

	root = cJSON_Parse(b.datos);
	assert(root != NULL);
	assert(cJSON_GetObjectItem(root, "archivo_origen") == NULL);
	assert(cJSON_GetObjectItem(root, "palabra_detectada") == NULL);
	assert(cJSON_GetObjectItem(root, "contenido") == NULL);
	assert(cJSON_GetObjectItem(root, "id") != NULL);
	assert(cJSON_GetObjectItem(root, "tipo") != NULL);
	assert(cJSON_GetObjectItem(root, "motivo") != NULL);
	assert(cJSON_GetObjectItem(root, "descripcion") != NULL);
	assert(cJSON_GetObjectItem(root, "severidad") != NULL);
	assert(cJSON_GetObjectItem(root, "timestamp") != NULL);

	cJSON_Delete(root);
	buf_liberar(&b);

	printf("\033[0;32m✓ Caso 8: json_alerta serializa campos exactos y parsea con cJSON\033[0m\n");
}

static void *hilo_trabajador(void *arg)
{
	(void)arg;
	for (int i = 0; i < 100; i++) {
		alerta_emitir(MOT_MONITOREO_ACTIVADO, "hilo iter %d", i);
	}
	return NULL;
}

static void test_concurrencia(void)
{
	pthread_t hilos[4];
	struct alerta r[250];
	size_t n;

	alertas_reiniciar();
	for (int i = 0; i < 4; i++) {
		assert(pthread_create(&hilos[i], NULL, hilo_trabajador, NULL) == 0);
	}
	for (int i = 0; i < 4; i++) {
		assert(pthread_join(hilos[i], NULL) == 0);
	}

	n = alertas_recientes(r, 250);
	assert(n == 200);
	for (size_t i = 0; i < 200; i++) {
		assert(r[i].id == 400 - i);
	}

	printf("\033[0;32m✓ Caso 9: Concurrencia de 4 hilos x 100 genera 400 alertas con ultimos 200 ids unicos\033[0m\n");
}

static int es_utf8_valido(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p < 0x80) {
			p++;
		} else if ((*p & 0xE0) == 0xC0) {
			if ((p[1] & 0xC0) != 0x80)
				return 0;
			p += 2;
		} else if ((*p & 0xF0) == 0xE0) {
			if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
				return 0;
			p += 3;
		} else if ((*p & 0xF8) == 0xF0) {
			if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
				return 0;
			p += 4;
		} else {
			return 0;
		}
	}
	return 1;
}

static void test_descripcion_truncada_utf8(void)
{
	char linea[600];
	struct alerta r[1];
	unsigned long id;
	struct buf b;
	cJSON *root;

	memset(linea, 'x', 254);
	snprintf(linea + 254, sizeof(linea) - 254, "ñandú acción rápido crítico");
	alertas_reiniciar();
	id = alerta_emitir(MOT_MONITOREO_ACTIVADO, "%s", linea);
	assert(id == 1);
	assert(alertas_recientes(r, 1) == 1);
	assert(strlen(r[0].descripcion) == 254);
	assert(r[0].descripcion[253] == 'x');
	assert(r[0].descripcion[254] == '\0');
	assert(es_utf8_valido(r[0].descripcion));

	memset(&b, 0, sizeof(b));
	json_alerta(&b, &r[0]);
	assert(b.largo > 0 && !b.fallo);
	root = cJSON_Parse(b.datos);
	assert(root != NULL);
	cJSON_Delete(root);
	buf_liberar(&b);

	snprintf(linea, sizeof(linea),
		 "atención, autenticación, notificación y configuración crítica del sistema en ejecución continua: ");
	while (strlen(linea) < 500) {
		strcat(linea, "acción rápida y verificación de módulos con éxito total; ");
	}
	id = alerta_emitir_log("ATENCION", "/var/log/seguridad.log", linea, 0);
	assert(id == 2);
	assert(alertas_recientes(r, 1) == 1);
	assert(es_utf8_valido(r[0].descripcion));
	assert(es_utf8_valido(r[0].contenido));

	memset(&b, 0, sizeof(b));
	json_alerta(&b, &r[0]);
	assert(b.largo > 0 && !b.fallo);
	root = cJSON_Parse(b.datos);
	assert(root != NULL);
	cJSON_Delete(root);
	buf_liberar(&b);

	printf("\033[0;32m✓ Caso 10: Descripcion truncada preserva caracteres UTF-8 sin bytes huerfanos\033[0m\n");
}

int main(void)
{
	test_catalogo();
	test_motivo_invalido();
	test_tres_emisiones();
	test_orden_recientes_y_desde();
	test_capacidad_circular();
	test_descripcion_truncada();
	test_emision_log();
	test_caso_8();
	test_concurrencia();
	test_descripcion_truncada_utf8();
	return 0;
}
