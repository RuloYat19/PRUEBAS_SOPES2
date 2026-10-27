#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_IPC_CHANNEL_SEND 443

int main(int argc, char *argv[]) {
    char mensaje[256];
    
    if (argc > 1) {
        strncpy(mensaje, argv[1], 255);
    } else {
        printf("Ingrese mensaje: ");
        fgets(mensaje, 256, stdin);
        mensaje[strcspn(mensaje, "\n")] = '\0';
    }
    
    printf("Enviando mensaje: '%s'\n", mensaje);
    
    long ret = syscall(SYS_IPC_CHANNEL_SEND, mensaje);
    
    if (ret == 0) {
        printf("Mensaje enviado exitosamente\n");
    } else {
        perror("Error al enviar");
        return 1;
    }
    
    return 0;
}