#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#define SYS_START_LOG_WATCH 445

int main(int argc, char *argv[]) {
    if (argc < 5) {
        printf("Uso: %s <log1> [log2...] <log_central> <palabra_clave>\n", argv[0]);
        printf("Ejemplo: %s /var/log/syslog /var/log/auth.log /tmp/alertas.log ERROR\n", argv[0]);
        return 1;
    }
    
    int num_logs = argc - 3;
    char *rutas[10];
    char *log_central = argv[argc - 2];
    char *palabra = argv[argc - 1];
    
    for (int i = 0; i < num_logs; i++) {
        rutas[i] = argv[i + 1];
    }
    
    printf("🔍 Iniciando monitoreo de %d archivo(s):\n", num_logs);
    for (int i = 0; i < num_logs; i++) {
        printf("   - %s\n", rutas[i]);
    }
    printf("Log central: %s\n", log_central);
    printf("Palabra clave: %s\n", palabra);
    
    long id = syscall(SYS_START_LOG_WATCH, rutas, num_logs, log_central, palabra);
    
    if (id > 0) {
        printf("Monitoreo iniciado con ID: %ld\n", id);
        printf("   Use: ./stop_watch %ld para detenerlo\n", id);
    } else {
        perror("Error al iniciar monitoreo");
        return 1;
    }
    
    return 0;
}