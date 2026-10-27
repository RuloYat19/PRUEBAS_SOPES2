#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include "comun.h"

static struct instantanea_procesos instantanea;
static pthread_mutex_t lock_procesos = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t hay_procesos_nuevos = 0;

static int leer_proceso_raiz(const char *raiz, const char *pid_str, struct proceso *p)
{
	char ruta[PATH_MAX];
	char stat_buf[1024];
	char linea[256];
	char *cierre;
	size_t leidos;
	FILE *f;

	snprintf(ruta, sizeof(ruta), "%s/%s/comm", raiz, pid_str);
	f = fopen(ruta, "r");
	if (!f)
		return -1;
	if (!fgets(p->nombre, sizeof(p->nombre), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	p->nombre[strcspn(p->nombre, "\n")] = '\0';

	snprintf(ruta, sizeof(ruta), "%s/%s/stat", raiz, pid_str);
	f = fopen(ruta, "r");
	if (!f)
		return -1;
	leidos = fread(stat_buf, 1, sizeof(stat_buf) - 1, f);
	fclose(f);
	stat_buf[leidos] = '\0';

	cierre = strrchr(stat_buf, ')');
	if (!cierre || cierre[1] != ' ' || !cierre[2])
		return -1;

	p->pid = (pid_t)atol(pid_str);
	p->estado = cierre[2];
	p->rss_kb = 0;

	snprintf(ruta, sizeof(ruta), "%s/%s/status", raiz, pid_str);
	f = fopen(ruta, "r");
	if (f) {
		while (fgets(linea, sizeof(linea), f)) {
			if (strncmp(linea, "VmRSS:", 6) == 0) {
				unsigned long v = 0;
				if (sscanf(linea + 6, "%lu", &v) == 1)
					p->rss_kb = v;
				break;
			}
		}
		fclose(f);
	}

	return 0;
}

int procesos_escanear(const char *raiz, struct proceso **lista, size_t *cantidad)
{
	struct proceso *procesos = NULL;
	struct proceso *ampliada;
	struct dirent *entrada;
	size_t n = 0, capacidad = 0;
	DIR *proc;

	proc = opendir(raiz);
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
		if (leer_proceso_raiz(raiz, entrada->d_name, &procesos[n]) == 0)
			n++;
	}
	closedir(proc);

	*lista = procesos;
	*cantidad = n;
	return 0;
}

static int comparar_procesos(const void *a, const void *b)
{
	const struct proceso *pa = (const struct proceso *)a;
	const struct proceso *pb = (const struct proceso *)b;

	if (pa->rss_kb > pb->rss_kb)
		return -1;
	if (pa->rss_kb < pb->rss_kb)
		return 1;
	if (pa->pid < pb->pid)
		return -1;
	if (pa->pid > pb->pid)
		return 1;
	return 0;
}

size_t procesos_top(const struct proceso *lista, size_t n, struct proceso *top, size_t max)
{
	struct proceso *copia;
	size_t a_copiar;

	if (!lista || n == 0 || !top || max == 0)
		return 0;

	copia = malloc(n * sizeof(*copia));
	if (!copia)
		return 0;

	memcpy(copia, lista, n * sizeof(*copia));
	qsort(copia, n, sizeof(struct proceso), comparar_procesos);

	a_copiar = n < max ? n : max;
	memcpy(top, copia, a_copiar * sizeof(*top));
	free(copia);
	return a_copiar;
}

double procesos_porcentaje(unsigned long rss_kb, unsigned long mem_total_kb)
{
	if (!mem_total_kb)
		return 0.0;
	return ((double)rss_kb * 100.0) / (double)mem_total_kb;
}

enum resultado_kill procesos_validar_kill(const char *raiz, long pid, pid_t pid_daemon,
					  char nombre[TASK_COMM_LEN])
{
	char ruta[PATH_MAX];
	char linea[256];
	char stat_buf[1024];
	char *cierre;
	long tgid = -1;
	FILE *f;
	size_t leidos;

	if (pid < 1 || pid > PID_MAXIMO)
		return KILL_PID_INVALIDO;

	if (pid == 1)
		return KILL_PID_UNO;

	snprintf(ruta, sizeof(ruta), "%s/%ld/status", raiz, pid);
	f = fopen(ruta, "r");
	if (!f)
		return KILL_NO_EXISTE;

	while (fgets(linea, sizeof(linea), f)) {
		if (strncmp(linea, "Tgid:", 5) == 0) {
			sscanf(linea + 5, "%ld", &tgid);
			break;
		}
	}
	fclose(f);

	if (pid == (long)pid_daemon || tgid == (long)pid_daemon)
		return KILL_DAEMON;

	if (pid == 2)
		return KILL_KERNEL;

	snprintf(ruta, sizeof(ruta), "%s/%ld/stat", raiz, pid);
	f = fopen(ruta, "r");
	if (!f)
		return KILL_NO_EXISTE;

	leidos = fread(stat_buf, 1, sizeof(stat_buf) - 1, f);
	fclose(f);
	if (leidos == 0)
		return KILL_KERNEL;

	stat_buf[leidos] = '\0';
	cierre = strrchr(stat_buf, ')');
	if (!cierre)
		return KILL_KERNEL;

	{
		char estado_c;
		long ppid, pgrp, session, tty_nr, tpgid;
		unsigned long flags = 0;

		if (sscanf(cierre + 1, " %c %ld %ld %ld %ld %ld %lu",
			   &estado_c, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags) != 7)
			return KILL_KERNEL;

		if (flags & PF_KTHREAD)
			return KILL_KERNEL;
	}

	if (tgid != pid)
		return KILL_NO_LIDER;

	if (nombre) {
		nombre[0] = '\0';
		snprintf(ruta, sizeof(ruta), "%s/%ld/comm", raiz, pid);
		f = fopen(ruta, "r");
		if (f) {
			if (fgets(nombre, TASK_COMM_LEN, f))
				nombre[strcspn(nombre, "\n")] = '\0';
			fclose(f);
		}
	}

	return KILL_OK;
}

void procesos_instantanea(struct instantanea_procesos *destino)
{
	if (!destino)
		return;
	pthread_mutex_lock(&lock_procesos);
	*destino = instantanea;
	pthread_mutex_unlock(&lock_procesos);
}

void procesos_tomar(const char *raiz)
{
	struct processes_state resumen;
	struct proceso *lista = NULL;
	struct proceso *anteriores = NULL;
	struct proceso top_local[TOP_PROCESOS];
	struct sysinfo si;
	size_t n = 0, n_top = 0;
	time_t ahora;
	struct tm tm_info;
	int hay_resumen;

	memset(&resumen, 0, sizeof(resumen));
	hay_resumen = syscall(SYS_GET_PROCESSES_STATE, &resumen) == 0;
	if (!hay_resumen)
		registrar("sys_get_processes_state fallo: %s", strerror(errno));

	if (procesos_escanear(raiz, &lista, &n) != 0) {
		registrar("no se pudo escanear %s: %s", raiz, strerror(errno));
		return;
	}

	memset(top_local, 0, sizeof(top_local));
	n_top = procesos_top(lista, n, top_local, TOP_PROCESOS);

	if (sysinfo(&si) != 0)
		memset(&si, 0, sizeof(si));

	if (!procesos_activos()) {
		free(lista);
		return;
	}

	ahora = time(NULL);
	localtime_r(&ahora, &tm_info);

	pthread_mutex_lock(&lock_procesos);
	instantanea.valida = 1;
	strftime(instantanea.timestamp, sizeof(instantanea.timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);
	if (hay_resumen)
		instantanea.resumen = resumen;
	memset(instantanea.top, 0, sizeof(instantanea.top));
	memcpy(instantanea.top, top_local, n_top * sizeof(struct proceso));
	instantanea.n_top = n_top;
	instantanea.mem_total_kb = ((unsigned long)si.totalram * (unsigned long)si.mem_unit) / 1024;
	pthread_mutex_unlock(&lock_procesos);

	pthread_mutex_lock(&estado.lock);
	if (hay_resumen)
		estado.resumen = resumen;
	anteriores = estado.procesos;
	estado.procesos = lista;
	estado.n_procesos = n;
	pthread_mutex_unlock(&estado.lock);

	free(anteriores);

	__atomic_store_n(&hay_procesos_nuevos, 1, __ATOMIC_SEQ_CST);

	if (fd_aviso >= 0) {
		eventfd_t val = 1;
		eventfd_write(fd_aviso, val);
	}
}
