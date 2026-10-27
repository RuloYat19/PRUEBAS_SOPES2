#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

struct estado estado = { .lock = PTHREAD_MUTEX_INITIALIZER };
volatile sig_atomic_t ejecutando = 1;
int fd_aviso = -1;

static pthread_mutex_t lock_registro = PTHREAD_MUTEX_INITIALIZER;

void registrar(const char *formato, ...)
{
	char fecha[32];
	struct tm desglose;
	va_list args;
	time_t ahora;

	ahora = time(NULL);
	localtime_r(&ahora, &desglose);
	strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", &desglose);

	pthread_mutex_lock(&lock_registro);
	fprintf(stderr, "[%s] ", fecha);
	va_start(args, formato);
	vfprintf(stderr, formato, args);
	va_end(args);
	fputc('\n', stderr);
	fflush(stderr);
	pthread_mutex_unlock(&lock_registro);
}

static void manejar_paro(int senal)
{
	uint64_t uno = 1;
	int errno_previo = errno;
	ssize_t escritos;

	(void)senal;
	ejecutando = 0;
	escritos = write(fd_aviso, &uno, sizeof(uno));
	(void)escritos;
	errno = errno_previo;
}

static void manejar_despertar(int senal)
{
	(void)senal;
}

static void instalar_senales(void)
{
	struct sigaction accion;

	memset(&accion, 0, sizeof(accion));
	sigemptyset(&accion.sa_mask);

	accion.sa_handler = manejar_paro;
	sigaction(SIGINT, &accion, NULL);
	sigaction(SIGTERM, &accion, NULL);

	accion.sa_handler = manejar_despertar;
	sigaction(SIGUSR1, &accion, NULL);

	signal(SIGPIPE, SIG_IGN);
}

static void asegurar_directorio_padre(const char *ruta)
{
	char dir[PATH_MAX * 2];
	char *barra;
	size_t len = strlen(ruta);
	if (len >= sizeof(dir))
		len = sizeof(dir) - 1;
	memcpy(dir, ruta, len);
	dir[len] = '\0';
	barra = strrchr(dir, '/');
	if (barra && barra != dir) {
		*barra = '\0';
		mkdir(dir, 0755);
	}
}

static int preparar_archivo(const char *ruta, char *absoluta)
{
	int fd;

	asegurar_directorio_padre(ruta);
	fd = open(ruta, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	close(fd);

	if (!realpath(ruta, absoluta))
		return -1;
	if (!strncmp(absoluta, "/tmp/", 5))
		registrar("aviso: %s esta en /tmp, fs.protected_regular puede bloquear al kernel",
			  absoluta);
	return 0;
}

static void uso(const char *programa)
{
	fprintf(stderr,
		"Uso: %s [-c archivo] [-d] [-h]\n"
		"  -c archivo   Archivo de configuracion (por defecto: daemon.conf junto al ejecutable)\n"
		"  -d           Ejecutar en segundo plano\n"
		"  -h           Mostrar esta ayuda\n",
		programa);
}

static void detener_hilo_ipc(pthread_t hilo)
{
	struct timespec limite;
	int intento;

	ipc_despertar();
	for (intento = 0; intento < 10; intento++) {
		clock_gettime(CLOCK_REALTIME, &limite);
		limite.tv_sec += 1;
		if (pthread_timedjoin_np(hilo, NULL, &limite) == 0)
			return;
		pthread_kill(hilo, SIGUSR1);
	}
	registrar("el hilo de eventos no respondio al paro");
}

int main(int argc, char *argv[])
{
	static char rutas_abs[MAX_RUTAS_LOG][PATH_MAX];
	static char central[PATH_MAX];
	static char frontend[PATH_MAX];
	char carpeta[PATH_MAX];
	char archivo_salida[PATH_MAX * 2];
	char conf_defecto[PATH_MAX * 2];
	char dir_ejecutable[PATH_MAX];
	const char *rutas[MAX_RUTAS_LOG];
	struct configuracion conf;
	const char *archivo_conf = NULL;
	int segundo_plano = 0;
	pthread_t t_monitoreo, t_archivos, t_ipc;
	int hay_monitoreo = 0, hay_archivos = 0, hay_ipc = 0;
	const char *home;
	sigset_t paro;
	int n_rutas = 0;
	int fd_escucha, salida;
	int i, opt;

	while ((opt = getopt(argc, argv, "c:dh")) != -1) {
		switch (opt) {
		case 'c':
			archivo_conf = optarg;
			break;
		case 'd':
			segundo_plano = 1;
			break;
		case 'h':
			uso(argv[0]);
			return 0;
		default:
			uso(argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		uso(argv[0]);
		return 1;
	}

	ssize_t largo = readlink("/proc/self/exe", dir_ejecutable, sizeof(dir_ejecutable) - 1);
	if (largo > 0) {
		dir_ejecutable[largo] = '\0';
		char *d = dirname(dir_ejecutable);
		memmove(dir_ejecutable, d, strlen(d) + 1);
	} else {
		snprintf(dir_ejecutable, sizeof(dir_ejecutable), ".");
	}

	config_por_defecto(&conf, dir_ejecutable);
	if (archivo_conf) {
		if (config_cargar(&conf, archivo_conf) < 0) {
			fprintf(stderr, "Error: archivo de configuracion '%s' no existe o no es legible.\n", archivo_conf);
			return 1;
		}
	} else {
		snprintf(conf_defecto, sizeof(conf_defecto), "%s/daemon.conf", dir_ejecutable);
		if (access(conf_defecto, R_OK) == 0) {
			config_cargar(&conf, conf_defecto);
		} else {
			registrar("advertencia: no se encontro daemon.conf en %s, usando valores por defecto", dir_ejecutable);
		}
	}

	home = obtener_home();
	snprintf(carpeta, sizeof(carpeta), "%s/so2_logs", home);
	mkdir(carpeta, 0755);

	snprintf(frontend, sizeof(frontend), "%s", conf.dashboard);

	if (preparar_archivo(conf.log_central, central) < 0) {
		fprintf(stderr, "No se pudo preparar %s: %s\n", conf.log_central, strerror(errno));
		return 1;
	}

	for (i = 0; i < conf.n_log_rutas; i++) {
		if (preparar_archivo(conf.log_rutas[i], rutas_abs[n_rutas]) < 0) {
			registrar("no se vigilara %s: %s", conf.log_rutas[i], strerror(errno));
			continue;
		}
		if (!strcmp(rutas_abs[n_rutas], central)) {
			fprintf(stderr, "El log centralizado no puede ser un archivo vigilado.\n");
			return 1;
		}
		rutas[n_rutas] = rutas_abs[n_rutas];
		n_rutas++;
	}

	if (segundo_plano) {
		snprintf(archivo_salida, sizeof(archivo_salida),
			 "%s/daemon_monitor.out", carpeta);
		if (daemon(1, 0) < 0) {
			perror("daemon");
			return 1;
		}
		salida = open(archivo_salida, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (salida >= 0) {
			dup2(salida, STDERR_FILENO);
			close(salida);
		}
	}

	fd_aviso = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd_aviso < 0) {
		registrar("eventfd fallo: %s", strerror(errno));
		return 1;
	}

	auth_iniciar();

	fd_escucha = servidor_iniciar(conf.puerto);
	if (fd_escucha < 0) {
		registrar("no se pudo abrir el puerto %d: %s", conf.puerto, strerror(errno));
		return 1;
	}

	blacklist_iniciar(conf.blacklist);
	sensor_iniciar(conf.ruta_vigilada);
	if (sensor_activar() < 0) {
		struct estado_sensor est;
		analisis_estado_sensor(&est);
		registrar("sensor eBPF no disponible: %s", est.error);
	}

	instalar_senales();

	char base_copia[PATH_MAX];
	snprintf(base_copia, sizeof(base_copia), "%s", central);
	logwatch_iniciar(dirname(base_copia));

	for (i = 0; i < conf.n_log_palabras && n_rutas > 0; i++) {
		struct peticion_logwatch p;
		struct logwatch creado;
		char err[256];
		snprintf(p.palabra, sizeof(p.palabra), "%s", conf.log_palabras[i]);
		snprintf(p.central, sizeof(p.central), "%s", central);
		p.n_rutas = n_rutas;
		for (int r = 0; r < n_rutas; r++)
			snprintf(p.rutas[r], sizeof(p.rutas[r]), "%s", rutas[r]);
		if (logwatch_crear(&p, "configuracion", "", &creado, err, sizeof(err)) < 0) {
			registrar("sys_start_log_watch('%s') fallo: %s",
				  conf.log_palabras[i], err);
			continue;
		}
		registrar("vigilando '%s' en %d archivos, watch_id %d",
			  conf.log_palabras[i], n_rutas, creado.id);
	}

	sigemptyset(&paro);
	sigaddset(&paro, SIGINT);
	sigaddset(&paro, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &paro, NULL);
	if (pthread_create(&t_monitoreo, NULL, hilo_monitoreo, &conf.intervalo) == 0) {
		hay_monitoreo = 1;
		pthread_setname_np(t_monitoreo, "monitoreo");
	}
	if (pthread_create(&t_archivos, NULL, hilo_archivos, NULL) == 0) {
		hay_archivos = 1;
		pthread_setname_np(t_archivos, "archivos");
	}
	if (pthread_create(&t_ipc, NULL, hilo_ipc, NULL) == 0) {
		hay_ipc = 1;
		pthread_setname_np(t_ipc, "ipc");
	}
	pthread_sigmask(SIG_UNBLOCK, &paro, NULL);
	if (!hay_monitoreo || !hay_archivos || !hay_ipc)
		registrar("no se pudieron crear todos los hilos");

	registrar("daemon_monitor PID %d, puerto %d, dashboard en %s",
		  getpid(), conf.puerto, frontend);

	servidor_bucle(fd_escucha, frontend, conf.intervalo);

	registrar("senal de paro recibida, cerrando");
	ejecutando = 0;

	logwatch_finalizar_todos();

	if (hay_monitoreo) {
		monitoreo_despertar();
		pthread_join(t_monitoreo, NULL);
	}
	if (hay_archivos) {
		archivos_despertar();
		pthread_join(t_archivos, NULL);
	}
	sensor_finalizar();
	blacklist_finalizar();
	if (hay_ipc)
		detener_hilo_ipc(t_ipc);

	close(fd_aviso);
	free(estado.procesos);
	pthread_mutex_destroy(&estado.lock);
	registrar("daemon finalizado");
	return 0;
}
