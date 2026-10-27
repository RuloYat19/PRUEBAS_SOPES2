#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

static void copiar(char *destino, size_t tamano, const char *origen)
{
	snprintf(destino, tamano, "%s", origen);
}

static int interpretar(char *mensaje, char *palabra, size_t tam_palabra,
		       char *archivo, size_t tam_archivo,
		       char *contenido, size_t tam_contenido,
		       time_t *epoca)
{
	char *campos[5];
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

	*epoca = (time_t)strtoll(campos[1], NULL, 10);
	copiar(palabra, tam_palabra, campos[2]);
	copiar(archivo, tam_archivo, campos[3]);
	copiar(contenido, tam_contenido, campos[4]);
	return 0;
}

void *hilo_ipc(void *sin_uso)
{
	char mensaje[IPC_MSG_MAX_LEN];
	char palabra[64];
	char archivo[IPC_MSG_MAX_LEN];
	char contenido[IPC_MSG_MAX_LEN];
	time_t epoca;

	(void)sin_uso;

	while (ejecutando) {
		memset(mensaje, 0, sizeof(mensaje));
		if (syscall(SYS_IPC_CHANNEL_RECEIVE, mensaje) < 0) {
			if (errno == EINTR)
				continue;
			registrar("sys_ipc_channel_receive fallo: %s", strerror(errno));
			if (errno == ENOSYS) {
				registrar("el kernel no tiene el canal IPC, panel de eventos inactivo");
				while (ejecutando)
					pause();
				break;
			}
			sleep(1);
			continue;
		}
		mensaje[sizeof(mensaje) - 1] = '\0';

		if (!strcmp(mensaje, MENSAJE_PARO))
			continue;

		palabra[0] = '\0';
		archivo[0] = '\0';
		contenido[0] = '\0';
		epoca = 0;

		if (interpretar(mensaje, palabra, sizeof(palabra),
				archivo, sizeof(archivo),
				contenido, sizeof(contenido),
				&epoca) < 0) {
			registrar("mensaje IPC ignorado, formato desconocido");
			continue;
		}
		alerta_emitir_log(palabra, archivo, contenido, epoca);
	}
	return NULL;
}

void ipc_despertar(void)
{
	if (syscall(SYS_IPC_CHANNEL_SEND, MENSAJE_PARO) < 0)
		registrar("sys_ipc_channel_send fallo: %s", strerror(errno));
}
