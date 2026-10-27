#ifndef COMUN_H
#define COMUN_H

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "sensor.h"

#define MAX_ARCHIVOS		256
#define LOTE_EVENTOS		64
#define SENSOR_DIR_MAX		200
#define MAX_AMENAZAS		100
#define MAX_CUARENTENA		256
#define MAX_USUARIO		32

enum estado_archivo { ARCHIVO_BENIGNO, ARCHIVO_INFECTADO };
enum accion_archivo { ACCION_INICIAL, ACCION_CREADO, ACCION_MODIFICADO };
enum resultado_analisis { ANALISIS_IGNORADO, ANALISIS_SIN_CAMBIO, ANALISIS_CREADO, ANALISIS_MODIFICADO };
enum resultado_ruta { RUTA_OK = 0, RUTA_INVALIDA = -1, RUTA_NO_DIRECTORIO = -2, RUTA_LARGA = -3, RUTA_ERROR_MAPA = -4 };
enum contexto_analisis { CTX_EVENTO, CTX_INICIAL, CTX_REEVALUACION };
enum resultado_cuarentena {
	CUARENTENA_OK = 0,
	CUARENTENA_NO_LISTADO = -1,
	CUARENTENA_YA = -2,
	CUARENTENA_ERROR = -3
};

struct amenaza {
	unsigned long id;
	char firma[65];
	char descripcion[257];
	char ruta[SENSOR_RUTA_MAX];
	char sha256[65];
	unsigned int pid;
	char proceso[SENSOR_COMM_MAX];
	char fecha[32];
	int cuarentena;
};

struct registro_cuarentena {
	char ruta[SENSOR_RUTA_MAX];
	char sha256[65];
	char tipo[12];
	char motivo[65];
	char usuario[MAX_USUARIO + 1];
	char fecha[32];
	char permisos_anteriores[8];
};

struct archivo_analizado {
	unsigned long seq;
	char ruta[SENSOR_RUTA_MAX];
	char sha256[65];
	char firma[65];
	long long tamano;
	struct timespec mtime;
	enum estado_archivo estado;
	int cuarentena;
	enum accion_archivo accion;
	unsigned int pid;
	char proceso[SENSOR_COMM_MAX];
	char analizado[32];
};

struct estado_sensor {
	int activo;
	char ruta[PATH_MAX];
	char error[128];
};

#define SYS_IPC_CHANNEL_SEND	548
#define SYS_IPC_CHANNEL_RECEIVE	549
#define SYS_START_LOG_WATCH	550
#define SYS_STOP_LOG_WATCH	551
#define SYS_GET_PROCESS_INFO	552
#define SYS_GET_PROCESSES_STATE	553

#define TASK_COMM_LEN		16
#define IPC_MSG_MAX_LEN		256
#define LOG_WATCH_MAX_FILES	8

#define MAX_ALERTAS		200
#define MAX_DESCRIPCION		256
#define MAX_MUESTRAS		150
#define MAX_PALABRAS		8
#define MAX_RUTAS_LOG		LOG_WATCH_MAX_FILES
#define MENSAJE_PARO		"CTL|STOP"

#define TOP_PROCESOS		20
#define PID_MAXIMO		4194304
#define PF_KTHREAD		0x00200000UL

#define TAM_TOKEN		64
#define MAX_PASSWORD		128
#define MAX_SESIONES		64
#define SESION_INACTIVIDAD_S	1800
#define COOKIE_SESION		"so2_sesion"
#define SERVICIO_PAM		"so2-monitor"

enum rol { ROL_NINGUNO = 0, ROL_COMUN = 1, ROL_ADMIN = 2 };

enum severidad { SEV_LOW, SEV_MEDIUM, SEV_HIGH };
enum tipo_alerta { TIPO_ARCHIVOS, TIPO_SISTEMA, TIPO_SESION, TIPO_LOGS };

enum motivo_alerta {
	MOT_ARCHIVO_MODIFICADO,
	MOT_MONITOREO_ACTIVADO,
	MOT_SESION_EXITOSA,
	MOT_MONITOREO_DESACTIVADO,
	MOT_SESION_FALLIDA,
	MOT_PALABRA_EN_LOG,
	MOT_BLACKLIST_ACTUALIZADA,
	MOT_PROCESO_MATADO,
	MOT_HASH_MALICIOSO,
	MOT_ARCHIVO_CUARENTENA,
	MOT_TOTAL
};

struct alerta {
	unsigned long id;
	enum motivo_alerta motivo;
	enum tipo_alerta tipo;
	enum severidad severidad;
	char descripcion[MAX_DESCRIPCION];
	char timestamp[32];
	char archivo[IPC_MSG_MAX_LEN];
	char palabra[64];
	char contenido[IPC_MSG_MAX_LEN];
};

struct muestra {
	unsigned long id;
	char timestamp[32];
	int primera;
	unsigned long mem_total, mem_libre, mem_disponible, buffers, cached;
	unsigned long activas, inactivas, swap_total, swap_libre;
	unsigned long usada, cache, libre, swap_usada;
	unsigned long long pgfault, pgmajfault;
	unsigned long long pgfault_delta, pgmajfault_delta;
};

struct configuracion {
	int puerto;
	int intervalo;
	char dashboard[PATH_MAX];
	char log_central[PATH_MAX];
	char log_rutas[MAX_RUTAS_LOG][PATH_MAX];
	int n_log_rutas;
	char log_palabras[MAX_PALABRAS][64];
	int n_log_palabras;
	char ruta_vigilada[PATH_MAX];
	char blacklist[PATH_MAX];
};

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
	unsigned long rss_kb;
};

struct instantanea_procesos {
	int valida;
	char timestamp[32];
	struct processes_state resumen;
	struct proceso top[TOP_PROCESOS];
	size_t n_top;
	unsigned long mem_total_kb;
};

enum resultado_kill {
	KILL_OK,
	KILL_PID_INVALIDO,
	KILL_PID_UNO,
	KILL_DAEMON,
	KILL_KERNEL,
	KILL_NO_LIDER,
	KILL_NO_EXISTE
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
void json_estado(struct buf *b);

void config_por_defecto(struct configuracion *c, const char *dir_ejecutable);
int config_cargar(struct configuracion *c, const char *archivo);
const char *obtener_home(void);

const char *alerta_motivo_texto(enum motivo_alerta m);
enum tipo_alerta alerta_tipo(enum motivo_alerta m);
enum severidad alerta_severidad(enum motivo_alerta m);
const char *alerta_tipo_texto(enum tipo_alerta t);
const char *alerta_severidad_texto(enum severidad s);

unsigned long alerta_emitir(enum motivo_alerta m, const char *formato, ...)
	__attribute__((format(printf, 2, 3)));
unsigned long alerta_emitir_log(const char *palabra, const char *archivo,
				const char *contenido, time_t epoca);

size_t alertas_desde(unsigned long ultimo_id, struct alerta *destino, size_t max);
size_t alertas_recientes(struct alerta *destino, size_t max);
void alertas_reiniciar(void);

void json_alerta(struct buf *b, const struct alerta *a);

int metricas_leer_meminfo(const char *ruta, struct muestra *m);
int metricas_leer_vmstat(const char *ruta, struct muestra *m);
void metricas_calcular(struct muestra *m, const struct muestra *anterior);
unsigned long metricas_tomar(const char *ruta_meminfo, const char *ruta_vmstat);

size_t metricas_desde(unsigned long ultimo_id, struct muestra *destino, size_t max);
size_t metricas_historial(struct muestra *destino, size_t max);
void metricas_reiniciar(void);

int metricas_activas(void);
int metricas_cambiar_estado(int activo);

void json_muestra(struct buf *b, const struct muestra *m);
void json_monitoreo(struct buf *b);

int procesos_escanear(const char *raiz, struct proceso **lista, size_t *cantidad);
size_t procesos_top(const struct proceso *lista, size_t n, struct proceso *top, size_t max);
double procesos_porcentaje(unsigned long rss_kb, unsigned long mem_total_kb);
enum resultado_kill procesos_validar_kill(const char *raiz, long pid, pid_t pid_daemon,
					  char nombre[TASK_COMM_LEN]);
void procesos_tomar(const char *raiz);
void procesos_instantanea(struct instantanea_procesos *destino);

int procesos_activos(void);
int procesos_cambiar_estado(int activo);

void json_procesos(struct buf *b, const struct instantanea_procesos *p);

int peticion_encabezado(const char *peticion, const char *nombre, char *valor, size_t tam);
int peticion_cookie(const char *peticion, const char *nombre, char *valor, size_t tam);
long peticion_content_length(const char *peticion);

const char *rol_nombre(enum rol rol);
enum rol rol_de_grupos(int en_admin, int en_comun);
int rol_suficiente(enum rol rol, enum rol minimo);

void auth_iniciar(void);
int auth_verificar_pam(const char *usuario, const char *password);
enum rol auth_rol_de_usuario(const char *usuario);

int sesion_crear(const char *usuario, enum rol rol, time_t ahora, char token[TAM_TOKEN + 1]);
int sesion_buscar(const char *token, time_t ahora, char usuario[MAX_USUARIO + 1], enum rol *rol);
int sesion_vigente(const char *token, time_t ahora);
void sesion_eliminar(const char *token);
void sesion_purgar(time_t ahora);

int permiso_buscar(const char *metodo, const char *ruta, enum rol *minimo);

void *hilo_monitoreo(void *intervalo_segundos);
void monitoreo_despertar(void);

void *hilo_archivos(void *sin_uso);
void archivos_despertar(void);

void *hilo_ipc(void *sin_uso);
void ipc_despertar(void);

int servidor_iniciar(int puerto);
void servidor_bucle(int fd_escucha, const char *dir_frontend, int intervalo_s);

int analisis_validar_ruta(const char *entrada, char *canonica, size_t tam);
int analisis_sha256_fd(int fd, char hex[65]);
enum resultado_analisis analisis_procesar(const char *dir, unsigned long generacion,
					  const struct evento_archivo *e);
size_t analisis_linea_base(const char *dir, unsigned long generacion);
size_t analisis_reevaluar(const char *dir, unsigned long generacion);
enum resultado_cuarentena analisis_cuarentena_manual(const char *ruta, const char *usuario);
void analisis_reiniciar(void);
unsigned long analisis_generacion(void);
size_t analisis_desde(unsigned long ultimo_seq, struct archivo_analizado *destino, size_t max);
size_t analisis_listado(struct archivo_analizado *destino, size_t max);
size_t analisis_amenazas(struct amenaza *destino, size_t max);
size_t analisis_amenazas_desde(unsigned long ultimo_id, struct amenaza *destino, size_t max);
size_t analisis_cuarentena(struct registro_cuarentena *destino, size_t max);
void analisis_publicar_sensor(int activo, const char *ruta, const char *error);
void analisis_estado_sensor(struct estado_sensor *destino);

void json_archivo(struct buf *b, const struct archivo_analizado *a);
unsigned long json_archivos(struct buf *b);
void json_amenaza(struct buf *b, const struct amenaza *am);
void json_registro_cuarentena(struct buf *b, const struct registro_cuarentena *rc);
void json_blacklist(struct buf *b);

int sensor_iniciar(const char *ruta_configurada);
int sensor_activar(void);
int sensor_desactivar(void);
int sensor_cambiar_ruta(const char *entrada, char *canonica, size_t tam);
int sensor_activo(void);
void sensor_finalizar(void);

#define MAX_FIRMAS		5000
#define MAX_CUERPO_BLACKLIST	(1024 * 1024)

#define MAX_LOGWATCH		16
#define MAX_PALABRA_LOG		63

struct logwatch {
	int id;
	char palabra[MAX_PALABRA_LOG + 1];
	char central[PATH_MAX];
	char rutas[LOG_WATCH_MAX_FILES][PATH_MAX];
	int n_rutas;
	char origen[16];
	char usuario[MAX_USUARIO + 1];
	char creado[32];
};

struct peticion_logwatch {
	char palabra[MAX_PALABRA_LOG + 1];
	char central[PATH_MAX];
	char rutas[LOG_WATCH_MAX_FILES][PATH_MAX];
	int n_rutas;
};

typedef long (*fn_start_log_watch)(const char *const *rutas, int n, const char *central, const char *palabra);
typedef long (*fn_stop_log_watch)(int id);

void logwatch_iniciar(const char *base);
void logwatch_base(char *destino, size_t tam);
int logwatch_validar(const char *json, struct peticion_logwatch *p, int *codigo, char *error, size_t tam);
int logwatch_crear(const struct peticion_logwatch *p, const char *origen, const char *usuario, struct logwatch *creado, char *error, size_t tam);
int logwatch_finalizar(int id, char *error, size_t tam);
size_t logwatch_lista(struct logwatch *destino, size_t max);
void logwatch_finalizar_todos(void);
void logwatch_usar_syscalls(fn_start_log_watch fn_start, fn_stop_log_watch fn_stop);
void json_logwatch(struct buf *b, const struct logwatch *lw);

struct firma {
	char sha256[65];
	char nombre[65];
	char descripcion[257];
};

struct info_blacklist {
	char nombre[65];
	int version;
	char actualizado[33];
	size_t firmas;
	char origen[16];
	char actualizada_por[MAX_USUARIO + 1];
	char cargada[32];
	char error[160];
};

struct tabla_firmas;

int blacklist_iniciar(const char *ruta);
int blacklist_validar(const char *json, size_t largo, struct tabla_firmas **tabla,
		      struct info_blacklist *info, char *error, size_t tam_error);
int blacklist_actualizar(const char *json, size_t largo, const char *usuario,
			 size_t *firmas, char *error, size_t tam_error);
int blacklist_buscar(const char *sha256, struct firma *destino);
unsigned long blacklist_version(void);
void blacklist_info(struct info_blacklist *destino);
size_t blacklist_lista(struct firma *destino, size_t max);
void tabla_firmas_liberar(struct tabla_firmas *t);
void blacklist_finalizar(void);

#endif

