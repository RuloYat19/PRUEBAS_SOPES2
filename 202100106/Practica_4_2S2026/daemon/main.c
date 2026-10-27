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

struct opciones {
	int puerto;
	int intervalo;
	int segundo_plano;
	int n_palabras;
	const char *palabras[MAX_PALABRAS];
	const char *central;
	const char *frontend;
};

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

static int preparar_archivo(const char *ruta, char *absoluta)
{
	int fd;

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

static void ubicar_frontend(char *destino, size_t tamano)
{
	char ejecutable[PATH_MAX];
	char candidata[PATH_MAX + 16];
	ssize_t largo;

	largo = readlink("/proc/self/exe", ejecutable, sizeof(ejecutable) - 1);
	if (largo < 0) {
		snprintf(destino, tamano, "../frontend");
		return;
	}
	ejecutable[largo] = '\0';

	snprintf(candidata, sizeof(candidata), "%s/../frontend", dirname(ejecutable));
	if (!realpath(candidata, destino))
		snprintf(destino, tamano, "%s", candidata);
}

static void uso(const char *programa)
{
	fprintf(stderr,
		"Uso: %s [opciones] [log1 ... log%d]\n\n"
		"  -p puerto     Puerto HTTP (8080)\n"
		"  -i segundos   Intervalo de metricas (2)\n"
		"  -k palabra    Palabra clave a vigilar, repetible (ERROR, FAIL)\n"
		"  -c archivo    Log centralizado (~/so2_logs/log_centralizado.txt)\n"
		"  -w carpeta    Carpeta del dashboard (../frontend)\n"
		"  -d            Ejecutar en segundo plano\n\n"
		"Sin rutas se vigilan ~/so2_logs/auth.log y ~/so2_logs/app.log.\n",
		programa, LOG_WATCH_MAX_FILES);
}

static int leer_opciones(int argc, char *argv[], struct opciones *o)
{
	int opcion;

	o->puerto = 8080;
	o->intervalo = 2;

	while ((opcion = getopt(argc, argv, "p:i:k:c:w:dh")) != -1) {
		switch (opcion) {
		case 'p':
			o->puerto = atoi(optarg);
			break;
		case 'i':
			o->intervalo = atoi(optarg);
			break;
		case 'k':
			if (o->n_palabras == MAX_PALABRAS) {
				fprintf(stderr, "Maximo %d palabras clave.\n", MAX_PALABRAS);
				return -1;
			}
			o->palabras[o->n_palabras++] = optarg;
			break;
		case 'c':
			o->central = optarg;
			break;
		case 'w':
			o->frontend = optarg;
			break;
		case 'd':
			o->segundo_plano = 1;
			break;
		default:
			return -1;
		}
	}

	if (o->puerto <= 0 || o->puerto > 65535 || o->intervalo <= 0 ||
	    argc - optind > LOG_WATCH_MAX_FILES)
		return -1;

	if (!o->n_palabras) {
		o->palabras[o->n_palabras++] = "ERROR";
		o->palabras[o->n_palabras++] = "FAIL";
	}
	return 0;
}

static void detener_hilo_eventos(pthread_t hilo)
{
	struct timespec limite;
	int intento;

	eventos_despertar();
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
	static char rutas_abs[LOG_WATCH_MAX_FILES][PATH_MAX];
	static char central[PATH_MAX];
	static char frontend[PATH_MAX + 32];
	char carpeta[PATH_MAX];
	char predeterminada[PATH_MAX + 64];
	const char *rutas[LOG_WATCH_MAX_FILES];
	int watch_ids[MAX_PALABRAS];
	struct opciones o = { 0 };
	pthread_t eventos, metricas;
	int hay_eventos, hay_metricas;
	const char *home;
	sigset_t paro;
	int n_rutas = 0, n_watches = 0;
	int fd_escucha, salida;
	long id;
	int i;

	if (leer_opciones(argc, argv, &o) < 0) {
		uso(argv[0]);
		return 1;
	}

	home = getenv("HOME");
	if (!home)
		home = ".";
	snprintf(carpeta, sizeof(carpeta), "%s/so2_logs", home);
	mkdir(carpeta, 0755);

	if (o.frontend)
		snprintf(frontend, sizeof(frontend), "%s", o.frontend);
	else
		ubicar_frontend(frontend, sizeof(frontend));

	if (!o.central) {
		snprintf(predeterminada, sizeof(predeterminada),
			 "%s/log_centralizado.txt", carpeta);
		o.central = predeterminada;
	}
	if (preparar_archivo(o.central, central) < 0) {
		fprintf(stderr, "No se pudo preparar %s: %s\n", o.central, strerror(errno));
		return 1;
	}

	if (optind == argc) {
		static char auth[PATH_MAX + 64], app[PATH_MAX + 64];
		static char *por_defecto[2] = { auth, app };

		snprintf(auth, sizeof(auth), "%s/auth.log", carpeta);
		snprintf(app, sizeof(app), "%s/app.log", carpeta);
		argv = por_defecto;
		argc = 2;
		optind = 0;
	}
	for (i = optind; i < argc; i++) {
		if (preparar_archivo(argv[i], rutas_abs[n_rutas]) < 0) {
			registrar("no se vigilara %s: %s", argv[i], strerror(errno));
			continue;
		}
		if (!strcmp(rutas_abs[n_rutas], central)) {
			fprintf(stderr, "El log centralizado no puede ser un archivo vigilado.\n");
			return 1;
		}
		rutas[n_rutas] = rutas_abs[n_rutas];
		n_rutas++;
	}

	if (o.segundo_plano) {
		snprintf(predeterminada, sizeof(predeterminada),
			 "%s/daemon_monitor.out", carpeta);
		if (daemon(1, 0) < 0) {
			perror("daemon");
			return 1;
		}
		salida = open(predeterminada, O_WRONLY | O_CREAT | O_APPEND, 0644);
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

	fd_escucha = servidor_iniciar(o.puerto);
	if (fd_escucha < 0) {
		registrar("no se pudo abrir el puerto %d: %s", o.puerto, strerror(errno));
		return 1;
	}

	instalar_senales();

	for (i = 0; i < o.n_palabras && n_rutas > 0; i++) {
		id = syscall(SYS_START_LOG_WATCH, rutas, n_rutas, central, o.palabras[i]);
		if (id < 0) {
			registrar("sys_start_log_watch('%s') fallo: %s",
				  o.palabras[i], strerror(errno));
			continue;
		}
		watch_ids[n_watches++] = (int)id;
		registrar("vigilando '%s' en %d archivos, watch_id %ld",
			  o.palabras[i], n_rutas, id);
	}

	sigemptyset(&paro);
	sigaddset(&paro, SIGINT);
	sigaddset(&paro, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &paro, NULL);
	hay_eventos = pthread_create(&eventos, NULL, hilo_eventos, NULL) == 0;
	hay_metricas = pthread_create(&metricas, NULL, hilo_metricas, &o.intervalo) == 0;
	pthread_sigmask(SIG_UNBLOCK, &paro, NULL);
	if (!hay_eventos || !hay_metricas)
		registrar("no se pudieron crear todos los hilos");

	registrar("daemon_monitor PID %d, puerto %d, dashboard en %s",
		  getpid(), o.puerto, frontend);

	servidor_bucle(fd_escucha, frontend);

	registrar("senal de paro recibida, cerrando");
	ejecutando = 0;

	for (i = 0; i < n_watches; i++) {
		if (syscall(SYS_STOP_LOG_WATCH, watch_ids[i]) < 0)
			registrar("sys_stop_log_watch(%d) fallo: %s",
				  watch_ids[i], strerror(errno));
		else
			registrar("watch_id %d detenido", watch_ids[i]);
	}

	if (hay_metricas) {
		metricas_despertar();
		pthread_join(metricas, NULL);
	}
	if (hay_eventos)
		detener_hilo_eventos(eventos);

	close(fd_aviso);
	free(estado.procesos);
	pthread_mutex_destroy(&estado.lock);
	registrar("daemon finalizado");
	return 0;
}
