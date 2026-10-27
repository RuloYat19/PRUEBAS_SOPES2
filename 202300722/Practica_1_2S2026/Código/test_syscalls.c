#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

#define __NR_getpid_counter 472

int main() {
    printf("Llamando a getpid() varias veces...\n");
    for (int i = 0; i < 5; i++) {
        printf("getpid() devolvió: %d\n", getpid());
    }

    printf("\nConsultando el contador a través de getpid_counter()...\n");
    long counter = syscall(__NR_getpid_counter);
    printf("El contador es: %ld\n", counter);

    return 0;
}