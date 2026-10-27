#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "comun.h"

int peticion_encabezado(const char *peticion, const char *nombre, char *valor, size_t tam)
{
	const char *p;
	const char *fin_headers;
	size_t len_nombre;

	if (!peticion || !nombre || !valor || tam == 0)
		return -1;

	p = strstr(peticion, "\r\n");
	if (!p)
		return -1;
	p += 2;

	fin_headers = strstr(p, "\r\n\r\n");
	if (!fin_headers)
		fin_headers = p + strlen(p);

	len_nombre = strlen(nombre);

	while (p < fin_headers) {
		const char *fin_linea = strstr(p, "\r\n");
		if (!fin_linea || fin_linea > fin_headers)
			fin_linea = fin_headers;

		if ((size_t)(fin_linea - p) > len_nombre &&
		    strncasecmp(p, nombre, len_nombre) == 0 &&
		    p[len_nombre] == ':') {
			const char *v = p + len_nombre + 1;
			while (v < fin_linea && (*v == ' ' || *v == '\t'))
				v++;
			const char *fin_v = fin_linea;
			while (fin_v > v && (fin_v[-1] == ' ' || fin_v[-1] == '\t'))
				fin_v--;

			size_t len_v = (size_t)(fin_v - v);
			if (len_v >= tam)
				return -1;
			memcpy(valor, v, len_v);
			valor[len_v] = '\0';
			return 0;
		}

		if (fin_linea == fin_headers)
			break;
		p = fin_linea + 2;
	}

	return -1;
}

int peticion_cookie(const char *peticion, const char *nombre, char *valor, size_t tam)
{
	char raw_cookie[4096];
	size_t len_nombre;
	const char *p;

	if (!peticion || !nombre || !valor || tam == 0)
		return -1;

	if (peticion_encabezado(peticion, "Cookie", raw_cookie, sizeof(raw_cookie)) < 0)
		return -1;

	len_nombre = strlen(nombre);
	p = raw_cookie;

	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;

		const char *sig = strchr(p, ';');
		const char *fin_token = sig ? sig : (p + strlen(p));

		if ((size_t)(fin_token - p) > len_nombre &&
		    strncmp(p, nombre, len_nombre) == 0 &&
		    p[len_nombre] == '=') {
			const char *v = p + len_nombre + 1;
			const char *fin_v = fin_token;
			while (fin_v > v && (fin_v[-1] == ' ' || fin_v[-1] == '\t'))
				fin_v--;

			size_t len_v = (size_t)(fin_v - v);
			if (len_v >= tam)
				return -1;
			memcpy(valor, v, len_v);
			valor[len_v] = '\0';
			return 0;
		}

		if (!sig)
			break;
		p = sig + 1;
	}

	return -1;
}

long peticion_content_length(const char *peticion)
{
	char valor[64];
	char *fin;
	long val;

	if (peticion_encabezado(peticion, "Content-Length", valor, sizeof(valor)) < 0)
		return -1;

	if (valor[0] == '\0')
		return -1;

	errno = 0;
	val = strtol(valor, &fin, 10);
	if (errno != 0 || fin == valor || *fin != '\0' || val < 0)
		return -1;

	return val;
}
