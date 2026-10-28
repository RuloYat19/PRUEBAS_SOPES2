#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "comun.h"
#include "ebpf/sensor.skel.h"

static pthread_mutex_t lock_sensor = PTHREAD_MUTEX_INITIALIZER;
static struct sensor_bpf *esqueleto = NULL;
static struct ring_buffer *consumidor = NULL;
static char ruta_actual[SENSOR_RUTA_MAX] = "";
static int ruta_valida = 0;
static char error_sensor[128] = "";
static int fd_control = -1;

static struct evento_archivo lote[LOTE_EVENTOS];
static size_t n_lote = 0;

static int imprimir_libbpf(enum libbpf_print_level level, const char *fmt, va_list args)
{
	char buf[512];
	size_t len;

	if (level != LIBBPF_WARN)
		return 0;

	vsnprintf(buf, sizeof(buf), fmt, args);
	len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	if (len > 0)
		registrar("libbpf: %s", buf);

	return 0;
}

static int recibir_evento(void *ctx, void *data, size_t size)
{
	(void)ctx;
	if (size == sizeof(struct evento_archivo) && n_lote < LOTE_EVENTOS) {
		memcpy(&lote[n_lote], data, sizeof(struct evento_archivo));
		n_lote++;
	}
	return 0;
}

int sensor_iniciar(const char *ruta_configurada)
{
	char canonica[SENSOR_RUTA_MAX];
	int r;

	fd_control = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	libbpf_set_print(imprimir_libbpf);

	r = analisis_validar_ruta(ruta_configurada, canonica, sizeof(canonica));
	pthread_mutex_lock(&lock_sensor);
	if (r == RUTA_OK) {
		ruta_valida = 1;
		snprintf(ruta_actual, sizeof(ruta_actual), "%s", canonica);
		error_sensor[0] = '\0';
	} else {
		ruta_valida = 0;
		if (ruta_configurada)
			snprintf(ruta_actual, sizeof(ruta_actual), "%s", ruta_configurada);
		else
			ruta_actual[0] = '\0';
		snprintf(error_sensor, sizeof(error_sensor), "La ruta vigilada no es un directorio existente");
	}
	analisis_publicar_sensor(0, ruta_actual, error_sensor);
	pthread_mutex_unlock(&lock_sensor);

	return ruta_valida ? 0 : -1;
}

int sensor_activo(void)
{
	int activo;

	pthread_mutex_lock(&lock_sensor);
	activo = (esqueleto != NULL);
	pthread_mutex_unlock(&lock_sensor);

	return activo;
}

int sensor_activar(void)
{
	__u32 cero = 0;
	__u32 pid;
	int fd_pid;
	int fd_cfg;
	struct config_sensor cfg;

	pthread_mutex_lock(&lock_sensor);
	if (esqueleto) {
		pthread_mutex_unlock(&lock_sensor);
		return 0;
	}

	if (!ruta_valida) {
		snprintf(error_sensor, sizeof(error_sensor), "La ruta vigilada no es un directorio existente");
		analisis_publicar_sensor(0, ruta_actual, error_sensor);
		pthread_mutex_unlock(&lock_sensor);
		return -1;
	}

	esqueleto = sensor_bpf__open_and_load();
	if (!esqueleto) {
		snprintf(error_sensor, sizeof(error_sensor), "No se pudo cargar el sensor: %s", strerror(errno));
		analisis_publicar_sensor(0, ruta_actual, error_sensor);
		pthread_mutex_unlock(&lock_sensor);
		return -1;
	}

	pid = (__u32)getpid();
	fd_pid = bpf_map__fd(esqueleto->maps.pid_daemon);
	fd_cfg = bpf_map__fd(esqueleto->maps.config_sensor);

	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.ruta, sizeof(cfg.ruta), "%.200s/", ruta_actual);
	cfg.largo = (unsigned int)strlen(cfg.ruta);

	if (bpf_map_update_elem(fd_pid, &cero, &pid, BPF_ANY) < 0 ||
	    bpf_map_update_elem(fd_cfg, &cero, &cfg, BPF_ANY) < 0) {
		snprintf(error_sensor, sizeof(error_sensor), "No se pudo configurar el sensor: %s", strerror(errno));
		sensor_bpf__destroy(esqueleto);
		esqueleto = NULL;
		analisis_publicar_sensor(0, ruta_actual, error_sensor);
		pthread_mutex_unlock(&lock_sensor);
		return -1;
	}

	if (sensor_bpf__attach(esqueleto) < 0) {
		snprintf(error_sensor, sizeof(error_sensor), "No se pudo adjuntar el sensor: %s", strerror(errno));
		sensor_bpf__destroy(esqueleto);
		esqueleto = NULL;
		analisis_publicar_sensor(0, ruta_actual, error_sensor);
		pthread_mutex_unlock(&lock_sensor);
		return -1;
	}

	consumidor = ring_buffer__new(bpf_map__fd(esqueleto->maps.eventos), recibir_evento, NULL, NULL);
	if (!consumidor) {
		snprintf(error_sensor, sizeof(error_sensor), "No se pudo crear el ring buffer: %s", strerror(errno));
		sensor_bpf__destroy(esqueleto);
		esqueleto = NULL;
		analisis_publicar_sensor(0, ruta_actual, error_sensor);
		pthread_mutex_unlock(&lock_sensor);
		return -1;
	}

	error_sensor[0] = '\0';
	analisis_reiniciar();
	analisis_publicar_sensor(1, ruta_actual, error_sensor);
	pthread_mutex_unlock(&lock_sensor);

	if (fd_control >= 0)
		eventfd_write(fd_control, 1);

	return 1;
}

int sensor_desactivar(void)
{
	pthread_mutex_lock(&lock_sensor);
	if (!esqueleto) {
		pthread_mutex_unlock(&lock_sensor);
		return 0;
	}

	if (consumidor) {
		ring_buffer__free(consumidor);
		consumidor = NULL;
	}

	sensor_bpf__destroy(esqueleto);
	esqueleto = NULL;
	analisis_publicar_sensor(0, ruta_actual, error_sensor);
	pthread_mutex_unlock(&lock_sensor);

	if (fd_control >= 0)
		eventfd_write(fd_control, 1);

	return 1;
}

int sensor_cambiar_ruta(const char *entrada, char *canonica, size_t tam)
{
	int r;

	r = analisis_validar_ruta(entrada, canonica, tam);
	if (r != RUTA_OK)
		return r;

	pthread_mutex_lock(&lock_sensor);
	if (ruta_valida && strcmp(ruta_actual, canonica) == 0) {
		pthread_mutex_unlock(&lock_sensor);
		return 0;
	}

	if (esqueleto) {
		struct config_sensor cfg;
		__u32 cero = 0;

		memset(&cfg, 0, sizeof(cfg));
		snprintf(cfg.ruta, sizeof(cfg.ruta), "%.200s/", canonica);
		cfg.largo = (unsigned int)strlen(cfg.ruta);

		if (bpf_map_update_elem(bpf_map__fd(esqueleto->maps.config_sensor), &cero, &cfg, BPF_ANY) < 0) {
			pthread_mutex_unlock(&lock_sensor);
			return RUTA_ERROR_MAPA;
		}
	}

	snprintf(ruta_actual, sizeof(ruta_actual), "%s", canonica);
	ruta_valida = 1;
	error_sensor[0] = '\0';
	analisis_reiniciar();
	analisis_publicar_sensor(esqueleto != NULL, ruta_actual, error_sensor);
	pthread_mutex_unlock(&lock_sensor);

	if (fd_control >= 0)
		eventfd_write(fd_control, 1);

	return 1;
}

void sensor_finalizar(void)
{
	sensor_desactivar();
	pthread_mutex_lock(&lock_sensor);
	if (fd_control >= 0) {
		close(fd_control);
		fd_control = -1;
	}
	pthread_mutex_unlock(&lock_sensor);
}

void archivos_despertar(void)
{
	pthread_mutex_lock(&lock_sensor);
	if (fd_control >= 0)
		eventfd_write(fd_control, 1);
	pthread_mutex_unlock(&lock_sensor);
}

void *hilo_archivos(void *sin_uso)
{
	unsigned long gen_analizada = 0;
	unsigned long ver_evaluada = 0;
	(void)sin_uso;

	while (ejecutando) {
		struct pollfd pf[2];
		nfds_t n = 0;
		int fd_c = -1;
		int fd_rb = -1;
		int pr;
		eventfd_t val;
		char dir[PATH_MAX];
		unsigned long gen;
		int activo;
		size_t eventos_a_procesar;

		pthread_mutex_lock(&lock_sensor);
		fd_c = fd_control;
		if (consumidor)
			fd_rb = ring_buffer__epoll_fd(consumidor);
		pthread_mutex_unlock(&lock_sensor);

		if (fd_c < 0)
			break;

		pf[n++] = (struct pollfd){ .fd = fd_c, .events = POLLIN, .revents = 0 };
		if (fd_rb >= 0)
			pf[n++] = (struct pollfd){ .fd = fd_rb, .events = POLLIN, .revents = 0 };

		pr = poll(pf, n, -1);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			registrar("error en poll de hilo archivos: %s", strerror(errno));
			if (!ejecutando)
				break;
			continue;
		}

		if (pf[0].revents & POLLIN)
			eventfd_read(fd_c, &val);

		if (!ejecutando)
			break;

		pthread_mutex_lock(&lock_sensor);
		n_lote = 0;
		if (consumidor)
			ring_buffer__consume(consumidor);
		snprintf(dir, sizeof(dir), "%s", ruta_actual);
		gen = analisis_generacion();
		activo = (esqueleto != NULL);
		eventos_a_procesar = n_lote;
		pthread_mutex_unlock(&lock_sensor);

		if (activo && gen != gen_analizada) {
			ver_evaluada = blacklist_version();
			analisis_linea_base(dir, gen);
			gen_analizada = gen;
		}

		if (activo && blacklist_version() != ver_evaluada) {
			unsigned long ver = blacklist_version();
			analisis_reevaluar(dir, gen);
			ver_evaluada = ver;
		}

		for (size_t i = 0; i < eventos_a_procesar; i++)
			analisis_procesar(dir, gen, &lote[i]);
	}

	return NULL;
}
