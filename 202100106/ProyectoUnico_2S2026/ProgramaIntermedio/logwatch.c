#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"
#include "ebpf/filtro_ruta.h"
#include "lib/cJSON.h"
#include "rutas.h"

static pthread_mutex_t lock_logwatch = PTHREAD_MUTEX_INITIALIZER;
static struct logwatch registro[MAX_LOGWATCH];
static size_t cantidad = 0;
static char base_dir[PATH_MAX] = "";

static long default_start_log_watch(const char *const *rutas, int n, const char *central, const char *palabra)
{
	return syscall(SYS_START_LOG_WATCH, rutas, n, central, palabra);
}

static long default_stop_log_watch(int id)
{
	return syscall(SYS_STOP_LOG_WATCH, id);
}

static fn_start_log_watch actual_start = default_start_log_watch;
static fn_stop_log_watch actual_stop = default_stop_log_watch;

void logwatch_usar_syscalls(fn_start_log_watch fn_start, fn_stop_log_watch fn_stop)
{
	actual_start = fn_start ? fn_start : default_start_log_watch;
	actual_stop = fn_stop ? fn_stop : default_stop_log_watch;
}

void logwatch_iniciar(const char *base)
{
	pthread_mutex_lock(&lock_logwatch);
	cantidad = 0;
	memset(registro, 0, sizeof(registro));
	if (base) {
		char res[PATH_MAX];
		if (realpath(base, res))
			snprintf(base_dir, sizeof(base_dir), "%s", res);
		else
			snprintf(base_dir, sizeof(base_dir), "%s", base);
		size_t len = strlen(base_dir);
		while (len > 1 && base_dir[len - 1] == '/')
			base_dir[--len] = '\0';
	} else {
		base_dir[0] = '\0';
	}
	pthread_mutex_unlock(&lock_logwatch);
}

void logwatch_base(char *destino, size_t tam)
{
	if (!destino || tam == 0)
		return;
	pthread_mutex_lock(&lock_logwatch);
	snprintf(destino, tam, "%s", base_dir);
	pthread_mutex_unlock(&lock_logwatch);
}

static int ruta_valida(const char *ruta, int es_central)
{
	char base_local[PATH_MAX];
	char base_con_barra[PATH_MAX + 2];
	unsigned int largo_base;
	const char *nombre;
	struct stat st;
	int fd;
	size_t len;

	if (!ruta || ruta[0] != '/')
		return 0;

	len = strlen(ruta);
	if (len == 0 || len > 255)
		return 0;

	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)ruta[i];
		if (c == '|' || c < 0x20 || c == 0x7f)
			return 0;
	}

	logwatch_base(base_local, sizeof(base_local));
	if (base_local[0] == '\0')
		return 0;

	snprintf(base_con_barra, sizeof(base_con_barra), "%s/", base_local);
	largo_base = (unsigned int)strlen(base_con_barra);

	if (!ruta_en_directorio(ruta, base_con_barra, largo_base))
		return 0;

	nombre = ruta + largo_base;
	if (strcmp(nombre, ".") == 0 || strcmp(nombre, "..") == 0)
		return 0;

	fd = abrir_sin_enlaces(ruta, O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
	if (es_central) {
		if (fd < 0) {
			if (errno == ENOENT)
				return 1;
			return 0;
		}
		int ok = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
		close(fd);
		return ok;
	} else {
		if (fd < 0)
			return 0;
		int ok = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
		close(fd);
		return ok;
	}
}

int logwatch_validar(const char *json, struct peticion_logwatch *p, int *codigo, char *error, size_t tam)
{
	cJSON *root;
	cJSON *item_palabra;
	cJSON *item_central;
	cJSON *item_rutas;
	const char *pal;
	const char *cen;
	size_t pal_len;
	int n_rutas;

	if (!json || !codigo || !error || tam == 0)
		return -1;

	root = cJSON_Parse(json);
	if (!root || !cJSON_IsObject(root)) {
		if (root)
			cJSON_Delete(root);
		*codigo = 400;
		snprintf(error, tam, "Cuerpo invalido");
		return -1;
	}

	if (cJSON_GetArraySize(root) != 3) {
		cJSON_Delete(root);
		*codigo = 400;
		snprintf(error, tam, "Cuerpo invalido");
		return -1;
	}

	item_palabra = cJSON_GetObjectItemCaseSensitive(root, "palabra");
	item_central = cJSON_GetObjectItemCaseSensitive(root, "central");
	item_rutas = cJSON_GetObjectItemCaseSensitive(root, "rutas");

	if (!item_palabra || !cJSON_IsString(item_palabra) ||
	    !item_central || !cJSON_IsString(item_central) ||
	    !item_rutas || !cJSON_IsArray(item_rutas)) {
		cJSON_Delete(root);
		*codigo = 400;
		snprintf(error, tam, "Cuerpo invalido");
		return -1;
	}

	n_rutas = cJSON_GetArraySize(item_rutas);
	for (int i = 0; i < n_rutas; i++) {
		cJSON *r = cJSON_GetArrayItem(item_rutas, i);
		if (!r || !cJSON_IsString(r)) {
			cJSON_Delete(root);
			*codigo = 400;
			snprintf(error, tam, "Cuerpo invalido");
			return -1;
		}
	}

	pal = item_palabra->valuestring;
	pal_len = strlen(pal);
	if (pal_len < 1 || pal_len > MAX_PALABRA_LOG) {
		cJSON_Delete(root);
		*codigo = 400;
		snprintf(error, tam, "Palabra invalida");
		return -1;
	}

	for (size_t i = 0; i < pal_len; i++) {
		unsigned char c = (unsigned char)pal[i];
		if (c == '|' || c < 0x20 || c == 0x7f) {
			cJSON_Delete(root);
			*codigo = 400;
			snprintf(error, tam, "Palabra invalida");
			return -1;
		}
	}

	if (n_rutas < 1 || n_rutas > LOG_WATCH_MAX_FILES) {
		cJSON_Delete(root);
		*codigo = 400;
		snprintf(error, tam, "Se requieren entre 1 y 8 rutas");
		return -1;
	}

	cen = item_central->valuestring;
	if (!ruta_valida(cen, 1)) {
		cJSON_Delete(root);
		*codigo = 400;
		snprintf(error, tam, "Ruta central invalida");
		return -1;
	}

	for (int i = 0; i < n_rutas; i++) {
		const char *r_str = cJSON_GetArrayItem(item_rutas, i)->valuestring;
		if (!ruta_valida(r_str, 0)) {
			*codigo = 400;
			snprintf(error, tam, "Ruta invalida: %s", r_str);
			cJSON_Delete(root);
			return -1;
		}
	}

	for (int i = 0; i < n_rutas; i++) {
		const char *ri = cJSON_GetArrayItem(item_rutas, i)->valuestring;
		for (int j = i + 1; j < n_rutas; j++) {
			const char *rj = cJSON_GetArrayItem(item_rutas, j)->valuestring;
			if (strcmp(ri, rj) == 0) {
				*codigo = 400;
				snprintf(error, tam, "Ruta repetida: %s", rj);
				cJSON_Delete(root);
				return -1;
			}
		}
	}

	for (int i = 0; i < n_rutas; i++) {
		const char *ri = cJSON_GetArrayItem(item_rutas, i)->valuestring;
		if (strcmp(cen, ri) == 0) {
			*codigo = 400;
			snprintf(error, tam, "La ruta central no puede ser un archivo vigilado");
			cJSON_Delete(root);
			return -1;
		}
	}

	pthread_mutex_lock(&lock_logwatch);
	for (size_t k = 0; k < cantidad; k++) {
		const struct logwatch *lw = &registro[k];
		for (int r = 0; r < lw->n_rutas; r++) {
			if (strcmp(cen, lw->rutas[r]) == 0) {
				*codigo = 409;
				snprintf(error, tam, "Conflicto con el logwatch %d: un log central no puede vigilarse", lw->id);
				pthread_mutex_unlock(&lock_logwatch);
				cJSON_Delete(root);
				return -1;
			}
		}
		for (int m = 0; m < n_rutas; m++) {
			const char *rm = cJSON_GetArrayItem(item_rutas, m)->valuestring;
			if (strcmp(rm, lw->central) == 0) {
				*codigo = 409;
				snprintf(error, tam, "Conflicto con el logwatch %d: un log central no puede vigilarse", lw->id);
				pthread_mutex_unlock(&lock_logwatch);
				cJSON_Delete(root);
				return -1;
			}
		}
	}

	if (cantidad >= MAX_LOGWATCH) {
		*codigo = 409;
		snprintf(error, tam, "Limite de 16 logwatch alcanzado");
		pthread_mutex_unlock(&lock_logwatch);
		cJSON_Delete(root);
		return -1;
	}
	pthread_mutex_unlock(&lock_logwatch);

	if (p) {
		snprintf(p->palabra, sizeof(p->palabra), "%s", pal);
		snprintf(p->central, sizeof(p->central), "%s", cen);
		p->n_rutas = n_rutas;
		for (int i = 0; i < n_rutas; i++)
			snprintf(p->rutas[i], sizeof(p->rutas[i]), "%s", cJSON_GetArrayItem(item_rutas, i)->valuestring);
	}

	cJSON_Delete(root);
	return 0;
}

int logwatch_crear(const struct peticion_logwatch *p, const char *origen, const char *usuario, struct logwatch *creado, char *error, size_t tam)
{
	const char *rutas_ptr[LOG_WATCH_MAX_FILES];
	struct tm tm_info;
	time_t ahora;
	long id;

	if (!p)
		return -1;

	if (origen && strcmp(origen, "dashboard") == 0) {
		int cfd = abrir_sin_enlaces(p->central, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
		if (cfd >= 0) {
			char b[PATH_MAX];
			logwatch_base(b, sizeof(b));
			int bfd = abrir_sin_enlaces(b, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
			if (bfd >= 0) {
				struct stat st;
				if (fstat(bfd, &st) == 0) {
					if (fchown(cfd, st.st_uid, st.st_gid) != 0)
						registrar("fchown fallido en %s: %s", p->central, strerror(errno));
				}
				close(bfd);
			}
			close(cfd);
		}
	}

	for (int i = 0; i < p->n_rutas; i++)
		rutas_ptr[i] = p->rutas[i];

	id = actual_start(rutas_ptr, p->n_rutas, p->central, p->palabra);
	if (id <= 0) {
		if (error && tam > 0)
			snprintf(error, tam, "%s", strerror(errno));
		return -2;
	}

	ahora = time(NULL);
	localtime_r(&ahora, &tm_info);

	pthread_mutex_lock(&lock_logwatch);
	if (cantidad >= MAX_LOGWATCH) {
		pthread_mutex_unlock(&lock_logwatch);
		actual_stop((int)id);
		if (error && tam > 0)
			snprintf(error, tam, "Limite de 16 logwatch alcanzado");
		return -1;
	}

	struct logwatch *lw = &registro[cantidad++];
	lw->id = (int)id;
	snprintf(lw->palabra, sizeof(lw->palabra), "%s", p->palabra);
	snprintf(lw->central, sizeof(lw->central), "%s", p->central);
	lw->n_rutas = p->n_rutas;
	for (int i = 0; i < p->n_rutas; i++)
		snprintf(lw->rutas[i], sizeof(lw->rutas[i]), "%s", p->rutas[i]);
	snprintf(lw->origen, sizeof(lw->origen), "%s", origen ? origen : "");
	if (usuario)
		snprintf(lw->usuario, sizeof(lw->usuario), "%s", usuario);
	else
		lw->usuario[0] = '\0';
	strftime(lw->creado, sizeof(lw->creado), "%Y-%m-%d %H:%M:%S", &tm_info);

	if (creado)
		*creado = *lw;

	pthread_mutex_unlock(&lock_logwatch);
	return 0;
}

int logwatch_finalizar(int id, char *error, size_t tam)
{
	int encontrado = 0;
	long ret;

	pthread_mutex_lock(&lock_logwatch);
	for (size_t i = 0; i < cantidad; i++) {
		if (registro[i].id == id) {
			encontrado = 1;
			break;
		}
	}
	if (!encontrado) {
		pthread_mutex_unlock(&lock_logwatch);
		return -ENOENT;
	}
	pthread_mutex_unlock(&lock_logwatch);

	ret = actual_stop(id);
	if (ret < 0) {
		if (errno == ESRCH) {
			pthread_mutex_lock(&lock_logwatch);
			for (size_t i = 0; i < cantidad; i++) {
				if (registro[i].id == id) {
					for (size_t j = i; j + 1 < cantidad; j++)
						registro[j] = registro[j + 1];
					cantidad--;
					break;
				}
			}
			pthread_mutex_unlock(&lock_logwatch);
			return -ENOENT;
		}
		if (error && tam > 0)
			snprintf(error, tam, "%s", strerror(errno));
		return -1;
	}

	pthread_mutex_lock(&lock_logwatch);
	for (size_t i = 0; i < cantidad; i++) {
		if (registro[i].id == id) {
			for (size_t j = i; j + 1 < cantidad; j++)
				registro[j] = registro[j + 1];
			cantidad--;
			break;
		}
	}
	pthread_mutex_unlock(&lock_logwatch);

	return 0;
}

static int comparar_id(const void *a, const void *b)
{
	const struct logwatch *la = a;
	const struct logwatch *lb = b;

	if (la->id < lb->id)
		return -1;
	if (la->id > lb->id)
		return 1;
	return 0;
}

size_t logwatch_lista(struct logwatch *destino, size_t max)
{
	size_t n = 0;

	if (!destino || max == 0)
		return 0;

	pthread_mutex_lock(&lock_logwatch);
	n = (cantidad < max) ? cantidad : max;
	for (size_t i = 0; i < n; i++)
		destino[i] = registro[i];
	pthread_mutex_unlock(&lock_logwatch);

	if (n > 1)
		qsort(destino, n, sizeof(struct logwatch), comparar_id);

	return n;
}

void logwatch_finalizar_todos(void)
{
	int ids[MAX_LOGWATCH];
	size_t n;

	pthread_mutex_lock(&lock_logwatch);
	n = cantidad;
	for (size_t i = 0; i < n; i++)
		ids[i] = registro[i].id;
	cantidad = 0;
	memset(registro, 0, sizeof(registro));
	pthread_mutex_unlock(&lock_logwatch);

	for (size_t i = 0; i < n; i++) {
		if (actual_stop(ids[i]) == 0)
			registrar("logwatch %d detenido", ids[i]);
		else
			registrar("sys_stop_log_watch(%d) fallo: %s", ids[i], strerror(errno));
	}
}
