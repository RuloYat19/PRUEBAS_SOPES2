#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_STOP_LOG_WATCH 446

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <id_monitoreo>\n", argv[0]);
        return 1;
    }
    
    int id = atoi(argv[1]);
    
    printf("Deteniendo monitoreo ID: %d\n", id);
    
    long ret = syscall(SYS_STOP_LOG_WATCH, id);
    
    if (ret == 0) {
        printf("Monitoreo detenido exitosamente\n");
    } else if (ret == -2) {
        printf("Error: ID %d no encontrado\n", id);
        return 1;
    } else {
        perror("Error al detener");
        return 1;
    }
    
    return 0;
}