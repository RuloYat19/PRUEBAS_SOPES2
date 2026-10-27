#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

static struct muestra historial[MAX_MUESTRAS];
static unsigned long total_muestras;
static struct muestra anterior;
static int hay_anterior;
static pthread_mutex_t lock_metricas = PTHREAD_MUTEX_INITIALIZER;

static int activas = 1;
static int pendiente_muestra;
static pthread_mutex_t lock_espera = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_espera = PTHREAD_COND_INITIALIZER;

int metricas_leer_meminfo(const char *ruta, struct muestra *m)
{
	FILE *f;
	char linea[128];
	char clave[64];
	unsigned long valor;
	unsigned int mascara = 0;

	if (!ruta || !m)
		return -1;

	f = fopen(ruta, "r");
	if (!f)
		return -1;

	while (fgets(linea, sizeof(linea), f)) {
		if (sscanf(linea, "%63[^:]: %lu", clave, &valor) != 2)
			continue;
		if (!strcmp(clave, "MemTotal")) {
			m->mem_total = valor;
			mascara |= (1 << 0);
		} else if (!strcmp(clave, "MemFree")) {
			m->mem_libre = valor;
			mascara |= (1 << 1);
		} else if (!strcmp(clave, "MemAvailable")) {
			m->mem_disponible = valor;
			mascara |= (1 << 2);
		} else if (!strcmp(clave, "Buffers")) {
			m->buffers = valor;
			mascara |= (1 << 3);
		} else if (!strcmp(clave, "Cached")) {
			m->cached = valor;
			mascara |= (1 << 4);
		} else if (!strcmp(clave, "Active")) {
			m->activas = valor;
			mascara |= (1 << 5);
		} else if (!strcmp(clave, "Inactive")) {
			m->inactivas = valor;
			mascara |= (1 << 6);
		} else if (!strcmp(clave, "SwapTotal")) {
			m->swap_total = valor;
			mascara |= (1 << 7);
		} else if (!strcmp(clave, "SwapFree")) {
			m->swap_libre = valor;
			mascara |= (1 << 8);
		}
	}
	fclose(f);

	return (mascara == 0x1FF) ? 0 : -1;
}

int metricas_leer_vmstat(const char *ruta, struct muestra *m)
{
	FILE *f;
	char linea[128];
	char clave[64];
	unsigned long long valor;
	unsigned int mascara = 0;

	if (!ruta || !m)
		return -1;

	f = fopen(ruta, "r");
	if (!f)
		return -1;

	while (fgets(linea, sizeof(linea), f)) {
		if (sscanf(linea, "%63s %llu", clave, &valor) != 2)
			continue;
		if (!strcmp(clave, "pgfault")) {
			m->pgfault = valor;
			mascara |= (1 << 0);
		} else if (!strcmp(clave, "pgmajfault")) {
			m->pgmajfault = valor;
			mascara |= (1 << 1);
		}
	}
	fclose(f);

	return (mascara == 0x3) ? 0 : -1;
}

void metricas_calcular(struct muestra *m, const struct muestra *ant)
{
	if (!m)
		return;
	m->cache = m->buffers + m->cached;
	m->libre = m->mem_libre;
	m->usada = m->mem_total > m->libre + m->cache ? m->mem_total - m->libre - m->cache : 0;
	m->swap_usada = m->swap_total > m->swap_libre ? m->swap_total - m->swap_libre : 0;
	m->primera = ant == NULL ? 1 : 0;
	m->pgfault_delta    = ant && m->pgfault    >= ant->pgfault    ? m->pgfault    - ant->pgfault    : 0;
	m->pgmajfault_delta = ant && m->pgmajfault >= ant->pgmajfault ? m->pgmajfault - ant->pgmajfault : 0;
}

unsigned long metricas_tomar(const char *ruta_meminfo, const char *ruta_vmstat)
{
	struct muestra m;
	time_t ahora;
	struct tm tm_info;
	unsigned long id_asignado;

	memset(&m, 0, sizeof(m));
	if (metricas_leer_meminfo(ruta_meminfo, &m) != 0) {
		registrar("no se pudo leer meminfo: %s", ruta_meminfo);
		return 0;
	}
	if (metricas_leer_vmstat(ruta_vmstat, &m) != 0) {
		registrar("no se pudo leer vmstat: %s", ruta_vmstat);
		return 0;
	}

	ahora = time(NULL);
	localtime_r(&ahora, &tm_info);
	strftime(m.timestamp, sizeof(m.timestamp), "%Y-%m-%d %H:%M:%S", &tm_info);

	pthread_mutex_lock(&lock_metricas);
	if (!metricas_activas()) {
		pthread_mutex_unlock(&lock_metricas);
		return 0;
	}
	metricas_calcular(&m, hay_anterior ? &anterior : NULL);
	total_muestras++;
	m.id = total_muestras;
	historial[(total_muestras - 1) % MAX_MUESTRAS] = m;
	anterior = m;
	hay_anterior = 1;
	id_asignado = m.id;
	pthread_mutex_unlock(&lock_metricas);

	pthread_mutex_lock(&estado.lock);
	estado.memoria_usada = m.mem_total > m.mem_disponible ? m.mem_total - m.mem_disponible : 0;
	estado.memoria_libre = m.mem_disponible;
	estado.memoria_cache = m.cached + m.buffers;
	estado.swap_usada = m.swap_usada;
	pthread_mutex_unlock(&estado.lock);

	if (fd_aviso >= 0) {
		eventfd_t val = 1;
		eventfd_write(fd_aviso, val);
	}

	return id_asignado;
}

size_t metricas_desde(unsigned long ultimo_id, struct muestra *destino, size_t max)
{
	size_t n = 0;
	size_t disponibles;
	unsigned long primer_existente;
	unsigned long inicio_id;

	if (!destino || max == 0)
		return 0;

	pthread_mutex_lock(&lock_metricas);
	if (total_muestras <= ultimo_id) {
		pthread_mutex_unlock(&lock_metricas);
		return 0;
	}

	primer_existente = total_muestras > MAX_MUESTRAS ? total_muestras - MAX_MUESTRAS + 1 : 1;
	inicio_id = ultimo_id + 1;
	if (inicio_id < primer_existente)
		inicio_id = primer_existente;

	disponibles = total_muestras - inicio_id + 1;
	n = disponibles < max ? disponibles : max;
	for (size_t i = 0; i < n; i++) {
		unsigned long id = inicio_id + i;
		destino[i] = historial[(id - 1) % MAX_MUESTRAS];
	}
	pthread_mutex_unlock(&lock_metricas);

	return n;
}

size_t metricas_historial(struct muestra *destino, size_t max)
{
	size_t n = 0;
	size_t disponibles;
	unsigned long primer_id;

	if (!destino || max == 0)
		return 0;

	pthread_mutex_lock(&lock_metricas);
	if (total_muestras == 0) {
		pthread_mutex_unlock(&lock_metricas);
		return 0;
	}

	disponibles = total_muestras > MAX_MUESTRAS ? MAX_MUESTRAS : total_muestras;
	n = disponibles < max ? disponibles : max;
	primer_id = total_muestras - n + 1;
	for (size_t i = 0; i < n; i++) {
		unsigned long id = primer_id + i;
		destino[i] = historial[(id - 1) % MAX_MUESTRAS];
	}
	pthread_mutex_unlock(&lock_metricas);

	return n;
}

static int procesos_activos_val = 1;

void metricas_reiniciar(void)
{
	pthread_mutex_lock(&lock_metricas);
	total_muestras = 0;
	hay_anterior = 0;
	memset(historial, 0, sizeof(historial));
	memset(&anterior, 0, sizeof(anterior));
	pthread_mutex_unlock(&lock_metricas);
	__atomic_store_n(&activas, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&procesos_activos_val, 1, __ATOMIC_SEQ_CST);
	pthread_mutex_lock(&lock_espera);
	pendiente_muestra = 0;
	pthread_mutex_unlock(&lock_espera);
}

int metricas_activas(void)
{
	return __atomic_load_n(&activas, __ATOMIC_SEQ_CST);
}

int metricas_cambiar_estado(int activo)
{
	int deseado = activo ? 1 : 0;
	int previo = __atomic_exchange_n(&activas, deseado, __ATOMIC_SEQ_CST);

	if (previo == deseado)
		return 0;

	if (deseado) {
		pthread_mutex_lock(&lock_metricas);
		hay_anterior = 0;
		pthread_mutex_unlock(&lock_metricas);

		pthread_mutex_lock(&lock_espera);
		pendiente_muestra = 1;
		pthread_cond_signal(&cond_espera);
		pthread_mutex_unlock(&lock_espera);
	}
	return 1;
}

int procesos_activos(void)
{
	return __atomic_load_n(&procesos_activos_val, __ATOMIC_SEQ_CST);
}

int procesos_cambiar_estado(int activo)
{
	int deseado = activo ? 1 : 0;
	int previo = __atomic_exchange_n(&procesos_activos_val, deseado, __ATOMIC_SEQ_CST);

	if (previo == deseado)
		return 0;

	if (deseado) {
		pthread_mutex_lock(&lock_espera);
		pendiente_muestra = 1;
		pthread_cond_signal(&cond_espera);
		pthread_mutex_unlock(&lock_espera);
	}
	return 1;
}

void *hilo_monitoreo(void *intervalo_segundos)
{
	int intervalo = *(int *)intervalo_segundos;
	struct timespec limite;

	pthread_mutex_lock(&lock_espera);
	while (ejecutando) {
		pendiente_muestra = 0;
		pthread_mutex_unlock(&lock_espera);
		if (metricas_activas())
			metricas_tomar("/proc/meminfo", "/proc/vmstat");
		if (procesos_activos())
			procesos_tomar("/proc");
		pthread_mutex_lock(&lock_espera);

		if (!metricas_activas() && !procesos_activos()) {
			while (ejecutando && !pendiente_muestra)
				pthread_cond_wait(&cond_espera, &lock_espera);
		} else {
			clock_gettime(CLOCK_REALTIME, &limite);
			limite.tv_sec += intervalo;
			while (ejecutando && !pendiente_muestra &&
			       pthread_cond_timedwait(&cond_espera, &lock_espera, &limite) != ETIMEDOUT)
				;
		}
	}
	pthread_mutex_unlock(&lock_espera);
	return NULL;
}

void monitoreo_despertar(void)
{
	pthread_mutex_lock(&lock_espera);
	pthread_cond_signal(&cond_espera);
	pthread_mutex_unlock(&lock_espera);
}
