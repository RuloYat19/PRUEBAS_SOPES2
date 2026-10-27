#ifndef COMUN_H
#define COMUN_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

#define SYS_IPC_CHANNEL_SEND	548
#define SYS_IPC_CHANNEL_RECEIVE	549
#define SYS_START_LOG_WATCH	550
#define SYS_STOP_LOG_WATCH	551
#define SYS_GET_PROCESS_INFO	552
#define SYS_GET_PROCESSES_STATE	553

#define TASK_COMM_LEN		16
#define IPC_MSG_MAX_LEN		256
#define LOG_WATCH_MAX_FILES	8

#define MAX_EVENTOS		100
#define MAX_PALABRAS		8
#define MENSAJE_PARO		"CTL|STOP"

struct process_info {
	char name[TASK_COMM_LEN];
	pid_t pid;
	unsigned long long runtime_ns;
	unsigned long memory_kb;
};

struct processes_state {
	unsigned int total;
	unsigned int running;
	unsigned int sleeping;
	unsigned int zombie;
};

struct proceso {
	char nombre[TASK_COMM_LEN];
	pid_t pid;
	char estado;
};

struct evento {
	unsigned long id;
	char archivo[IPC_MSG_MAX_LEN];
	char palabra[IPC_MSG_MAX_LEN];
	char contenido[IPC_MSG_MAX_LEN];
	char timestamp[32];
};

struct estado {
	pthread_mutex_t lock;
	unsigned long memoria_usada;
	unsigned long memoria_libre;
	unsigned long memoria_cache;
	unsigned long swap_usada;
	struct processes_state resumen;
	struct proceso *procesos;
	size_t n_procesos;
	struct evento eventos[MAX_EVENTOS];
	unsigned long total_eventos;
};

struct buf {
	char *datos;
	size_t largo;
	size_t capacidad;
	int fallo;
};

extern struct estado estado;
extern volatile sig_atomic_t ejecutando;
extern int fd_aviso;

void registrar(const char *formato, ...) __attribute__((format(printf, 1, 2)));

void buf_agregar(struct buf *b, const void *datos, size_t largo);
void buf_printf(struct buf *b, const char *formato, ...)
	__attribute__((format(printf, 2, 3)));
void buf_liberar(struct buf *b);
void json_cadena(struct buf *b, const char *texto);
void json_evento(struct buf *b, const struct evento *e);
void json_estado(struct buf *b);

void *hilo_metricas(void *intervalo_segundos);
void metricas_despertar(void);

void *hilo_eventos(void *sin_uso);
void eventos_despertar(void);

int servidor_iniciar(int puerto);
void servidor_bucle(int fd_escucha, const char *dir_frontend);

#endif
