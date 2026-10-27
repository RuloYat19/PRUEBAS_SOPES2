#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>

// Definir los números de syscall
#define SYS_GET_PROCESS_INFO 470
#define SYS_GET_PROCESSES_STATE 471

// Definir las estructuras (deben coincidir con las del kernel)
#define MAX_PROCESS_NAME 16

struct process_info {
    char name[MAX_PROCESS_NAME];
    pid_t pid;
    unsigned long utime;
    unsigned long stime;
    unsigned long vsize;
};

struct processes_state {
    int total;
    int running;
    int sleeping;
    int zombie;
};

void print_process_info(struct process_info *info) {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║              INFORMACIÓN DEL PROCESO                 ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Nombre del proceso: %-40s ║\n", info->name);
    printf("║ PID:               %-40d ║\n", info->pid);
    printf("║ Tiempo de usuario:  %-40lu ║\n", info->utime);
    printf("║ Tiempo de sistema:  %-40lu ║\n", info->stime);
    printf("║ Memoria virtual:    %-40lu bytes ║\n", info->vsize);
    printf("╚══════════════════════════════════════════════════════╝\n");
}

void print_processes_state(struct processes_state *state) {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║            ESTADO GLOBAL DE PROCESOS                 ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║ Total procesos:     %-40d ║\n", state->total);
    printf("║ En ejecución:       %-40d ║\n", state->running);
    printf("║ En espera/IDLE:     %-40d ║\n", state->sleeping);
    printf("║ Zombies:            %-40d ║\n", state->zombie);
    printf("╚══════════════════════════════════════════════════════╝\n");
}

int main(int argc, char *argv[]) {
    struct process_info info;
    struct processes_state state;
    pid_t pid;
    long result;
    
    printf("\n=== SISTEMA DE MÉTRICAS DE PROCESOS ===\n");
    
    // 1. Obtener estadísticas globales
    printf("\nObteniendo estadísticas globales de procesos...\n");
    result = syscall(SYS_GET_PROCESSES_STATE, &state);
    if (result == 0) {
        print_processes_state(&state);
    } else {
        printf("Error al obtener estadísticas globales: %ld\n", result);
        perror("syscall");
    }
    
    // 2. Obtener información de un proceso específico
    if (argc > 1) {
        pid = atoi(argv[1]);
    } else {
        pid = getpid(); // Usar el PID del propio proceso
    }
    
    printf("\nObteniendo información del proceso PID=%d...\n", pid);
    result = syscall(SYS_GET_PROCESS_INFO, pid, &info);
    
    if (result == 0) {
        print_process_info(&info);
    } else if (result == -ESRCH) {
        printf("Proceso con PID %d no encontrado\n", pid);
    } else {
        printf("Error al obtener información: %ld\n", result);
        perror("syscall");
    }
    
    // 3. Prueba con el proceso init (PID=1)
    printf("\nProbando con el proceso INIT (PID=1)...\n");
    result = syscall(SYS_GET_PROCESS_INFO, 1, &info);
    if (result == 0) {
        print_process_info(&info);
    }
    
    printf("\nPrueba completada.\n");
    
    return 0;
}