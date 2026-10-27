#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

#define SYS_STOP_LOG_WATCH 551

int main(int argc, char *argv[])
{
	long resultado;
	int id_monitoreo;

	printf("==============================================================\n");
	printf("  PRACTICA 3 - SO2 (2S2026) - DETENCION DE MONITOREO DE LOGS\n");
	printf("  Carne: 202100106\n");
	printf("  Syscall: sys_stop_log_watch (%d)\n", SYS_STOP_LOG_WATCH);
	printf("==============================================================\n\n");

	if (argc < 2) {
		printf("Uso: %s <id_monitoreo>\n\n", argv[0]);
		printf("Ejemplo: %s 1\n", argv[0]);
		return 1;
	}

	id_monitoreo = atoi(argv[1]);
	printf("Solicitando la detencion del monitoreo con id %d...\n\n", id_monitoreo);

	resultado = syscall(SYS_STOP_LOG_WATCH, id_monitoreo);

	if (resultado == 0) {
		printf("[EXITO] Codigo de respuesta 0.\n");
		printf("        El hilo de kernel fue detenido con kthread_stop y los\n");
		printf("        recursos asociados al id %d fueron liberados.\n", id_monitoreo);
		return 0;
	}

	printf("[ERROR] Codigo de respuesta %ld (%s)\n", resultado, strerror(errno));

	if (errno == ESRCH)
		printf("        El id %d no existe o ya habia sido detenido.\n", id_monitoreo);
	else if (errno == EINVAL)
		printf("        El id %d no es un identificador valido.\n", id_monitoreo);

	return 1;
}
