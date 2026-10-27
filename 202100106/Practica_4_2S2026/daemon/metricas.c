#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

struct memoria {
	unsigned long usada;
	unsigned long libre;
	unsigned long cache;
	unsigned long swap;
};

static pthread_mutex_t lock_espera = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_espera = PTHREAD_COND_INITIALIZER;

static int leer_meminfo(struct memoria *m)
{
	unsigned long total = 0, disponible = 0, cached = 0, buffers = 0;
	unsigned long swap_total = 0, swap_libre = 0, valor;
	char linea[128];
	char clave[64];
	FILE *f;

	f = fopen("/proc/meminfo", "r");
	if (!f)
		return -1;

	while (fgets(linea, sizeof(linea), f)) {
		if (sscanf(linea, "%63[^:]: %lu", clave, &valor) != 2)
			continue;
		if (!strcmp(clave, "MemTotal"))
			total = valor;
		else if (!strcmp(clave, "MemAvailable"))
			disponible = valor;
		else if (!strcmp(clave, "Cached"))
			cached = valor;
		else if (!strcmp(clave, "Buffers"))
			buffers = valor;
		else if (!strcmp(clave, "SwapTotal"))
			swap_total = valor;
		else if (!strcmp(clave, "SwapFree"))
			swap_libre = valor;
	}
	fclose(f);

	m->usada = total - disponible;
	m->libre = disponible;
	m->cache = cached + buffers;
	m->swap = swap_total - swap_libre;
	return 0;
}

static int leer_proceso(const char *pid, struct proceso *p)
{
	char ruta[288];
	char stat[512];
	char *cierre;
	size_t leidos;
	FILE *f;

	snprintf(ruta, sizeof(ruta), "/proc/%s/comm", pid);
	f = fopen(ruta, "r");
	if (!f)
		return -1;
	if (!fgets(p->nombre, sizeof(p->nombre), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	p->nombre[strcspn(p->nombre, "\n")] = '\0';

	snprintf(ruta, sizeof(ruta), "/proc/%s/stat", pid);
	f = fopen(ruta, "r");
	if (!f)
		return -1;
	leidos = fread(stat, 1, sizeof(stat) - 1, f);
	fclose(f);
	stat[leidos] = '\0';

	cierre = strrchr(stat, ')');
	if (!cierre || cierre[1] != ' ' || !cierre[2])
		return -1;

	p->pid = (pid_t)atoi(pid);
	p->estado = cierre[2];
	return 0;
}

static int escanear_procesos(struct proceso **lista, size_t *cantidad)
{
	struct proceso *procesos = NULL;
	struct proceso *ampliada;
	struct dirent *entrada;
	size_t n = 0, capacidad = 0;
	DIR *proc;

	proc = opendir("/proc");
	if (!proc)
		return -1;

	while ((entrada = readdir(proc)) != NULL) {
		if (!isdigit((unsigned char)entrada->d_name[0]))
			continue;
		if (n == capacidad) {
			capacidad = capacidad ? capacidad * 2 : 256;
			ampliada = realloc(procesos, capacidad * sizeof(*procesos));
			if (!ampliada) {
				free(procesos);
				closedir(proc);
				return -1;
			}
			procesos = ampliada;
		}
		if (leer_proceso(entrada->d_name, &procesos[n]) == 0)
			n++;
	}
	closedir(proc);

	*lista = procesos;
	*cantidad = n;
	return 0;
}

static void recolectar(void)
{
	struct processes_state resumen;
	struct proceso *procesos = NULL;
	struct proceso *anteriores = NULL;
	struct memoria memoria;
	size_t n_procesos = 0;
	int hay_resumen, hay_memoria, hay_procesos;

	memset(&resumen, 0, sizeof(resumen));
	hay_resumen = syscall(SYS_GET_PROCESSES_STATE, &resumen) == 0;
	if (!hay_resumen)
		registrar("sys_get_processes_state fallo: %s", strerror(errno));

	hay_memoria = leer_meminfo(&memoria) == 0;
	if (!hay_memoria)
		registrar("no se pudo leer /proc/meminfo: %s", strerror(errno));

	hay_procesos = escanear_procesos(&procesos, &n_procesos) == 0;
	if (!hay_procesos)
		registrar("no se pudo escanear /proc: %s", strerror(errno));

	pthread_mutex_lock(&estado.lock);
	if (hay_resumen)
		estado.resumen = resumen;
	if (hay_memoria) {
		estado.memoria_usada = memoria.usada;
		estado.memoria_libre = memoria.libre;
		estado.memoria_cache = memoria.cache;
		estado.swap_usada = memoria.swap;
	}
	if (hay_procesos) {
		anteriores = estado.procesos;
		estado.procesos = procesos;
		estado.n_procesos = n_procesos;
	}
	pthread_mutex_unlock(&estado.lock);

	free(anteriores);
}

void *hilo_metricas(void *intervalo_segundos)
{
	int intervalo = *(int *)intervalo_segundos;
	struct timespec limite;

	pthread_mutex_lock(&lock_espera);
	while (ejecutando) {
		pthread_mutex_unlock(&lock_espera);
		recolectar();
		pthread_mutex_lock(&lock_espera);

		clock_gettime(CLOCK_REALTIME, &limite);
		limite.tv_sec += intervalo;
		while (ejecutando &&
		       pthread_cond_timedwait(&cond_espera, &lock_espera, &limite) == 0)
			;
	}
	pthread_mutex_unlock(&lock_espera);
	return NULL;
}

void metricas_despertar(void)
{
	pthread_mutex_lock(&lock_espera);
	pthread_cond_signal(&cond_espera);
	pthread_mutex_unlock(&lock_espera);
}
