#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "comun.h"

static char base_dir[PATH_MAX] = ".";

static void copiar_cadena(char *destino, const char *origen, size_t tamano)
{
	size_t len;
	if (!destino || !origen || tamano == 0)
		return;
	len = strlen(origen);
	if (len >= tamano)
		len = tamano - 1;
	memcpy(destino, origen, len);
	destino[len] = '\0';
}

const char *obtener_home(void)
{
	if (geteuid() == 0) {
		const char *sudo_user = getenv("SUDO_USER");
		if (sudo_user && *sudo_user) {
			struct passwd *pw = getpwnam(sudo_user);
			if (pw && pw->pw_dir)
				return pw->pw_dir;
		}
	}
	const char *home = getenv("HOME");
	if (home && *home)
		return home;
	return ".";
}

static void resolver_ruta(char *destino, size_t tamano, const char *origen, const char *dir_base)
{
	char expandida[PATH_MAX * 2];
	const char *home;

	if (origen[0] == '~' && (origen[1] == '/' || origen[1] == '\0')) {
		home = obtener_home();
		if (origen[1] == '/')
			snprintf(expandida, sizeof(expandida), "%s%s", home, origen + 1);
		else
			snprintf(expandida, sizeof(expandida), "%s", home);
	} else if (origen[0] != '/' && dir_base && dir_base[0]) {
		snprintf(expandida, sizeof(expandida), "%s/%s", dir_base, origen);
	} else {
		snprintf(expandida, sizeof(expandida), "%s", origen);
	}

	char canonica[PATH_MAX];
	if (realpath(expandida, canonica))
		copiar_cadena(destino, canonica, tamano);
	else
		copiar_cadena(destino, expandida, tamano);
}

static char *recortar(char *s)
{
	char *fin;
	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	fin = s + strlen(s) - 1;
	while (fin > s && isspace((unsigned char)*fin))
		*fin-- = '\0';
	return s;
}

void config_por_defecto(struct configuracion *c, const char *dir_ejecutable)
{
	const char *home = obtener_home();

	if (dir_ejecutable && dir_ejecutable[0])
		copiar_cadena(base_dir, dir_ejecutable, sizeof(base_dir));
	else
		copiar_cadena(base_dir, ".", sizeof(base_dir));

	memset(c, 0, sizeof(*c));
	c->puerto = 8080;
	c->intervalo = 2;

	resolver_ruta(c->dashboard, sizeof(c->dashboard), "../Dashboard", base_dir);
	resolver_ruta(c->blacklist, sizeof(c->blacklist), "blacklist.json", base_dir);

	snprintf(c->log_central, sizeof(c->log_central), "%s/so2_logs/log_centralizado.txt", home);
	snprintf(c->ruta_vigilada, sizeof(c->ruta_vigilada), "%s/so2_vigilado", home);

	c->n_log_rutas = 2;
	snprintf(c->log_rutas[0], sizeof(c->log_rutas[0]), "%s/so2_logs/auth.log", home);
	snprintf(c->log_rutas[1], sizeof(c->log_rutas[1]), "%s/so2_logs/app.log", home);

	c->n_log_palabras = 2;
	copiar_cadena(c->log_palabras[0], "ERROR", sizeof(c->log_palabras[0]));
	copiar_cadena(c->log_palabras[1], "FAIL", sizeof(c->log_palabras[1]));
}

int config_cargar(struct configuracion *c, const char *archivo)
{
	struct stat st;
	FILE *f;
	char *linea = NULL;
	size_t capacidad = 0;
	ssize_t leidos;
	int n_linea = 0;

	if (!archivo)
		return -1;

	if (stat(archivo, &st) < 0 || !S_ISREG(st.st_mode))
		return -1;

	f = fopen(archivo, "r");
	if (!f)
		return -1;

	while ((leidos = getline(&linea, &capacidad, f)) != -1) {
		n_linea++;
		char *l = recortar(linea);
		if (*l == '\0' || *l == '#')
			continue;

		char *igual = strchr(l, '=');
		if (!igual) {
			registrar("%s:%d: linea sin '=' ignorada", archivo, n_linea);
			continue;
		}

		*igual = '\0';
		char *clave = recortar(l);
		char *valor = recortar(igual + 1);

		if (!strcmp(clave, "puerto")) {
			char *fin;
			errno = 0;
			long val = strtol(valor, &fin, 10);
			if (errno || *fin != '\0' || val < 1 || val > 65535)
				registrar("%s:%d: valor invalido para puerto '%s'", archivo, n_linea, valor);
			else
				c->puerto = (int)val;
		} else if (!strcmp(clave, "intervalo")) {
			char *fin;
			errno = 0;
			long val = strtol(valor, &fin, 10);
			if (errno || *fin != '\0' || val < 1 || val > 60)
				registrar("%s:%d: valor invalido para intervalo '%s'", archivo, n_linea, valor);
			else
				c->intervalo = (int)val;
		} else if (!strcmp(clave, "dashboard")) {
			resolver_ruta(c->dashboard, sizeof(c->dashboard), valor, base_dir);
		} else if (!strcmp(clave, "log_central")) {
			resolver_ruta(c->log_central, sizeof(c->log_central), valor, base_dir);
		} else if (!strcmp(clave, "ruta_vigilada")) {
			resolver_ruta(c->ruta_vigilada, sizeof(c->ruta_vigilada), valor, base_dir);
		} else if (!strcmp(clave, "blacklist")) {
			resolver_ruta(c->blacklist, sizeof(c->blacklist), valor, base_dir);
		} else if (!strcmp(clave, "log_rutas")) {
			char *token;
			char *resto = valor;
			int count = 0;
			char rutas_temp[MAX_RUTAS_LOG][PATH_MAX];
			while ((token = strtok_r(resto, ",", &resto)) != NULL) {
				char *item = recortar(token);
				if (*item == '\0')
					continue;
				if (count >= MAX_RUTAS_LOG) {
					registrar("%s:%d: exceso de elementos en log_rutas (max %d)", archivo, n_linea, MAX_RUTAS_LOG);
					break;
				}
				resolver_ruta(rutas_temp[count], sizeof(rutas_temp[count]), item, base_dir);
				count++;
			}
			if (count > 0) {
				c->n_log_rutas = count;
				for (int i = 0; i < count; i++)
					copiar_cadena(c->log_rutas[i], rutas_temp[i], sizeof(c->log_rutas[i]));
			}
		} else if (!strcmp(clave, "log_palabras")) {
			char *token;
			char *resto = valor;
			int count = 0;
			char palabras_temp[MAX_PALABRAS][64];
			while ((token = strtok_r(resto, ",", &resto)) != NULL) {
				char *item = recortar(token);
				if (*item == '\0')
					continue;
				if (count >= MAX_PALABRAS) {
					registrar("%s:%d: exceso de elementos en log_palabras (max %d)", archivo, n_linea, MAX_PALABRAS);
					break;
				}
				copiar_cadena(palabras_temp[count], item, sizeof(palabras_temp[count]));
				count++;
			}
			if (count > 0) {
				c->n_log_palabras = count;
				for (int i = 0; i < count; i++)
					copiar_cadena(c->log_palabras[i], palabras_temp[i], sizeof(c->log_palabras[i]));
			}
		} else {
			registrar("%s:%d: clave desconocida '%s'", archivo, n_linea, clave);
		}
	}

	free(linea);
	fclose(f);
	return 0;
}
