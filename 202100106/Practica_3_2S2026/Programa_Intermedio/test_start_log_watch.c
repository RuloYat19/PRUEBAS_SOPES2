#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

#define SYS_START_LOG_WATCH 550
#define LOG_WATCH_MAX_FILES 8

int main(int argc, char *argv[])
{
	const char *rutas[LOG_WATCH_MAX_FILES];
	const char *palabra_clave;
	const char *ruta_log_central;
	long watch_id;
	int cantidad;
	int i;

	printf("==============================================================\n");
	printf("  PRACTICA 3 - SO2 (2S2026) - INICIO DE MONITOREO DE LOGS\n");
	printf("  Carne: 202100106\n");
	printf("  Syscall: sys_start_log_watch (%d)\n", SYS_START_LOG_WATCH);
	printf("==============================================================\n\n");

	if (argc < 4) {
		printf("Uso: %s <palabra_clave> <log_central> <log1> [log2 ... log%d]\n\n",
		       argv[0], LOG_WATCH_MAX_FILES);
		printf("Ejemplo:\n");
		printf("  %s ERROR ~/so2_logs/log_centralizado.txt \\\n", argv[0]);
		printf("     ~/so2_logs/app1.log ~/so2_logs/app2.log\n");
		return 1;
	}

	palabra_clave = argv[1];
	ruta_log_central = argv[2];
	cantidad = argc - 3;

	if (cantidad > LOG_WATCH_MAX_FILES) {
		printf("[ERROR] El kernel admite un maximo de %d archivos vigilados,\n",
		       LOG_WATCH_MAX_FILES);
		printf("        se recibieron %d.\n", cantidad);
		return 1;
	}

	for (i = 0; i < cantidad; i++)
		rutas[i] = argv[i + 3];

	printf("Palabra clave      : %s\n", palabra_clave);
	printf("Log centralizado   : %s\n", ruta_log_central);
	printf("Archivos vigilados : %d\n", cantidad);
	for (i = 0; i < cantidad; i++)
		printf("   [%d] %s\n", i + 1, rutas[i]);
	printf("\n");

	watch_id = syscall(SYS_START_LOG_WATCH, rutas, cantidad,
			   ruta_log_central, palabra_clave);

	if (watch_id < 0) {
		perror("[ERROR] sys_start_log_watch");
		printf("Codigo de respuesta: %ld\n", watch_id);
		return 1;
	}

	printf("[OK] Monitoreo activo en un hilo de kernel independiente.\n");
	printf("     watch_id asignado: %ld\n\n", watch_id);
	printf("El hilo sigue vigilando aunque este proceso termine.\n");
	printf("Verificar el hilo   : ps -ef | grep log_watch_%ld\n", watch_id);
	printf("Detener el monitoreo: ./test_stop_log_watch %ld\n", watch_id);

	return 0;
}
