#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

#define SYS_IPC_CHANNEL_RECEIVE 549
#define IPC_MSG_MAX_LEN         256

int main(int argc, char *argv[])
{
	char buffer[IPC_MSG_MAX_LEN];
	long resultado;
	int recibidos = 0;
	int maximo = 0;

	if (argc > 1)
		maximo = atoi(argv[1]);

	printf("==============================================================\n");
	printf("  PRACTICA 3 - SO2 (2S2026) - RECEPTOR DEL CANAL IPC\n");
	printf("  Carne: 202100106\n");
	printf("  Syscall: sys_ipc_channel_receive (%d)\n", SYS_IPC_CHANNEL_RECEIVE);
	printf("  PID de este proceso: %d\n", getpid());
	printf("==============================================================\n\n");

	while (maximo == 0 || recibidos < maximo) {
		printf("[ESPERA] Proceso bloqueado pasivamente en la cola de espera\n");
		printf("         del kernel. Sin consumo de CPU hasta que llegue\n");
		printf("         un mensaje. Verificar con: ps -o pid,stat,%%cpu -p %d\n\n",
		       getpid());
		fflush(stdout);

		memset(buffer, 0, sizeof(buffer));
		resultado = syscall(SYS_IPC_CHANNEL_RECEIVE, buffer);

		if (resultado < 0) {
			if (errno == EINTR) {
				printf("[AVISO] Espera interrumpida por una senal.\n");
				break;
			}
			perror("[ERROR] sys_ipc_channel_receive");
			printf("Codigo de respuesta: %ld\n", resultado);
			return 1;
		}

		recibidos++;
		printf("[RECIBIDO %d] %s\n\n", recibidos, buffer);
	}

	printf("Receptor finalizado. Mensajes recibidos: %d\n", recibidos);
	return 0;
}
