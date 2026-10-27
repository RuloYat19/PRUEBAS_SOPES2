#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SYS_IPC_CHANNEL_SEND    548
#define SYS_IPC_CHANNEL_RECEIVE 549
#define SYS_START_LOG_WATCH     550
#define SYS_STOP_LOG_WATCH      551
#define SYS_GET_PROCESS_INFO    552
#define SYS_GET_PROCESSES_STATE 553

#define TASK_COMM_LEN 16

struct process_info {
	char name[TASK_COMM_LEN];
	pid_t pid;
	unsigned long long runtime_ns;
	unsigned long memory_kb;
};

struct processes_state {
	unsigned int total;
	unsigned int running;
	unsigned int sleeping;
	unsigned int zombie;
};

int main(void)
{
	printf("==============================================================\n");
	printf("   VALIDACION DE LAS 6 SYSCALLS (FASE 1) - PRACTICA 4 SO2     \n");
	printf("==============================================================\n\n");

	/* 1. Probar sys_get_processes_state (553) */
	printf("[1] Probando sys_get_processes_state (%d)...\n", SYS_GET_PROCESSES_STATE);
	struct processes_state pstate;
	memset(&pstate, 0, sizeof(pstate));
	long ret = syscall(SYS_GET_PROCESSES_STATE, &pstate);
	if (ret < 0) {
		printf("    -> Syscall %d fallo: %s (errno=%d)\n", SYS_GET_PROCESSES_STATE, strerror(errno), errno);
		if (errno == ENOSYS)
			printf("       [NOTA] La syscall no esta en el kernel en ejecucion (%s).\n", strerror(errno));
	} else {
		printf("    -> Exito! Total=%u, Running=%u, Sleeping=%u, Zombie=%u\n",
		       pstate.total, pstate.running, pstate.sleeping, pstate.zombie);
	}

	/* 2. Probar sys_get_process_info (552) sobre el propio PID */
	printf("\n[2] Probando sys_get_process_info (%d) para PID %d...\n", SYS_GET_PROCESS_INFO, getpid());
	struct process_info pinfo;
	memset(&pinfo, 0, sizeof(pinfo));
	ret = syscall(SYS_GET_PROCESS_INFO, getpid(), &pinfo);
	if (ret < 0) {
		printf("    -> Syscall %d fallo: %s (errno=%d)\n", SYS_GET_PROCESS_INFO, strerror(errno), errno);
	} else {
		printf("    -> Exito! Nombre='%s', PID=%d, Runtime=%llu ns, Memoria=%lu KB\n",
		       pinfo.name, pinfo.pid, pinfo.runtime_ns, pinfo.memory_kb);
	}

	/* 3. Probar sys_ipc_channel_send (548) y sys_ipc_channel_receive (549) */
	printf("\n[3] Probando canal IPC (Send %d / Receive %d)...\n", SYS_IPC_CHANNEL_SEND, SYS_IPC_CHANNEL_RECEIVE);
	const char *mensaje_test = "TEST_IPC_MSG";
	ret = syscall(SYS_IPC_CHANNEL_SEND, mensaje_test);
	if (ret < 0) {
		printf("    -> sys_ipc_channel_send fallo: %s (errno=%d)\n", strerror(errno), errno);
	} else {
		printf("    -> sys_ipc_channel_send envio: '%s'\n", mensaje_test);
		char buffer_recibido[256];
		memset(buffer_recibido, 0, sizeof(buffer_recibido));
		ret = syscall(SYS_IPC_CHANNEL_RECEIVE, buffer_recibido);
		if (ret < 0) {
			printf("    -> sys_ipc_channel_receive fallo: %s (errno=%d)\n", strerror(errno), errno);
		} else {
			printf("    -> sys_ipc_channel_receive leyo: '%s'\n", buffer_recibido);
		}
	}

	/* 4. Probar sys_start_log_watch (550) y sys_stop_log_watch (551) */
	printf("\n[4] Probando log_watch (Start %d / Stop %d)...\n", SYS_START_LOG_WATCH, SYS_STOP_LOG_WATCH);
	char ruta_test_log[256];
	char ruta_central_log[256];
	snprintf(ruta_test_log, sizeof(ruta_test_log), "%s/so2_logs/test_fase1_src.log", getenv("HOME"));
	snprintf(ruta_central_log, sizeof(ruta_central_log), "%s/so2_logs/test_fase1_central.log", getenv("HOME"));

	char *rutas[] = { ruta_test_log };
	int watch_id = syscall(SYS_START_LOG_WATCH, rutas, 1, ruta_central_log, "ALERTA");
	if (watch_id < 0) {
		printf("    -> sys_start_log_watch fallo: %s (errno=%d)\n", strerror(errno), errno);
	} else {
		printf("    -> sys_start_log_watch inicio correctamente con watch_id = %d\n", watch_id);
		ret = syscall(SYS_STOP_LOG_WATCH, watch_id);
		if (ret < 0) {
			printf("    -> sys_stop_log_watch fallo: %s (errno=%d)\n", strerror(errno), errno);
		} else {
			printf("    -> sys_stop_log_watch detuvo correctamente watch_id = %d\n", watch_id);
		}
	}

	printf("\n==============================================================\n");
	printf("   FIN DE VALIDACION DE SYSCALLS                              \n");
	printf("==============================================================\n");
	return 0;
}
