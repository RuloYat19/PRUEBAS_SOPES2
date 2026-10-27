#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"
#include "lib/cJSON.h"

struct tabla_firmas {
	struct firma *entradas;
	unsigned int *ranuras;
	size_t n;
	size_t capacidad;
};

static pthread_mutex_t lock_blacklist = PTHREAD_MUTEX_INITIALIZER;
static struct tabla_firmas *vigente = NULL;
static struct info_blacklist info_global;
static unsigned long version_global = 0;
static char ruta_configurada[PATH_MAX] = "";

static uint64_t hash_sha(const char *sha)
{
	char buf[17];
	memcpy(buf, sha, 16);
	buf[16] = '\0';
	return (uint64_t)strtoull(buf, NULL, 16);
}

void tabla_firmas_liberar(struct tabla_firmas *t)
{
	if (!t)
		return;
	free(t->entradas);
	free(t->ranuras);
	free(t);
}

static struct tabla_firmas *tabla_firmas_crear(size_t n)
{
	struct tabla_firmas *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->n = n;
	size_t cap = 16;
	while (cap < 2 * n)
		cap <<= 1;
	t->capacidad = cap;
	t->ranuras = calloc(cap, sizeof(unsigned int));
	if (n > 0)
		t->entradas = calloc(n, sizeof(struct firma));
	if ((n > 0 && !t->entradas) || !t->ranuras) {
		tabla_firmas_liberar(t);
		return NULL;
	}
	return t;
}

int blacklist_validar(const char *json, size_t largo, struct tabla_firmas **tabla,
		      struct info_blacklist *info, char *error, size_t tam_error)
{
	if (!json || largo == 0) {
		snprintf(error, tam_error, "JSON mal formado");
		return -1;
	}

	cJSON *root = cJSON_ParseWithLength(json, largo);
	if (!root) {
		snprintf(error, tam_error, "JSON mal formado");
		return -1;
	}

	if (!cJSON_IsObject(root)) {
		snprintf(error, tam_error, "La raiz debe ser un objeto");
		cJSON_Delete(root);
		return -1;
	}

	int es_diccionario = 0;
	if (!cJSON_GetObjectItem(root, "firmas") && !cJSON_GetObjectItem(root, "nombre") &&
	    !cJSON_GetObjectItem(root, "version") && !cJSON_GetObjectItem(root, "actualizado")) {
		int total = cJSON_GetArraySize(root);
		if (total >= 1 && total <= 5000) {
			int todos_objetos = 1;
			cJSON *elem = NULL;
			cJSON_ArrayForEach(elem, root) {
				if (!elem->string || !cJSON_IsObject(elem)) {
					todos_objetos = 0;
					break;
				}
				cJSON *h = cJSON_GetObjectItem(elem, "hash");
				if (!h)
					h = cJSON_GetObjectItem(elem, "sha256");
				if (!h || !cJSON_IsString(h) || !h->valuestring || strlen(h->valuestring) != 64) {
					todos_objetos = 0;
					break;
				}
			}
			if (todos_objetos)
				es_diccionario = 1;
		}
	}

	if (es_diccionario) {
		int num_firmas = cJSON_GetArraySize(root);
		struct tabla_firmas *t = tabla_firmas_crear((size_t)num_firmas);
		if (!t) {
			snprintf(error, tam_error, "Memoria insuficiente");
			cJSON_Delete(root);
			return -1;
		}

		cJSON *elem = NULL;
		int i = 0;
		cJSON_ArrayForEach(elem, root) {
			const char *nom = elem->string;
			size_t nom_len = strlen(nom);
			if (nom_len < 1 || nom_len > 64) {
				snprintf(error, tam_error, "Firma %d: nombre invalido", i + 1);
				tabla_firmas_liberar(t);
				cJSON_Delete(root);
				return -1;
			}

			cJSON *h = cJSON_GetObjectItem(elem, "hash");
			if (!h)
				h = cJSON_GetObjectItem(elem, "sha256");
			for (int j = 0; j < 64; j++) {
				if (!isxdigit((unsigned char)h->valuestring[j])) {
					snprintf(error, tam_error, "Firma %d: sha256 invalido", i + 1);
					tabla_firmas_liberar(t);
					cJSON_Delete(root);
					return -1;
				}
			}

			cJSON *d = cJSON_GetObjectItem(elem, "descripcion");
			const char *desc = nom;
			if (d && cJSON_IsString(d) && d->valuestring && strlen(d->valuestring) > 0)
				desc = d->valuestring;

			char sha_lower[65];
			for (int j = 0; j < 64; j++)
				sha_lower[j] = (char)tolower((unsigned char)h->valuestring[j]);
			sha_lower[64] = '\0';

			uint64_t hash_val = hash_sha(sha_lower);
			size_t idx = (size_t)(hash_val & (t->capacidad - 1));
			while (t->ranuras[idx] != 0) {
				size_t e_idx = (size_t)(t->ranuras[idx] - 1);
				if (strcmp(t->entradas[e_idx].sha256, sha_lower) == 0) {
					snprintf(error, tam_error, "Firma %d: sha256 duplicado", i + 1);
					tabla_firmas_liberar(t);
					cJSON_Delete(root);
					return -1;
				}
				idx = (idx + 1) & (t->capacidad - 1);
			}

			memcpy(t->entradas[i].sha256, sha_lower, 65);
			snprintf(t->entradas[i].nombre, sizeof(t->entradas[i].nombre), "%s", nom);
			snprintf(t->entradas[i].descripcion, sizeof(t->entradas[i].descripcion), "%s", desc);
			t->ranuras[idx] = (unsigned int)(i + 1);
			i++;
		}

		if (info) {
			memset(info, 0, sizeof(*info));
			snprintf(info->nombre, sizeof(info->nombre), "Base local de firmas SO2");
			info->version = 1;
			snprintf(info->actualizado, sizeof(info->actualizado), "2026-10-25");
			info->firmas = (size_t)num_firmas;
		}

		if (tabla)
			*tabla = t;
		else
			tabla_firmas_liberar(t);

		cJSON_Delete(root);
		return 0;
	}

	int visto_nombre = 0, visto_version = 0, visto_actualizado = 0, visto_firmas = 0;
	cJSON *nombre_nodo = NULL, *version_nodo = NULL, *actualizado_nodo = NULL, *firmas_nodo = NULL;
	cJSON *hijo = NULL;

	cJSON_ArrayForEach(hijo, root) {
		if (!hijo->string) {
			snprintf(error, tam_error, "Clave desconocida: ");
			cJSON_Delete(root);
			return -1;
		}
		if (strcmp(hijo->string, "nombre") == 0) {
			if (visto_nombre) {
				snprintf(error, tam_error, "Clave desconocida: nombre");
				cJSON_Delete(root);
				return -1;
			}
			visto_nombre = 1;
			nombre_nodo = hijo;
		} else if (strcmp(hijo->string, "version") == 0) {
			if (visto_version) {
				snprintf(error, tam_error, "Clave desconocida: version");
				cJSON_Delete(root);
				return -1;
			}
			visto_version = 1;
			version_nodo = hijo;
		} else if (strcmp(hijo->string, "actualizado") == 0) {
			if (visto_actualizado) {
				snprintf(error, tam_error, "Clave desconocida: actualizado");
				cJSON_Delete(root);
				return -1;
			}
			visto_actualizado = 1;
			actualizado_nodo = hijo;
		} else if (strcmp(hijo->string, "firmas") == 0) {
			if (visto_firmas) {
				snprintf(error, tam_error, "Clave desconocida: firmas");
				cJSON_Delete(root);
				return -1;
			}
			visto_firmas = 1;
			firmas_nodo = hijo;
		} else {
			snprintf(error, tam_error, "Clave desconocida: %s", hijo->string);
			cJSON_Delete(root);
			return -1;
		}
	}

	if (!visto_firmas) {
		snprintf(error, tam_error, "Falta el arreglo firmas");
		cJSON_Delete(root);
		return -1;
	}

	if (!cJSON_IsArray(firmas_nodo)) {
		snprintf(error, tam_error, "firmas debe tener entre 1 y 5000 elementos");
		cJSON_Delete(root);
		return -1;
	}

	int num_firmas = cJSON_GetArraySize(firmas_nodo);
	if (num_firmas < 1 || num_firmas > 5000) {
		snprintf(error, tam_error, "firmas debe tener entre 1 y 5000 elementos");
		cJSON_Delete(root);
		return -1;
	}

	if (nombre_nodo) {
		if (!cJSON_IsString(nombre_nodo) || !nombre_nodo->valuestring) {
			snprintf(error, tam_error, "Campo invalido: nombre");
			cJSON_Delete(root);
			return -1;
		}
		size_t len = strlen(nombre_nodo->valuestring);
		if (len < 1 || len > 64) {
			snprintf(error, tam_error, "Campo invalido: nombre");
			cJSON_Delete(root);
			return -1;
		}
	}

	if (version_nodo) {
		if (!cJSON_IsNumber(version_nodo) || version_nodo->valuedouble < 1.0 ||
		    (double)version_nodo->valueint != version_nodo->valuedouble || version_nodo->valueint < 1) {
			snprintf(error, tam_error, "Campo invalido: version");
			cJSON_Delete(root);
			return -1;
		}
	}

	if (actualizado_nodo) {
		if (!cJSON_IsString(actualizado_nodo) || !actualizado_nodo->valuestring) {
			snprintf(error, tam_error, "Campo invalido: actualizado");
			cJSON_Delete(root);
			return -1;
		}
		size_t len = strlen(actualizado_nodo->valuestring);
		if (len < 1 || len > 32) {
			snprintf(error, tam_error, "Campo invalido: actualizado");
			cJSON_Delete(root);
			return -1;
		}
	}

	struct tabla_firmas *t = tabla_firmas_crear((size_t)num_firmas);
	if (!t) {
		snprintf(error, tam_error, "Memoria insuficiente");
		cJSON_Delete(root);
		return -1;
	}

	for (int i = 0; i < num_firmas; i++) {
		cJSON *fitem = cJSON_GetArrayItem(firmas_nodo, i);
		if (!fitem || !cJSON_IsObject(fitem)) {
			snprintf(error, tam_error, "Firma %d: falta sha256", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}

		int visto_f_sha = 0, visto_f_nom = 0, visto_f_desc = 0;
		cJSON *f_sha = NULL, *f_nom = NULL, *f_desc = NULL;
		cJSON *fhijo = NULL;

		cJSON_ArrayForEach(fhijo, fitem) {
			if (!fhijo->string) {
				snprintf(error, tam_error, "Firma %d: clave desconocida ", i + 1);
				tabla_firmas_liberar(t);
				cJSON_Delete(root);
				return -1;
			}
			if (strcmp(fhijo->string, "sha256") == 0) {
				if (visto_f_sha) {
					snprintf(error, tam_error, "Firma %d: clave desconocida sha256", i + 1);
					tabla_firmas_liberar(t);
					cJSON_Delete(root);
					return -1;
				}
				visto_f_sha = 1;
				f_sha = fhijo;
			} else if (strcmp(fhijo->string, "nombre") == 0) {
				if (visto_f_nom) {
					snprintf(error, tam_error, "Firma %d: clave desconocida nombre", i + 1);
					tabla_firmas_liberar(t);
					cJSON_Delete(root);
					return -1;
				}
				visto_f_nom = 1;
				f_nom = fhijo;
			} else if (strcmp(fhijo->string, "descripcion") == 0) {
				if (visto_f_desc) {
					snprintf(error, tam_error, "Firma %d: clave desconocida descripcion", i + 1);
					tabla_firmas_liberar(t);
					cJSON_Delete(root);
					return -1;
				}
				visto_f_desc = 1;
				f_desc = fhijo;
			} else {
				snprintf(error, tam_error, "Firma %d: clave desconocida %s", i + 1, fhijo->string);
				tabla_firmas_liberar(t);
				cJSON_Delete(root);
				return -1;
			}
		}

		if (!visto_f_sha) {
			snprintf(error, tam_error, "Firma %d: falta sha256", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}
		if (!visto_f_nom) {
			snprintf(error, tam_error, "Firma %d: falta nombre", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}
		if (!visto_f_desc) {
			snprintf(error, tam_error, "Firma %d: falta descripcion", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}

		if (!cJSON_IsString(f_sha) || !f_sha->valuestring || strlen(f_sha->valuestring) != 64) {
			snprintf(error, tam_error, "Firma %d: sha256 invalido", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}
		for (int j = 0; j < 64; j++) {
			if (!isxdigit((unsigned char)f_sha->valuestring[j])) {
				snprintf(error, tam_error, "Firma %d: sha256 invalido", i + 1);
				tabla_firmas_liberar(t);
				cJSON_Delete(root);
				return -1;
			}
		}

		if (!cJSON_IsString(f_nom) || !f_nom->valuestring) {
			snprintf(error, tam_error, "Firma %d: nombre invalido", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}
		size_t nom_len = strlen(f_nom->valuestring);
		if (nom_len < 1 || nom_len > 64) {
			snprintf(error, tam_error, "Firma %d: nombre invalido", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}

		if (!cJSON_IsString(f_desc) || !f_desc->valuestring) {
			snprintf(error, tam_error, "Firma %d: descripcion invalida", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}
		size_t desc_len = strlen(f_desc->valuestring);
		if (desc_len < 1 || desc_len > 256) {
			snprintf(error, tam_error, "Firma %d: descripcion invalida", i + 1);
			tabla_firmas_liberar(t);
			cJSON_Delete(root);
			return -1;
		}

		char sha_lower[65];
		for (int j = 0; j < 64; j++)
			sha_lower[j] = (char)tolower((unsigned char)f_sha->valuestring[j]);
		sha_lower[64] = '\0';

		uint64_t h = hash_sha(sha_lower);
		size_t idx = (size_t)(h & (t->capacidad - 1));
		while (t->ranuras[idx] != 0) {
			size_t e_idx = (size_t)(t->ranuras[idx] - 1);
			if (strcmp(t->entradas[e_idx].sha256, sha_lower) == 0) {
				snprintf(error, tam_error, "Firma %d: sha256 duplicado", i + 1);
				tabla_firmas_liberar(t);
				cJSON_Delete(root);
				return -1;
			}
			idx = (idx + 1) & (t->capacidad - 1);
		}

		memcpy(t->entradas[i].sha256, sha_lower, 65);
		snprintf(t->entradas[i].nombre, sizeof(t->entradas[i].nombre), "%s", f_nom->valuestring);
		snprintf(t->entradas[i].descripcion, sizeof(t->entradas[i].descripcion), "%s", f_desc->valuestring);
		t->ranuras[idx] = (unsigned int)(i + 1);
	}

	if (info) {
		memset(info, 0, sizeof(*info));
		if (nombre_nodo && nombre_nodo->valuestring)
			snprintf(info->nombre, sizeof(info->nombre), "%s", nombre_nodo->valuestring);
		if (version_nodo)
			info->version = version_nodo->valueint;
		if (actualizado_nodo && actualizado_nodo->valuestring)
			snprintf(info->actualizado, sizeof(info->actualizado), "%s", actualizado_nodo->valuestring);
		info->firmas = (size_t)num_firmas;
	}

	if (tabla)
		*tabla = t;
	else
		tabla_firmas_liberar(t);

	cJSON_Delete(root);
	if (error && tam_error > 0)
		error[0] = '\0';
	return 0;
}

static int guardar_atomico(const char *ruta, const void *datos, size_t largo)
{
	char dir[PATH_MAX];
	char plantilla[PATH_MAX + 32];
	const char *barra = strrchr(ruta, '/');
	if (barra) {
		size_t dlen = (size_t)(barra - ruta);
		if (dlen == 0)
			snprintf(dir, sizeof(dir), "/");
		else
			snprintf(dir, sizeof(dir), "%.*s", (int)dlen, ruta);
	} else {
		snprintf(dir, sizeof(dir), ".");
	}
	snprintf(plantilla, sizeof(plantilla), "%s/.blacklist.XXXXXX", dir);
	int fd = mkstemp(plantilla);
	if (fd < 0)
		return -1;

	size_t escrito = 0;
	while (escrito < largo) {
		ssize_t n = write(fd, (const char *)datos + escrito, largo - escrito);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(plantilla);
			return -1;
		}
		escrito += (size_t)n;
	}
	if (fsync(fd) < 0) {
		close(fd);
		unlink(plantilla);
		return -1;
	}

	struct stat st;
	if (stat(ruta, &st) == 0) {
		if (fchown(fd, st.st_uid, st.st_gid) < 0) {
		}
		fchmod(fd, st.st_mode & 07777);
	} else {
		fchmod(fd, 0644);
	}
	close(fd);

	if (rename(plantilla, ruta) < 0) {
		unlink(plantilla);
		return -1;
	}
	return 0;
}

int blacklist_iniciar(const char *ruta)
{
	pthread_mutex_lock(&lock_blacklist);
	if (ruta)
		snprintf(ruta_configurada, sizeof(ruta_configurada), "%s", ruta);
	else
		ruta_configurada[0] = '\0';
	memset(&info_global, 0, sizeof(info_global));
	version_global = 1;
	pthread_mutex_unlock(&lock_blacklist);

	struct stat st;
	if (stat(ruta_configurada, &st) < 0) {
		char reg[256];
		pthread_mutex_lock(&lock_blacklist);
		snprintf(info_global.error, sizeof(info_global.error), "%s: %s",
			 ruta_configurada, strerror(errno));
		snprintf(reg, sizeof(reg), "%s", info_global.error);
		pthread_mutex_unlock(&lock_blacklist);
		registrar("blacklist no cargada: %s", reg);
		return 0;
	}
	if (!S_ISREG(st.st_mode)) {
		char reg[256];
		pthread_mutex_lock(&lock_blacklist);
		snprintf(info_global.error, sizeof(info_global.error), "%s: No es un archivo regular",
			 ruta_configurada);
		snprintf(reg, sizeof(reg), "%s", info_global.error);
		pthread_mutex_unlock(&lock_blacklist);
		registrar("blacklist no cargada: %s", reg);
		return 0;
	}

	if (st.st_size > MAX_CUERPO_BLACKLIST) {
		char reg[256];
		pthread_mutex_lock(&lock_blacklist);
		snprintf(info_global.error, sizeof(info_global.error), "%s: Archivo demasiado grande",
			 ruta_configurada);
		snprintf(reg, sizeof(reg), "%s", info_global.error);
		pthread_mutex_unlock(&lock_blacklist);
		registrar("blacklist no cargada: %s", reg);
		return 0;
	}

	int fd = open(ruta_configurada, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		char reg[256];
		pthread_mutex_lock(&lock_blacklist);
		snprintf(info_global.error, sizeof(info_global.error), "%s: %s",
			 ruta_configurada, strerror(errno));
		snprintf(reg, sizeof(reg), "%s", info_global.error);
		pthread_mutex_unlock(&lock_blacklist);
		registrar("blacklist no cargada: %s", reg);
		return 0;
	}

	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		close(fd);
		char reg[256];
		pthread_mutex_lock(&lock_blacklist);
		snprintf(info_global.error, sizeof(info_global.error), "%s: Memoria insuficiente",
			 ruta_configurada);
		snprintf(reg, sizeof(reg), "%s", info_global.error);
		pthread_mutex_unlock(&lock_blacklist);
		registrar("blacklist no cargada: %s", reg);
		return 0;
	}

	ssize_t leidos = 0;
	while (leidos < st.st_size) {
		ssize_t n = read(fd, buf + leidos, (size_t)st.st_size - (size_t)leidos);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		leidos += n;
	}
	close(fd);
	buf[leidos] = '\0';

	struct tabla_firmas *nueva = NULL;
	struct info_blacklist info_leida;
	memset(&info_leida, 0, sizeof(info_leida));
	char err[160] = "";

	if (blacklist_validar(buf, (size_t)leidos, &nueva, &info_leida, err, sizeof(err)) < 0) {
		free(buf);
		char reg[256];
		pthread_mutex_lock(&lock_blacklist);
		snprintf(info_global.error, sizeof(info_global.error), "%s: %s",
			 ruta_configurada, err);
		snprintf(reg, sizeof(reg), "%s", info_global.error);
		pthread_mutex_unlock(&lock_blacklist);
		registrar("blacklist no cargada: %s", reg);
		return 0;
	}
	free(buf);

	time_t ahora = time(NULL);
	struct tm tm_info;
	localtime_r(&ahora, &tm_info);

	pthread_mutex_lock(&lock_blacklist);
	tabla_firmas_liberar(vigente);
	vigente = nueva;
	memcpy(&info_global, &info_leida, sizeof(info_global));
	snprintf(info_global.origen, sizeof(info_global.origen), "archivo");
	strftime(info_global.cargada, sizeof(info_global.cargada), "%Y-%m-%d %H:%M:%S", &tm_info);
	info_global.error[0] = '\0';
	version_global = 1;
	pthread_mutex_unlock(&lock_blacklist);

	return 0;
}

int blacklist_actualizar(const char *json, size_t largo, const char *usuario,
			 size_t *firmas, char *error, size_t tam_error)
{
	struct tabla_firmas *nueva = NULL;
	struct info_blacklist info_nueva;
	memset(&info_nueva, 0, sizeof(info_nueva));

	if (blacklist_validar(json, largo, &nueva, &info_nueva, error, tam_error) < 0)
		return -1;

	char ruta_copia[PATH_MAX];
	pthread_mutex_lock(&lock_blacklist);
	snprintf(ruta_copia, sizeof(ruta_copia), "%s", ruta_configurada);
	pthread_mutex_unlock(&lock_blacklist);

	if (guardar_atomico(ruta_copia, json, largo) < 0) {
		tabla_firmas_liberar(nueva);
		snprintf(error, tam_error, "No se pudo guardar la blacklist");
		return -2;
	}

	struct tabla_firmas *vieja = NULL;
	time_t ahora = time(NULL);
	struct tm tm_info;
	localtime_r(&ahora, &tm_info);

	pthread_mutex_lock(&lock_blacklist);
	vieja = vigente;
	vigente = nueva;
	memcpy(&info_global, &info_nueva, sizeof(info_global));
	snprintf(info_global.origen, sizeof(info_global.origen), "subida");
	if (usuario)
		snprintf(info_global.actualizada_por, sizeof(info_global.actualizada_por), "%s", usuario);
	else
		info_global.actualizada_por[0] = '\0';
	strftime(info_global.cargada, sizeof(info_global.cargada), "%Y-%m-%d %H:%M:%S", &tm_info);
	info_global.error[0] = '\0';
	version_global++;
	if (firmas)
		*firmas = info_global.firmas;
	pthread_mutex_unlock(&lock_blacklist);

	tabla_firmas_liberar(vieja);
	return 0;
}

int blacklist_buscar(const char *sha256, struct firma *destino)
{
	if (!sha256 || strlen(sha256) != 64)
		return 0;

	char sha_lower[65];
	for (int j = 0; j < 64; j++)
		sha_lower[j] = (char)tolower((unsigned char)sha256[j]);
	sha_lower[64] = '\0';

	pthread_mutex_lock(&lock_blacklist);
	if (!vigente || vigente->n == 0 || !vigente->ranuras) {
		pthread_mutex_unlock(&lock_blacklist);
		return 0;
	}

	uint64_t h = hash_sha(sha_lower);
	size_t idx = (size_t)(h & (vigente->capacidad - 1));
	while (vigente->ranuras[idx] != 0) {
		size_t e_idx = (size_t)(vigente->ranuras[idx] - 1);
		if (strcmp(vigente->entradas[e_idx].sha256, sha_lower) == 0) {
			if (destino)
				memcpy(destino, &vigente->entradas[e_idx], sizeof(struct firma));
			pthread_mutex_unlock(&lock_blacklist);
			return 1;
		}
		idx = (idx + 1) & (vigente->capacidad - 1);
	}

	pthread_mutex_unlock(&lock_blacklist);
	return 0;
}

unsigned long blacklist_version(void)
{
	pthread_mutex_lock(&lock_blacklist);
	unsigned long v = version_global;
	pthread_mutex_unlock(&lock_blacklist);
	return v;
}

void blacklist_info(struct info_blacklist *destino)
{
	if (!destino)
		return;
	pthread_mutex_lock(&lock_blacklist);
	memcpy(destino, &info_global, sizeof(*destino));
	pthread_mutex_unlock(&lock_blacklist);
}

size_t blacklist_lista(struct firma *destino, size_t max)
{
	pthread_mutex_lock(&lock_blacklist);
	if (!vigente || !vigente->entradas || max == 0) {
		pthread_mutex_unlock(&lock_blacklist);
		return 0;
	}
	size_t copiar = vigente->n < max ? vigente->n : max;
	if (destino)
		memcpy(destino, vigente->entradas, copiar * sizeof(struct firma));
	pthread_mutex_unlock(&lock_blacklist);
	return copiar;
}

void blacklist_finalizar(void)
{
	pthread_mutex_lock(&lock_blacklist);
	tabla_firmas_liberar(vigente);
	vigente = NULL;
	memset(&info_global, 0, sizeof(info_global));
	version_global = 0;
	ruta_configurada[0] = '\0';
	pthread_mutex_unlock(&lock_blacklist);
}
