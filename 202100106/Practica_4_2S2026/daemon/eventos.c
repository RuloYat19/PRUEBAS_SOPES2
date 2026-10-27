#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

static void copiar(char *destino, size_t tamano, const char *origen)
{
	snprintf(destino, tamano, "%s", origen);
}

static int interpretar(char *mensaje, struct evento *e)
{
	char *campos[5];
	struct tm desglose;
	time_t epoca;
	int i;

	campos[0] = mensaje;
	for (i = 1; i < 5; i++) {
		campos[i] = strchr(campos[i - 1], '|');
		if (!campos[i])
			return -1;
		*campos[i]++ = '\0';
	}
	if (strcmp(campos[0], "EVT"))
		return -1;

	epoca = (time_t)strtoll(campos[1], NULL, 10);
	localtime_r(&epoca, &desglose);
	strftime(e->timestamp, sizeof(e->timestamp), "%Y-%m-%d %H:%M:%S", &desglose);

	copiar(e->palabra, sizeof(e->palabra), campos[2]);
	copiar(e->archivo, sizeof(e->archivo), campos[3]);
	copiar(e->contenido, sizeof(e->contenido), campos[4]);
	return 0;
}

static void publicar(const struct evento *nuevo)
{
	struct evento *destino;

	pthread_mutex_lock(&estado.lock);
	destino = &estado.eventos[estado.total_eventos % MAX_EVENTOS];
	*destino = *nuevo;
	destino->id = ++estado.total_eventos;
	pthread_mutex_unlock(&estado.lock);

	eventfd_write(fd_aviso, 1);
}

void *hilo_eventos(void *sin_uso)
{
	char mensaje[IPC_MSG_MAX_LEN];
	struct evento e;

	(void)sin_uso;

	while (ejecutando) {
		memset(mensaje, 0, sizeof(mensaje));
		if (syscall(SYS_IPC_CHANNEL_RECEIVE, mensaje) < 0) {
			if (errno == EINTR)
				continue;
			registrar("sys_ipc_channel_receive fallo: %s", strerror(errno));
			if (errno == ENOSYS) {
				registrar("el kernel no tiene el canal IPC, panel de eventos inactivo");
				break;
			}
			sleep(1);
			continue;
		}
		mensaje[sizeof(mensaje) - 1] = '\0';

		if (!strcmp(mensaje, MENSAJE_PARO))
			continue;

		memset(&e, 0, sizeof(e));
		if (interpretar(mensaje, &e) < 0) {
			registrar("mensaje IPC ignorado, formato desconocido");
			continue;
		}
		publicar(&e);
		registrar("evento: '%s' en %s", e.palabra, e.archivo);
	}
	return NULL;
}

void eventos_despertar(void)
{
	if (syscall(SYS_IPC_CHANNEL_SEND, MENSAJE_PARO) < 0)
		registrar("sys_ipc_channel_send fallo: %s", strerror(errno));
}
