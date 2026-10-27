CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2 -D_GNU_SOURCE -Ilib -Iebpf
LDLIBS = -lpam -lbpf -lcrypto

PROGRAMA = daemon_monitor
FUENTES = main.c metricas.c procesos.c ipc.c http.c json.c config.c sensor.c peticion.c auth.c alertas.c analisis.c blacklist.c rutas.c logwatch.c lib/cJSON.c
OBJETOS = $(FUENTES:.c=.o)

all: $(PROGRAMA)

$(PROGRAMA): $(OBJETOS)
	$(CC) $(CFLAGS) -o $@ $(OBJETOS) $(LDLIBS)

%.o: %.c comun.h
	$(CC) $(CFLAGS) -c -o $@ $<

ebpf/sensor.skel.h: ebpf/sensor.bpf.c ebpf/sensor.h ebpf/filtro_ruta.h
	$(MAKE) -C ebpf

sensor.o: sensor.c comun.h ebpf/sensor.skel.h
	$(CC) $(CFLAGS) -c -o $@ $<

rutas.o: rutas.c rutas.h
	$(CC) $(CFLAGS) -c -o $@ $<

lib/cJSON.o: lib/cJSON.c lib/cJSON.h
	$(CC) $(CFLAGS) -c -o $@ $<

pruebas: config.o peticion.o auth.o alertas.o json.o metricas.o procesos.o analisis.o blacklist.o rutas.o logwatch.o lib/cJSON.o pruebas/test_config.c pruebas/test_peticion.c pruebas/test_auth.c pruebas/test_alertas.c pruebas/test_metricas.c pruebas/test_procesos.c pruebas/test_analisis.c pruebas/test_blacklist.c pruebas/test_logwatch.c
	$(CC) $(CFLAGS) -o pruebas/test_config pruebas/test_config.c config.o
	./pruebas/test_config
	$(CC) $(CFLAGS) -o pruebas/test_peticion pruebas/test_peticion.c peticion.o
	./pruebas/test_peticion
	$(CC) $(CFLAGS) -o pruebas/test_auth pruebas/test_auth.c auth.o $(LDLIBS)
	./pruebas/test_auth
	$(CC) $(CFLAGS) -o pruebas/test_alertas pruebas/test_alertas.c alertas.o json.o metricas.o procesos.o analisis.o blacklist.o rutas.o lib/cJSON.o -lcrypto
	./pruebas/test_alertas
	$(CC) $(CFLAGS) -o pruebas/test_metricas pruebas/test_metricas.c metricas.o alertas.o json.o procesos.o analisis.o blacklist.o rutas.o lib/cJSON.o -lcrypto
	./pruebas/test_metricas
	$(CC) $(CFLAGS) -o pruebas/test_procesos pruebas/test_procesos.c procesos.o metricas.o alertas.o json.o analisis.o blacklist.o rutas.o lib/cJSON.o -lcrypto
	./pruebas/test_procesos
	$(CC) $(CFLAGS) -o pruebas/test_analisis pruebas/test_analisis.c analisis.o rutas.o alertas.o json.o metricas.o procesos.o blacklist.o lib/cJSON.o -lcrypto
	./pruebas/test_analisis
	$(CC) $(CFLAGS) -o pruebas/test_blacklist pruebas/test_blacklist.c blacklist.o lib/cJSON.o
	./pruebas/test_blacklist
	$(CC) $(CFLAGS) -o pruebas/test_logwatch pruebas/test_logwatch.c logwatch.o rutas.o json.o alertas.o metricas.o procesos.o analisis.o blacklist.o lib/cJSON.o -lcrypto
	./pruebas/test_logwatch

clean:
	rm -f $(OBJETOS) $(PROGRAMA) pruebas/test_config pruebas/test_peticion pruebas/test_auth pruebas/test_alertas pruebas/test_metricas pruebas/test_procesos pruebas/test_analisis pruebas/test_blacklist pruebas/test_logwatch lib/cJSON.o
	$(MAKE) -C ebpf clean

.PHONY: all clean pruebas
