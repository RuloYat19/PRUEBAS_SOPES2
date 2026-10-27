#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>

/*
 * ==============================================================================
 * Universidad de San Carlos de Guatemala - Facultad de Ingenieria
 * Sistemas Operativos 2 - 2S2026
 * Practica 2: Programa de Prueba de Syscalls en Espacio de Usuario
 * = Carne: 202100106
 * ==============================================================================
 */

#define SYS_GET_PROCESS_INFO    548
#define SYS_GET_PROCESSES_STATE 549

#define TASK_COMM_LEN 16

/* Estructura para metricas de un proceso individual */
struct process_info {
	char name[TASK_COMM_LEN];
	pid_t pid;
	unsigned long long runtime_ns;
	unsigned long memory_kb;
};

/* Estructura para el estado global de procesos del sistema */
struct processes_state {
	unsigned int total;
	unsigned int running;
	unsigned int sleeping;
	unsigned int zombie;
};

int main(int argc, char *argv[])
{
	struct process_info pinfo;
	struct processes_state pstate;
	pid_t target_pid;
	long ret;

	printf("====================================================================\n");
	printf("       PRACTICA 2 - SISTEMAS OPERATIVOS 2 (2S2026)                  \n");
	printf("       Validacion de Syscalls: Metricas de Procesos del Kernel      \n");
	printf("       Nombre: Keneth Lopez Carne: 202100106                                  \n");
	printf("====================================================================\n\n");

	/* ------------------------------------------------------------------ */
	/* 1. Invocacion de sys_get_processes_state                           */
	/* ------------------------------------------------------------------ */
	printf("[1] Estado Global de Procesos (Syscall %d)...\n", SYS_GET_PROCESSES_STATE);
	memset(&pstate, 0, sizeof(pstate));
	ret = syscall(SYS_GET_PROCESSES_STATE, &pstate);

	if (ret < 0) {
		perror("    [ERROR] Fallo al invocar sys_get_processes_state");
	} else {
		printf("    ----------------------------------------\n");
		printf("    Total de Procesos  : %u\n", pstate.total);
		printf("    Procesos Running   : %u\n", pstate.running);
		printf("    Procesos Sleeping  : %u\n", pstate.sleeping);
		printf("    Procesos Zombie    : %u\n", pstate.zombie);
		printf("    ----------------------------------------\n\n");
	}

	/* ------------------------------------------------------------------ */
	/* 2. Invocacion de sys_get_process_info                              */
	/* ------------------------------------------------------------------ */
	target_pid = (argc > 1) ? atoi(argv[1]) : getpid();
	printf("[2] Consultando PID: %d (Syscall %d)...\n", target_pid, SYS_GET_PROCESS_INFO);
	memset(&pinfo, 0, sizeof(pinfo));
	ret = syscall(SYS_GET_PROCESS_INFO, target_pid, &pinfo);

	if (ret < 0) {
		fprintf(stderr, "    [ERROR] Fallo al invocar sys_get_process_info para PID %d: %s\n",
		        target_pid, strerror(errno));
	} else {
		printf("    ----------------------------------------\n");
		printf("    Nombre del Proceso : %s\n", pinfo.name);
		printf("    PID     : %d\n", pinfo.pid);
		printf("    Tiempo de CPU (ns) : %llu ns (%.3f ms)\n",
		       pinfo.runtime_ns, (double)pinfo.runtime_ns / 1000000.0);
		printf("    Memoria Aprox : %lu KB (%.2f MB)\n",
		       pinfo.memory_kb, (double)pinfo.memory_kb / 1024.0);
		printf("    ----------------------------------------\n");
	}
	printf("xddddddddddddddddddddddddddd");
	printf("    sudo dmesg | grep -E \"get_process_info|get_processes_state\"\n");
	printf("====================================================================\n");

	return 0;
}
