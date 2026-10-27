#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

#define SYS_IPC_CHANNEL_SEND 548
#define IPC_MSG_MAX_LEN      256

int main(int argc, char *argv[])
{
	char mensaje[IPC_MSG_MAX_LEN];
	long resultado;
	size_t largo;
	int enviados = 0;

	printf("==============================================================\n");
	printf("  PRACTICA 3 - SO2 (2S2026) - EMISOR DEL CANAL IPC\n");
	printf("  Carne: 202100106\n");
	printf("  Syscall: sys_ipc_channel_send (%d)\n", SYS_IPC_CHANNEL_SEND);
	printf("==============================================================\n\n");

	if (argc > 1) {
		resultado = syscall(SYS_IPC_CHANNEL_SEND, argv[1]);
		if (resultado < 0) {
			perror("[ERROR] sys_ipc_channel_send");
			printf("Codigo de respuesta: %ld\n", resultado);
			return 1;
		}
		printf("[OK] Mensaje enviado. Codigo de respuesta: %ld\n", resultado);
		return 0;
	}

	printf("Escriba un mensaje y presione ENTER.\n");
	printf("Escriba 'salir' para terminar el emisor.\n\n");

	while (1) {
		printf("mensaje> ");
		fflush(stdout);

		if (!fgets(mensaje, sizeof(mensaje), stdin))
			break;

		largo = strlen(mensaje);
		if (largo > 0 && mensaje[largo - 1] == '\n')
			mensaje[largo - 1] = '\0';

		if (strlen(mensaje) == 0)
			continue;

		if (strcmp(mensaje, "salir") == 0)
			break;

		resultado = syscall(SYS_IPC_CHANNEL_SEND, mensaje);
		if (resultado < 0) {
			printf("  [ERROR] codigo %ld (%s)\n", resultado, strerror(errno));
		} else {
			enviados++;
			printf("  [OK] enviado, codigo de respuesta %ld\n", resultado);
		}
	}

	printf("\nEmisor finalizado. Mensajes enviados: %d\n", enviados);
	return 0;
}
