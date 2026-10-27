#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../comun.h"

static void test_encabezados(void)
{
	const char *req =
		"GET /api/estado HTTP/1.1\r\n"
		"Host: localhost:8080\r\n"
		"User-Agent: curl/7.81.0\r\n"
		"Accept: */*\r\n"
		"content-length: 42\r\n"
		"\r\n"
		"Host: cuerpo.falso\r\n";

	char buf[128];

	assert(peticion_encabezado(req, "Host", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "localhost:8080") == 0);

	assert(peticion_encabezado(req, "host", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "localhost:8080") == 0);

	assert(peticion_encabezado(req, "HOST", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "localhost:8080") == 0);

	assert(peticion_encabezado(req, "CONTENT-LENGTH", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "42") == 0);

	assert(peticion_encabezado(req, "Inexistente", buf, sizeof(buf)) == -1);

	const char *req_solo_cuerpo =
		"POST /api/login HTTP/1.1\r\n"
		"Content-Type: application/json\r\n"
		"\r\n"
		"Authorization: Bearer secreto\r\n";

	assert(peticion_encabezado(req_solo_cuerpo, "Authorization", buf, sizeof(buf)) == -1);

	printf("\033[0;32m✓ Caso encabezados: presente, ausente, mayusculas/minusculas y exclusor de cuerpo\033[0m\n");
}

static void test_cookies(void)
{
	char token[128];

	const char *req_sola =
		"GET /api/estado HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Cookie: so2_sesion=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\r\n"
		"\r\n";

	assert(peticion_cookie(req_sola, "so2_sesion", token, sizeof(token)) == 0);
	assert(strcmp(token, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);

	const char *req_multi =
		"GET /api/estado HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"Cookie: a=1; so2_sesion=token_en_medio; b=2\r\n"
		"\r\n";

	assert(peticion_cookie(req_multi, "so2_sesion", token, sizeof(token)) == 0);
	assert(strcmp(token, "token_en_medio") == 0);

	assert(peticion_cookie(req_multi, "a", token, sizeof(token)) == 0);
	assert(strcmp(token, "1") == 0);

	assert(peticion_cookie(req_multi, "b", token, sizeof(token)) == 0);
	assert(strcmp(token, "2") == 0);

	assert(peticion_cookie(req_multi, "no_existe", token, sizeof(token)) == -1);

	const char *req_sin_cookie =
		"GET /api/estado HTTP/1.1\r\n"
		"Host: localhost\r\n"
		"\r\n";

	assert(peticion_cookie(req_sin_cookie, "so2_sesion", token, sizeof(token)) == -1);

	printf("\033[0;32m✓ Caso cookies: sola, entre multiples delimitadas por punto y coma, y ausente\033[0m\n");
}

static void test_content_length(void)
{
	const char *req_valido =
		"POST /api/login HTTP/1.1\r\n"
		"Content-Length: 128\r\n"
		"\r\n";
	assert(peticion_content_length(req_valido) == 128);

	const char *req_cero =
		"POST /api/logout HTTP/1.1\r\n"
		"Content-Length: 0\r\n"
		"\r\n";
	assert(peticion_content_length(req_cero) == 0);

	const char *req_ausente =
		"POST /api/login HTTP/1.1\r\n"
		"\r\n";
	assert(peticion_content_length(req_ausente) == -1);

	const char *req_negativo =
		"POST /api/login HTTP/1.1\r\n"
		"Content-Length: -5\r\n"
		"\r\n";
	assert(peticion_content_length(req_negativo) == -1);

	const char *req_no_num =
		"POST /api/login HTTP/1.1\r\n"
		"Content-Length: abc\r\n"
		"\r\n";
	assert(peticion_content_length(req_no_num) == -1);

	const char *req_basura =
		"POST /api/login HTTP/1.1\r\n"
		"Content-Length: 12abc\r\n"
		"\r\n";
	assert(peticion_content_length(req_basura) == -1);

	const char *req_vacio =
		"POST /api/login HTTP/1.1\r\n"
		"Content-Length: \r\n"
		"\r\n";
	assert(peticion_content_length(req_vacio) == -1);

	printf("\033[0;32m✓ Caso Content-Length: valido, cero, ausente, negativo, no numerico y con basura\033[0m\n");
}

int main(void)
{
	printf("Iniciando pruebas de peticion.c:\n");
	test_encabezados();
	test_cookies();
	test_content_length();
	printf("Todas las pruebas de peticion.c pasaron exitosamente.\n");
	return 0;
}
