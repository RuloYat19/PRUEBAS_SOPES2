#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <math.h>
#include <assert.h>
#include "../comun.h"
#include "../lib/cJSON.h"

struct estado estado = { PTHREAD_MUTEX_INITIALIZER, 0, 0, 0, 0, {0, 0, 0, 0}, NULL, 0 };
volatile sig_atomic_t ejecutando = 1;
int fd_aviso = -1;

void registrar(const char *formato, ...)
{
	(void)formato;
}

static void crear_proceso(const char *raiz, long pid, const char *nombre, char estado_c,
			  long ppid, unsigned long flags, long tgid, unsigned long rss_kb,
			  int crear_comm, int crear_stat, int crear_status, int incluir_rss)
{
	char dir[PATH_MAX];
	char archivo[PATH_MAX + 64];
	FILE *f;

	snprintf(dir, sizeof(dir), "%s/%ld", raiz, pid);
	mkdir(dir, 0755);

	if (crear_comm) {
		snprintf(archivo, sizeof(archivo), "%s/comm", dir);
		f = fopen(archivo, "w");
		if (f) {
			fprintf(f, "%s\n", nombre);
			fclose(f);
		}
	}

	if (crear_stat) {
		snprintf(archivo, sizeof(archivo), "%s/stat", dir);
		f = fopen(archivo, "w");
		if (f) {
			fprintf(f, "%ld (%s) %c %ld 0 0 0 0 %lu 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
				pid, nombre, estado_c, ppid, flags);
			fclose(f);
		}
	}

	if (crear_status) {
		snprintf(archivo, sizeof(archivo), "%s/status", dir);
		f = fopen(archivo, "w");
		if (f) {
			fprintf(f, "Name:\t%s\nTgid:\t%ld\nPid:\t%ld\n", nombre, tgid, pid);
			if (incluir_rss)
				fprintf(f, "VmRSS:\t%lu kB\n", rss_kb);
			fclose(f);
		}
	}
}

static void borrar_recursivo(const char *ruta)
{
	DIR *d = opendir(ruta);
	struct dirent *e;
	char sub[PATH_MAX + 256];

	if (!d) {
		unlink(ruta);
		return;
	}

	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		snprintf(sub, sizeof(sub), "%s/%s", ruta, e->d_name);
		borrar_recursivo(sub);
	}
	closedir(d);
	rmdir(ruta);
}

int main(void)
{
	char plantilla[] = "/tmp/so2_test_proc_XXXXXX";
	char *temp_dir = mkdtemp(plantilla);
	struct proceso *lista = NULL;
	size_t n = 0;
	struct proceso top[TOP_PROCESOS];
	char nombre[TASK_COMM_LEN];
	size_t i, n_top;
	int encontrado_2 = 0, encontrado_600 = 0, encontrado_700 = 0;
	int idx_110 = -1, idx_115 = -1;
	double pct;
	long p;

	assert(temp_dir != NULL);

	crear_proceso(temp_dir, 1, "init", 'S', 0, 0, 1, 10000, 1, 1, 1, 1);
	crear_proceso(temp_dir, 2, "kthreadd", 'S', 0, PF_KTHREAD, 2, 0, 1, 1, 1, 0);
	crear_proceso(temp_dir, 50, "kworker/0:1", 'S', 2, PF_KTHREAD, 50, 0, 1, 1, 1, 0);
	crear_proceso(temp_dir, 500, "daemon", 'S', 1, 0, 500, 20000, 1, 1, 1, 1);
	crear_proceso(temp_dir, 501, "daemon", 'S', 1, 0, 500, 20000, 1, 1, 1, 1);
	crear_proceso(temp_dir, 600, "app", 'S', 1, 0, 600, 300000, 1, 1, 1, 1);
	crear_proceso(temp_dir, 601, "app_worker", 'S', 1, 0, 600, 300000, 1, 1, 1, 1);
	crear_proceso(temp_dir, 700, "incompleto", 'S', 1, 0, 700, 5000, 1, 0, 0, 0);
	crear_proceso(temp_dir, 701, "sin_stat", 'S', 1, 0, 701, 5000, 1, 0, 1, 1);
	crear_proceso(temp_dir, 702, "stat_corrupto", 'S', 1, 0, 702, 5000, 1, 0, 1, 1);
	{
		char archivo_corrupto[PATH_MAX + 64];
		FILE *fc;
		snprintf(archivo_corrupto, sizeof(archivo_corrupto), "%s/702/stat", temp_dir);
		fc = fopen(archivo_corrupto, "w");
		if (fc) {
			fprintf(fc, "invalido sin formato\n");
			fclose(fc);
		}
	}

	for (p = 100; p <= 124; p++) {
		char nom[32];
		unsigned long rss = (unsigned long)(p - 100) * 1000 + 1000;
		if (p == 110 || p == 115)
			rss = 50000;
		snprintf(nom, sizeof(nom), "proc_%ld", p);
		crear_proceso(temp_dir, p, nom, 'R', 1, 0, p, rss, 1, 1, 1, 1);
	}

	assert(procesos_escanear(temp_dir, &lista, &n) == 0);
	assert(n > 0);

	for (i = 0; i < n; i++) {
		if (lista[i].pid == 700)
			encontrado_700 = 1;
		if (lista[i].pid == 2) {
			encontrado_2 = 1;
			assert(lista[i].rss_kb == 0);
		}
		if (lista[i].pid == 600) {
			encontrado_600 = 1;
			assert(strcmp(lista[i].nombre, "app") == 0);
			assert(lista[i].estado == 'S');
			assert(lista[i].rss_kb == 300000);
		}
	}
	assert(!encontrado_700);
	assert(encontrado_2);
	assert(encontrado_600);
	printf("✓ Caso 1: procesos_escanear lee nombre, estado y RSS; omite el 700; PID 2 tiene RSS 0\n");

	n_top = procesos_top(lista, n, top, TOP_PROCESOS);
	assert(n_top == TOP_PROCESOS);
	for (i = 0; i < n_top - 1; i++) {
		if (top[i].rss_kb == top[i + 1].rss_kb)
			assert(top[i].pid < top[i + 1].pid);
		else
			assert(top[i].rss_kb > top[i + 1].rss_kb);

		if (top[i].pid == 110)
			idx_110 = (int)i;
		if (top[i].pid == 115)
			idx_115 = (int)i;
	}
	if (top[n_top - 1].pid == 110)
		idx_110 = (int)(n_top - 1);
	if (top[n_top - 1].pid == 115)
		idx_115 = (int)(n_top - 1);

	assert(idx_110 >= 0 && idx_115 >= 0);
	assert(idx_110 < idx_115);
	assert(top[0].pid == 600 || top[0].pid == 601);
	free(lista);
	printf("✓ Caso 2: procesos_top devuelve 20 ordenados por RSS desc y desempate por PID asc\n");

	pct = procesos_porcentaje(300000, 1000000);
	assert(fabs(pct - 30.0) < 0.001);
	assert(procesos_porcentaje(300000, 0) == 0.0);
	printf("✓ Caso 3: procesos_porcentaje calcula con precision y maneja mem_total 0\n");

	assert(procesos_validar_kill(temp_dir, 0, 500, nombre) == KILL_PID_INVALIDO);
	assert(procesos_validar_kill(temp_dir, 4194305, 500, nombre) == KILL_PID_INVALIDO);
	assert(procesos_validar_kill(temp_dir, 1, 500, nombre) == KILL_PID_UNO);
	assert(procesos_validar_kill(temp_dir, 500, 500, nombre) == KILL_DAEMON);
	assert(procesos_validar_kill(temp_dir, 501, 500, nombre) == KILL_DAEMON);
	assert(procesos_validar_kill(temp_dir, 2, 500, nombre) == KILL_KERNEL);
	assert(procesos_validar_kill(temp_dir, 50, 500, nombre) == KILL_KERNEL);
	assert(procesos_validar_kill(temp_dir, 601, 500, nombre) == KILL_NO_LIDER);
	assert(procesos_validar_kill(temp_dir, 999, 500, nombre) == KILL_NO_EXISTE);
	assert(procesos_validar_kill(temp_dir, 701, 500, nombre) == KILL_NO_EXISTE);
	assert(procesos_validar_kill(temp_dir, 702, 500, nombre) == KILL_KERNEL);

	memset(nombre, 0, sizeof(nombre));
	assert(procesos_validar_kill(temp_dir, 600, 500, nombre) == KILL_OK);
	assert(strcmp(nombre, "app") == 0);
	printf("✓ Caso 4: guardas de kill validan los 7 casos con orden y mensajes esperados\n");

	borrar_recursivo(temp_dir);

	assert(procesos_cambiar_estado(1) == 0);
	assert(procesos_activos() == 1);
	assert(procesos_cambiar_estado(0) == 1);
	assert(procesos_activos() == 0);
	assert(procesos_cambiar_estado(0) == 0);
	assert(procesos_activos() == 0);
	assert(procesos_cambiar_estado(1) == 1);
	assert(procesos_activos() == 1);
	printf("✓ Caso 5: procesos_cambiar_estado transiciones 1->0, 0->0, 0->1 y 1->1\n");

	{
		struct instantanea_procesos inst_prueba;
		struct buf b_json = { 0 };
		cJSON *parsed;
		cJSON *j_top;
		cJSON *j_pct;

		memset(&inst_prueba, 0, sizeof(inst_prueba));
		inst_prueba.valida = 1;
		snprintf(inst_prueba.timestamp, sizeof(inst_prueba.timestamp), "2026-10-18 17:00:00");
		inst_prueba.resumen.total = 100;
		inst_prueba.resumen.running = 2;
		inst_prueba.resumen.sleeping = 98;
		inst_prueba.n_top = 2;
		inst_prueba.mem_total_kb = 1000000;
		inst_prueba.top[0].pid = 10;
		snprintf(inst_prueba.top[0].nombre, sizeof(inst_prueba.top[0].nombre), "p1");
		inst_prueba.top[0].estado = 'R';
		inst_prueba.top[0].rss_kb = 100000;
		inst_prueba.top[1].pid = 20;
		snprintf(inst_prueba.top[1].nombre, sizeof(inst_prueba.top[1].nombre), "p2");
		inst_prueba.top[1].estado = 'S';
		inst_prueba.top[1].rss_kb = 50000;

		json_procesos(&b_json, &inst_prueba);
		assert(b_json.largo > 0 && !b_json.fallo);

		parsed = cJSON_Parse(b_json.datos);
		assert(parsed != NULL);
		j_top = cJSON_GetObjectItemCaseSensitive(parsed, "top");
		assert(cJSON_IsArray(j_top));
		assert(cJSON_GetArraySize(j_top) == 2);
		j_pct = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j_top, 0), "porcentaje_memoria");
		assert(cJSON_IsNumber(j_pct));
		assert(fabs(j_pct->valuedouble - 10.0) < 0.01);
		cJSON_Delete(parsed);
		buf_liberar(&b_json);
		printf("✓ Caso 6: json_procesos se parsea con cJSON y porcentaje_memoria es numerico\n");
	}

	printf("Todas las pruebas de procesos (Casos 1-6) pasaron exitosamente.\n");
	return 0;
}
