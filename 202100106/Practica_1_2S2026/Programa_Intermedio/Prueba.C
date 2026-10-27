/*
 * ==============================================================================
 * Universidad de San Carlos de Guatemala
 * Facultad de Ingenieria - Escuela de Ciencias y Sistemas
 * Sistemas Operativos 2 - 2S2026
 *
 * Practica 1: Modificacion de funciones del kernel
 * Carne: 202100106
 *
 * Descripcion:
 * Programa de prueba en espacio de usuario que invoca de forma repetida
 * la llamada al sistema sys_getpid() para incrementar el contador atomico
 * global en el kernel, y consulta el valor acumulado mediante la nueva
 * syscall personalizada sys_getpid_counter() (syscall 472).
 * ==============================================================================
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

/* Numero de syscall registrado en arch/x86/entry/syscalls/syscall_64.tbl */
#define SYS_GETPID_COUNTER 472

/* Cantidad de invocaciones a getpid() en cada ciclo de prueba */
#define REPETICIONES 5

int main(void)
{
	long antes = 0;
	long despues = 0;
	pid_t pid = 0;
	int i = 0;

	printf("==============================================================\n");
	printf("       PRACTICA 1 - SISTEMAS OPERATIVOS 2 (2S2026)            \n");
	printf("       Validacion de Syscalls: getpid() y getpid_counter()    \n");
	printf("       Estudiante Carne: 202100106                            \n");
	printf("==============================================================\n\n");

	/* 1. Invocacion inicial de la syscall personalizada */
	antes = syscall(SYS_GETPID_COUNTER);
	if (antes < 0) {
		perror("[ERROR] Fallo al invocar getpid_counter()");
		fprintf(stderr, "Detalle: Verifique que el kernel activo tenga registrada la syscall %d\n", SYS_GETPID_COUNTER);
		return 1;
	}
	printf("[1] Contador global en kernel ANTES del ciclo: %ld\n\n", antes);

	/* 2. Invocaciones sucesivas a getpid() usando directamente la syscall */
	printf("[2] Ejecutando %d llamadas explicitas a syscall(SYS_getpid):\n", REPETICIONES);
	for (i = 1; i <= REPETICIONES; i++) {
		pid = (pid_t) syscall(SYS_getpid);
		printf("    -> Invocacion #%d: PID obtenido = %d\n", i, pid);
	}

	/* 3. Invocacion final de la syscall personalizada */
	despues = syscall(SYS_GETPID_COUNTER);
	if (despues < 0) {
		perror("[ERROR] Fallo al invocar getpid_counter() despues del ciclo");
		return 1;
	}

	printf("\n[3] Contador global en kernel DESPUES del ciclo: %ld\n", despues);
	printf("    -> Incremento total observado en el kernel: +%ld\n", (despues - antes));
	printf("\n[4] Mensaje registrado en el buffer circular del kernel (dmesg).\n");
	printf("    Para verificarlo en vivo, ejecute: sudo dmesg | grep getpid_counter\n");
	printf("==============================================================\n");

	return 0;
}
