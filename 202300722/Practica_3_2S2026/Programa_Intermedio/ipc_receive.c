#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_IPC_CHANNEL_RECEIVE 444

int main() {
    char buffer[256];
    
    printf("Esperando mensaje (bloqueado)...\n");
    printf("   (Este proceso se dormirá hasta recibir un mensaje)\n");
    
    long ret = syscall(SYS_IPC_CHANNEL_RECEIVE, buffer);
    
    if (ret == 0) {
        printf("Mensaje recibido: '%s'\n", buffer);
    } else {
        perror("Error al recibir");
        return 1;
    }
    
    return 0;
}