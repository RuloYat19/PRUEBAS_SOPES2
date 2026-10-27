CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2 -D_GNU_SOURCE

PROGRAMA = daemon_monitor
FUENTES = main.c metricas.c eventos.c http.c json.c
OBJETOS = $(FUENTES:.c=.o)

all: $(PROGRAMA)

$(PROGRAMA): $(OBJETOS)
	$(CC) $(CFLAGS) -o $@ $(OBJETOS)

%.o: %.c comun.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJETOS) $(PROGRAMA)

.PHONY: all clean
