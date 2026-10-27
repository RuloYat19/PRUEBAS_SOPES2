#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <openssl/evp.h>

#include "comun.h"
#include "ebpf/filtro_ruta.h"
#include "rutas.h"

static pthread_mutex_t lock_archivos = PTHREAD_MUTEX_INITIALIZER;
static struct archivo_analizado listado[MAX_ARCHIVOS];
static size_t cantidad = 0;
static unsigned long ultimo_seq = 0;
static unsigned long generacion = 1;
static struct estado_sensor publicado;

static struct amenaza amenazas[MAX_AMENAZAS];
static unsigned long total_amenazas = 0;
static struct registro_cuarentena cuarentenas[MAX_CUARENTENA];
static unsigned long total_cuarentenas = 0;

int analisis_validar_ruta(const char *entrada, char *canonica, size_t tam)
{
	char res[PATH_MAX];
	struct stat st;
	size_t len;

	if (!entrada || entrada[0] != '/' || strlen(entrada) >= PATH_MAX)
		return RUTA_INVALIDA;

	if (!realpath(entrada, res))
		return RUTA_NO_DIRECTORIO;

	if (stat(res, &st) != 0 || !S_ISDIR(st.st_mode))
		return RUTA_NO_DIRECTORIO;

	if (strcmp(res, "/") == 0)
		return RUTA_INVALIDA;

	len = strlen(res);
	while (len > 1 && res[len - 1] == '/')
		res[--len] = '\0';

	if (len > SENSOR_DIR_MAX)
		return RUTA_LARGA;

	if (canonica) {
		if (tam <= len)
			return RUTA_LARGA;
		memcpy(canonica, res, len + 1);
	}

	return RUTA_OK;
}

int analisis_sha256_fd(int fd, char hex[65])
{
	EVP_MD_CTX *ctx;
	unsigned char buf[65536];
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int md_len = 0;
	ssize_t n;

	if (fd < 0 || !hex)
		return -1;

	ctx = EVP_MD_CTX_new();
	if (!ctx)
		return -1;

	if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_free(ctx);
		return -1;
	}

	while ((n = read(fd, buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			EVP_MD_CTX_free(ctx);
			return -1;
		}
		if (EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) {
			EVP_MD_CTX_free(ctx);
			return -1;
		}
	}

	if (EVP_DigestFinal_ex(ctx, md, &md_len) != 1 || md_len != 32) {
		EVP_MD_CTX_free(ctx);
		return -1;
	}

	EVP_MD_CTX_free(ctx);

	for (unsigned int i = 0; i < 32; i++)
		sprintf(hex + (i * 2), "%02x", md[i]);
	hex[64] = '\0';

	return 0;
}

void analisis_publicar_sensor(int activo, const char *ruta, const char *error)
{
	pthread_mutex_lock(&lock_archivos);
	publicado.activo = activo;
	if (ruta)
		snprintf(publicado.ruta, sizeof(publicado.ruta), "%s", ruta);
	else
		publicado.ruta[0] = '\0';
	if (error)
		snprintf(publicado.error, sizeof(publicado.error), "%s", error);
	else
		publicado.error[0] = '\0';
	pthread_mutex_unlock(&lock_archivos);
}

void analisis_estado_sensor(struct estado_sensor *destino)
{
	if (!destino)
		return;
	pthread_mutex_lock(&lock_archivos);
	*destino = publicado;
	pthread_mutex_unlock(&lock_archivos);
}

unsigned long analisis_generacion(void)
{
	unsigned long g;
	pthread_mutex_lock(&lock_archivos);
	g = generacion;
	pthread_mutex_unlock(&lock_archivos);
	return g;
}

void analisis_reiniciar(void)
{
	pthread_mutex_lock(&lock_archivos);
	cantidad = 0;
	memset(listado, 0, sizeof(listado));
	generacion++;
	pthread_mutex_unlock(&lock_archivos);
}

static int comparar_seq_asc(const void *a, const void *b)
{
	const struct archivo_analizado *const *pa = a;
	const struct archivo_analizado *const *pb = b;

	if ((*pa)->seq < (*pb)->seq)
		return -1;
	if ((*pa)->seq > (*pb)->seq)
		return 1;
	return 0;
}

static int comparar_seq_desc(const void *a, const void *b)
{
	const struct archivo_analizado *const *pa = a;
	const struct archivo_analizado *const *pb = b;

	if ((*pa)->seq > (*pb)->seq)
		return -1;
	if ((*pa)->seq < (*pb)->seq)
		return 1;
	return 0;
}

size_t analisis_desde(unsigned long ultimo_s, struct archivo_analizado *destino, size_t max)
{
	const struct archivo_analizado *ptrs[MAX_ARCHIVOS];
	size_t n = 0;
	size_t copiar;

	if (!destino || max == 0)
		return 0;

	pthread_mutex_lock(&lock_archivos);
	for (size_t i = 0; i < cantidad; i++) {
		if (listado[i].seq > ultimo_s)
			ptrs[n++] = &listado[i];
	}
	if (n > 1)
		qsort(ptrs, n, sizeof(ptrs[0]), comparar_seq_asc);

	copiar = (n < max) ? n : max;
	for (size_t i = 0; i < copiar; i++)
		destino[i] = *ptrs[i];
	pthread_mutex_unlock(&lock_archivos);

	return copiar;
}

size_t analisis_listado(struct archivo_analizado *destino, size_t max)
{
	const struct archivo_analizado *ptrs[MAX_ARCHIVOS];
	size_t n = 0;
	size_t copiar;

	if (!destino || max == 0)
		return 0;

	pthread_mutex_lock(&lock_archivos);
	for (size_t i = 0; i < cantidad; i++)
		ptrs[n++] = &listado[i];
	if (n > 1)
		qsort(ptrs, n, sizeof(ptrs[0]), comparar_seq_desc);

	copiar = (n < max) ? n : max;
	for (size_t i = 0; i < copiar; i++)
		destino[i] = *ptrs[i];
	pthread_mutex_unlock(&lock_archivos);

	return copiar;
}

size_t analisis_amenazas(struct amenaza *destino, size_t max)
{
	size_t n = 0;

	pthread_mutex_lock(&lock_archivos);
	if (total_amenazas > 0 && max > 0) {
		unsigned long inicio = (total_amenazas > MAX_AMENAZAS) ? (total_amenazas - MAX_AMENAZAS + 1) : 1;
		unsigned long cur = total_amenazas;
		while (cur >= inicio && n < max) {
			destino[n++] = amenazas[(cur - 1) % MAX_AMENAZAS];
			if (cur == inicio)
				break;
			cur--;
		}
	}
	pthread_mutex_unlock(&lock_archivos);
	return n;
}

size_t analisis_amenazas_desde(unsigned long ultimo_id, struct amenaza *destino, size_t max)
{
	size_t n = 0;

	pthread_mutex_lock(&lock_archivos);
	if (total_amenazas > 0 && max > 0) {
		unsigned long inicio = (total_amenazas > MAX_AMENAZAS) ? (total_amenazas - MAX_AMENAZAS + 1) : 1;
		unsigned long desde = (ultimo_id < inicio) ? inicio : (ultimo_id + 1);
		while (desde <= total_amenazas && n < max) {
			destino[n++] = amenazas[(desde - 1) % MAX_AMENAZAS];
			desde++;
		}
	}
	pthread_mutex_unlock(&lock_archivos);
	return n;
}

size_t analisis_cuarentena(struct registro_cuarentena *destino, size_t max)
{
	size_t n = 0;

	pthread_mutex_lock(&lock_archivos);
	if (total_cuarentenas > 0 && max > 0) {
		unsigned long inicio = (total_cuarentenas > MAX_CUARENTENA) ? (total_cuarentenas - MAX_CUARENTENA + 1) : 1;
		unsigned long cur = total_cuarentenas;
		while (cur >= inicio && n < max) {
			destino[n++] = cuarentenas[(cur - 1) % MAX_CUARENTENA];
			if (cur == inicio)
				break;
			cur--;
		}
	}
	pthread_mutex_unlock(&lock_archivos);
	return n;
}

static int analizar_abierto(int fd, const struct stat *st, const char *ruta, const char *sha,
			    enum contexto_analisis ctx, unsigned int pid, const char *proceso,
			    unsigned long gen, enum resultado_analisis *res_out)
{
	struct firma f;
	int infectado;
	int ya_cuarentena;
	int fchmod_exitoso = 0;
	char permisos_ant[8] = "";
	int cuarentena;
	int debe_emitir_amenaza = 0;
	int debe_emitir_cuarentena = 0;
	int cambio = 0;
	int es_creado = 0;
	time_t ahora;
	struct tm tm_info;
	int encontrado = 0;
	size_t idx = 0;
	int ya_infectada_mismo_sha = 0;

	memset(&f, 0, sizeof(f));
	infectado = blacklist_buscar(sha, &f);
	ya_cuarentena = ((st->st_mode & 07777) == 0);
	snprintf(permisos_ant, sizeof(permisos_ant), "%o", (unsigned int)(st->st_mode & 07777));
	cuarentena = ya_cuarentena ? 1 : 0;

	pthread_mutex_lock(&lock_archivos);
	if (generacion != gen) {
		pthread_mutex_unlock(&lock_archivos);
		if (res_out)
			*res_out = ANALISIS_IGNORADO;
		return -1;
	}
	pthread_mutex_unlock(&lock_archivos);

	if (infectado && !ya_cuarentena) {
		if (fd >= 0 && fchmod(fd, 0) == 0) {
			cuarentena = 1;
			fchmod_exitoso = 1;
		} else {
			if (fd >= 0)
				registrar("fchmod fallido en %s: %s", ruta, strerror(errno));
			cuarentena = 0;
		}
	} else if (ya_cuarentena) {
		cuarentena = 1;
	}

	ahora = time(NULL);
	localtime_r(&ahora, &tm_info);

	pthread_mutex_lock(&lock_archivos);
	if (generacion != gen) {
		if (fchmod_exitoso) {
			total_cuarentenas++;
			struct registro_cuarentena *rc = &cuarentenas[(total_cuarentenas - 1) % MAX_CUARENTENA];
			snprintf(rc->ruta, sizeof(rc->ruta), "%s", ruta);
			snprintf(rc->sha256, sizeof(rc->sha256), "%s", sha);
			snprintf(rc->tipo, sizeof(rc->tipo), "automatica");
			snprintf(rc->motivo, sizeof(rc->motivo), "%s", f.nombre);
			rc->usuario[0] = '\0';
			strftime(rc->fecha, sizeof(rc->fecha), "%Y-%m-%d %H:%M:%S", &tm_info);
			snprintf(rc->permisos_anteriores, sizeof(rc->permisos_anteriores), "%s", permisos_ant);
			debe_emitir_cuarentena = 1;
		}
		pthread_mutex_unlock(&lock_archivos);
		if (debe_emitir_cuarentena) {
			alerta_emitir(MOT_ARCHIVO_CUARENTENA,
				      "Archivo %s puesto en cuarentena automática (firma %s)",
				      ruta, f.nombre);
		}
		if (res_out)
			*res_out = ANALISIS_IGNORADO;
		return -1;
	}

	for (size_t i = 0; i < cantidad; i++) {
		if (strcmp(listado[i].ruta, ruta) == 0) {
			encontrado = 1;
			idx = i;
			break;
		}
	}

	if (encontrado && listado[idx].estado == ARCHIVO_INFECTADO && strcmp(listado[idx].sha256, sha) == 0)
		ya_infectada_mismo_sha = 1;

	if (encontrado) {
		if (strcmp(listado[idx].sha256, sha) != 0 ||
		    listado[idx].mtime.tv_sec != st->st_mtim.tv_sec ||
		    listado[idx].mtime.tv_nsec != st->st_mtim.tv_nsec ||
		    listado[idx].estado != (infectado ? ARCHIVO_INFECTADO : ARCHIVO_BENIGNO) ||
		    listado[idx].cuarentena != cuarentena) {
			cambio = 1;
		}
	} else {
		cambio = 1;
		es_creado = 1;
		if (cantidad < MAX_ARCHIVOS) {
			idx = cantidad++;
		} else {
			size_t min_idx = 0;
			unsigned long min_seq = listado[0].seq;
			for (size_t i = 1; i < MAX_ARCHIVOS; i++) {
				if (listado[i].seq < min_seq) {
					min_seq = listado[i].seq;
					min_idx = i;
				}
			}
			idx = min_idx;
		}
	}

	if (cambio) {
		listado[idx].seq = ++ultimo_seq;
		snprintf(listado[idx].ruta, sizeof(listado[idx].ruta), "%s", ruta);
		snprintf(listado[idx].sha256, sizeof(listado[idx].sha256), "%s", sha);
		listado[idx].tamano = (long long)st->st_size;
		listado[idx].mtime = st->st_mtim;
		listado[idx].estado = infectado ? ARCHIVO_INFECTADO : ARCHIVO_BENIGNO;
		listado[idx].cuarentena = cuarentena;
		if (infectado)
			snprintf(listado[idx].firma, sizeof(listado[idx].firma), "%s", f.nombre);
		else
			listado[idx].firma[0] = '\0';

		if (ctx == CTX_EVENTO) {
			listado[idx].accion = es_creado ? ACCION_CREADO : ACCION_MODIFICADO;
			listado[idx].pid = pid;
			snprintf(listado[idx].proceso, sizeof(listado[idx].proceso), "%s", proceso);
		} else if (ctx == CTX_INICIAL) {
			listado[idx].accion = ACCION_INICIAL;
			listado[idx].pid = 0;
			listado[idx].proceso[0] = '\0';
		}
		strftime(listado[idx].analizado, sizeof(listado[idx].analizado), "%Y-%m-%d %H:%M:%S", &tm_info);
	}

	if (infectado && !ya_infectada_mismo_sha) {
		total_amenazas++;
		struct amenaza *am = &amenazas[(total_amenazas - 1) % MAX_AMENAZAS];
		am->id = total_amenazas;
		snprintf(am->firma, sizeof(am->firma), "%s", f.nombre);
		snprintf(am->descripcion, sizeof(am->descripcion), "%s", f.descripcion);
		snprintf(am->ruta, sizeof(am->ruta), "%s", ruta);
		snprintf(am->sha256, sizeof(am->sha256), "%s", sha);
		am->pid = pid;
		snprintf(am->proceso, sizeof(am->proceso), "%s", proceso);
		strftime(am->fecha, sizeof(am->fecha), "%Y-%m-%d %H:%M:%S", &tm_info);
		am->cuarentena = cuarentena;
		debe_emitir_amenaza = 1;
	}

	if (fchmod_exitoso) {
		total_cuarentenas++;
		struct registro_cuarentena *rc = &cuarentenas[(total_cuarentenas - 1) % MAX_CUARENTENA];
		snprintf(rc->ruta, sizeof(rc->ruta), "%s", ruta);
		snprintf(rc->sha256, sizeof(rc->sha256), "%s", sha);
		snprintf(rc->tipo, sizeof(rc->tipo), "automatica");
		snprintf(rc->motivo, sizeof(rc->motivo), "%s", f.nombre);
		rc->usuario[0] = '\0';
		strftime(rc->fecha, sizeof(rc->fecha), "%Y-%m-%d %H:%M:%S", &tm_info);
		snprintf(rc->permisos_anteriores, sizeof(rc->permisos_anteriores), "%s", permisos_ant);
		debe_emitir_cuarentena = 1;
	}

	pthread_mutex_unlock(&lock_archivos);

	if (debe_emitir_amenaza) {
		if (ctx == CTX_EVENTO)
			alerta_emitir(MOT_HASH_MALICIOSO, "Firma %s detectada en %s (PID %u, %s)",
				      f.nombre, ruta, pid, proceso);
		else if (ctx == CTX_INICIAL)
			alerta_emitir(MOT_HASH_MALICIOSO, "Firma %s detectada en %s (análisis inicial)",
				      f.nombre, ruta);
		else
			alerta_emitir(MOT_HASH_MALICIOSO, "Firma %s detectada en %s (actualización de blacklist)",
				      f.nombre, ruta);
	}

	if (debe_emitir_cuarentena) {
		alerta_emitir(MOT_ARCHIVO_CUARENTENA,
			      "Archivo %s puesto en cuarentena automática (firma %s)",
			      ruta, f.nombre);
	}

	if (!infectado && ctx == CTX_EVENTO && cambio) {
		alerta_emitir(MOT_ARCHIVO_MODIFICADO,
			      "El archivo %s fue %s por el PID %u (%s)",
			      ruta,
			      es_creado ? "creado" : "modificado",
			      pid, proceso);
	}

	if (cambio && fd_aviso >= 0)
		eventfd_write(fd_aviso, 1);

	if (res_out) {
		if (!cambio)
			*res_out = ANALISIS_SIN_CAMBIO;
		else if (es_creado)
			*res_out = ANALISIS_CREADO;
		else
			*res_out = ANALISIS_MODIFICADO;
	}

	return 0;
}

enum resultado_analisis analisis_procesar(const char *dir, unsigned long gen,
					  const struct evento_archivo *e)
{
	char ruta[SENSOR_RUTA_MAX];
	char dir_barra[PATH_MAX + 2];
	size_t dir_len;
	unsigned int largo;
	int es_rdonly;
	int crea_o_trunca;
	int fd;
	struct stat st;
	char sha[65];
	enum resultado_analisis res = ANALISIS_IGNORADO;

	if (!dir || !e)
		return ANALISIS_IGNORADO;

	memcpy(ruta, e->ruta, SENSOR_RUTA_MAX);
	ruta[SENSOR_RUTA_MAX - 1] = '\0';

	es_rdonly = ((e->flags & O_ACCMODE) == O_RDONLY);
	crea_o_trunca = ((e->flags & (O_CREAT | O_TRUNC)) != 0);
	if (es_rdonly && !crea_o_trunca)
		return ANALISIS_IGNORADO;

	dir_len = strlen(dir);
	if (dir_len == 0 || dir_len + 2 > sizeof(dir_barra))
		return ANALISIS_IGNORADO;

	snprintf(dir_barra, sizeof(dir_barra), "%s%s", dir, (dir[dir_len - 1] == '/') ? "" : "/");
	largo = (unsigned int)strlen(dir_barra);
	if (!ruta_en_directorio(ruta, dir_barra, largo))
		return ANALISIS_IGNORADO;

	fd = abrir_sin_enlaces(ruta, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
	if (fd < 0)
		return ANALISIS_IGNORADO;

	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return ANALISIS_IGNORADO;
	}

	if (analisis_sha256_fd(fd, sha) < 0) {
		close(fd);
		return ANALISIS_IGNORADO;
	}

	analizar_abierto(fd, &st, ruta, sha, CTX_EVENTO, e->pid, e->proceso, gen, &res);
	close(fd);

	return res;
}

size_t analisis_linea_base(const char *dir, unsigned long gen)
{
	DIR *d;
	struct dirent *de;
	size_t analizados = 0;

	if (!dir)
		return 0;

	d = opendir(dir);
	if (!d)
		return 0;

	while ((de = readdir(d)) != NULL) {
		char ruta[SENSOR_RUTA_MAX];
		int n;
		int fd;
		struct stat st;
		char sha[65];

		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		n = snprintf(ruta, sizeof(ruta), "%s/%s", dir, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(ruta))
			continue;

		fd = abrir_sin_enlaces(ruta, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
		if (fd < 0)
			continue;

		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
			close(fd);
			continue;
		}

		if (analisis_sha256_fd(fd, sha) < 0) {
			close(fd);
			continue;
		}

		if (analizar_abierto(fd, &st, ruta, sha, CTX_INICIAL, 0, "", gen, NULL) == 0)
			analizados++;

		close(fd);
	}

	closedir(d);
	return analizados;
}

size_t analisis_reevaluar(const char *dir, unsigned long gen)
{
	char rutas_copia[MAX_ARCHIVOS][SENSOR_RUTA_MAX];
	char shas_copia[MAX_ARCHIVOS][65];
	size_t n_rutas;
	size_t reevaluados = 0;
	char dir_barra[PATH_MAX + 2];
	size_t dir_len;
	unsigned int largo_dir;

	if (!dir)
		return 0;

	pthread_mutex_lock(&lock_archivos);
	if (generacion != gen) {
		pthread_mutex_unlock(&lock_archivos);
		return 0;
	}
	n_rutas = cantidad;
	for (size_t i = 0; i < n_rutas; i++) {
		memcpy(rutas_copia[i], listado[i].ruta, SENSOR_RUTA_MAX);
		memcpy(shas_copia[i], listado[i].sha256, 65);
	}
	pthread_mutex_unlock(&lock_archivos);

	dir_len = strlen(dir);
	if (dir_len == 0 || dir_len + 2 > sizeof(dir_barra))
		return 0;

	snprintf(dir_barra, sizeof(dir_barra), "%s%s", dir, (dir[dir_len - 1] == '/') ? "" : "/");
	largo_dir = (unsigned int)strlen(dir_barra);

	for (size_t i = 0; i < n_rutas; i++) {
		if (!ruta_en_directorio(rutas_copia[i], dir_barra, largo_dir))
			continue;

		int fd = abrir_sin_enlaces(rutas_copia[i], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
		struct stat st;
		char sha[65];

		if (fd >= 0) {
			if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
				if (analisis_sha256_fd(fd, sha) == 0) {
					analizar_abierto(fd, &st, rutas_copia[i], sha, CTX_REEVALUACION, 0, "", gen, NULL);
					reevaluados++;
				}
			}
			close(fd);
		} else if (errno == EACCES) {
			int pfd = abrir_sin_enlaces(rutas_copia[i], O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
			if (pfd >= 0) {
				if (fstat(pfd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0) {
					analizar_abierto(-1, &st, rutas_copia[i], shas_copia[i], CTX_REEVALUACION, 0, "", gen, NULL);
					reevaluados++;
				}
				close(pfd);
			}
		}
	}

	return reevaluados;
}

enum resultado_cuarentena analisis_cuarentena_manual(const char *ruta, const char *usuario)
{
	char dir[PATH_MAX];
	char dir_barra[PATH_MAX + 2];
	size_t dir_len;
	unsigned int largo_dir;
	int encontrado = 0;
	int fd;
	struct stat st;
	char permisos_ant[8] = "";
	char sha_registro[65] = "";

	if (!ruta)
		return CUARENTENA_NO_LISTADO;

	pthread_mutex_lock(&lock_archivos);
	snprintf(dir, sizeof(dir), "%s", publicado.ruta);
	for (size_t i = 0; i < cantidad; i++) {
		if (strcmp(listado[i].ruta, ruta) == 0) {
			encontrado = 1;
			break;
		}
	}
	pthread_mutex_unlock(&lock_archivos);

	if (!encontrado)
		return CUARENTENA_NO_LISTADO;

	dir_len = strlen(dir);
	if (dir_len == 0 || dir_len + 2 > sizeof(dir_barra))
		return CUARENTENA_NO_LISTADO;

	snprintf(dir_barra, sizeof(dir_barra), "%s%s", dir, (dir[dir_len - 1] == '/') ? "" : "/");
	largo_dir = (unsigned int)strlen(dir_barra);
	if (!ruta_en_directorio(ruta, dir_barra, largo_dir))
		return CUARENTENA_NO_LISTADO;

	fd = abrir_sin_enlaces(ruta, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0);
	if (fd < 0) {
		if (errno == EACCES) {
			int pfd = abrir_sin_enlaces(ruta, O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
			if (pfd >= 0) {
				int ya = (fstat(pfd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0);
				close(pfd);
				if (ya)
					return CUARENTENA_YA;
			}
		}
		return CUARENTENA_NO_LISTADO;
	}

	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return CUARENTENA_NO_LISTADO;
	}

	if ((st.st_mode & 07777) == 0) {
		close(fd);
		return CUARENTENA_YA;
	}

	snprintf(permisos_ant, sizeof(permisos_ant), "%o", (unsigned int)(st.st_mode & 07777));

	if (fchmod(fd, 0) < 0) {
		close(fd);
		return CUARENTENA_ERROR;
	}
	close(fd);

	time_t ahora = time(NULL);
	struct tm tm_info;
	localtime_r(&ahora, &tm_info);

	pthread_mutex_lock(&lock_archivos);
	for (size_t i = 0; i < cantidad; i++) {
		if (strcmp(listado[i].ruta, ruta) == 0) {
			listado[i].cuarentena = 1;
			listado[i].seq = ++ultimo_seq;
			snprintf(sha_registro, sizeof(sha_registro), "%s", listado[i].sha256);
			break;
		}
	}

	total_cuarentenas++;
	struct registro_cuarentena *rc = &cuarentenas[(total_cuarentenas - 1) % MAX_CUARENTENA];
	snprintf(rc->ruta, sizeof(rc->ruta), "%s", ruta);
	snprintf(rc->sha256, sizeof(rc->sha256), "%s", sha_registro);
	snprintf(rc->tipo, sizeof(rc->tipo), "manual");
	snprintf(rc->motivo, sizeof(rc->motivo), "manual");
	if (usuario)
		snprintf(rc->usuario, sizeof(rc->usuario), "%s", usuario);
	else
		rc->usuario[0] = '\0';
	strftime(rc->fecha, sizeof(rc->fecha), "%Y-%m-%d %H:%M:%S", &tm_info);
	snprintf(rc->permisos_anteriores, sizeof(rc->permisos_anteriores), "%s", permisos_ant);

	for (size_t i = 0; i < total_amenazas && i < MAX_AMENAZAS; i++) {
		if (strcmp(amenazas[i].ruta, ruta) == 0)
			amenazas[i].cuarentena = 1;
	}
	pthread_mutex_unlock(&lock_archivos);

	alerta_emitir(MOT_ARCHIVO_CUARENTENA,
		      "Archivo %s puesto en cuarentena manual por %s",
		      ruta, usuario ? usuario : "");

	if (fd_aviso >= 0)
		eventfd_write(fd_aviso, 1);

	return CUARENTENA_OK;
}
