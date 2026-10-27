#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../comun.h"

void registrar(const char *formato, ...)
{
	(void)formato;
}

static void test_sesion_crear_y_buscar(void)
{
	char token[TAM_TOKEN + 1];
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t t0 = 100000;

	assert(sesion_crear("admin", ROL_ADMIN, t0, token) == 0);
	assert(strlen(token) == 64);
	for (int i = 0; i < 64; i++) {
		char c = token[i];
		assert((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
	}

	assert(sesion_buscar(token, t0, usuario, &rol) == 0);
	assert(strcmp(usuario, "admin") == 0);
	assert(rol == ROL_ADMIN);

	printf("\033[0;32m✓ Caso 1: sesion_crear devuelve 64 hex y sesion_buscar recupera usuario y rol\033[0m\n");
}

static void test_tokens_distintos(void)
{
	char t1[TAM_TOKEN + 1];
	char t2[TAM_TOKEN + 1];
	time_t t0 = 100000;

	assert(sesion_crear("u1", ROL_COMUN, t0, t1) == 0);
	assert(sesion_crear("u2", ROL_COMUN, t0, t2) == 0);
	assert(strcmp(t1, t2) != 0);

	printf("\033[0;32m✓ Caso 2: Dos sesiones creadas seguidas tienen tokens distintos\033[0m\n");
}

static void test_token_desconocido(void)
{
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t t0 = 100000;
	const char *inventado = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

	assert(sesion_buscar(inventado, t0, usuario, &rol) == -1);

	printf("\033[0;32m✓ Caso 3: Token desconocido retorna -1\033[0m\n");
}

static void test_expiracion_y_renovacion(void)
{
	char token[TAM_TOKEN + 1];
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t t = 200000;

	assert(sesion_crear("u_renueva", ROL_COMUN, t, token) == 0);

	assert(sesion_buscar(token, t + 1800, usuario, &rol) == 0);

	assert(sesion_buscar(token, t + 1800 + 1801, usuario, &rol) == -1);

	printf("\033[0;32m✓ Caso 4: Renueva al buscar; expira a los 1801 s sin renovar\033[0m\n");
}

static void test_vigente_no_renueva(void)
{
	char token[TAM_TOKEN + 1];
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t t = 300000;

	assert(sesion_crear("u_vigente", ROL_COMUN, t, token) == 0);

	assert(sesion_vigente(token, t + 1000) == 1);

	assert(sesion_buscar(token, t + 1801, usuario, &rol) == -1);

	printf("\033[0;32m✓ Caso 5: sesion_vigente no renueva y permite que la sesion expire en t+1801\033[0m\n");
}

static void test_eliminar(void)
{
	char token[TAM_TOKEN + 1];
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t t = 400000;

	assert(sesion_crear("u_borrar", ROL_COMUN, t, token) == 0);
	assert(sesion_buscar(token, t, usuario, &rol) == 0);

	sesion_eliminar(token);
	assert(sesion_buscar(token, t, usuario, &rol) == -1);

	printf("\033[0;32m✓ Caso 6: sesion_eliminar invalida la sesion inmediatamente\033[0m\n");
}

static void test_expulsion_lru(void)
{
	char tokens[65][TAM_TOKEN + 1];
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t t = 500000;

	for (int i = 0; i < 65; i++) {
		char nom[32];
		snprintf(nom, sizeof(nom), "user_%d", i);
		assert(sesion_crear(nom, ROL_COMUN, t + i, tokens[i]) == 0);
	}

	assert(sesion_buscar(tokens[0], t + 100, usuario, &rol) == -1);

	assert(sesion_buscar(tokens[64], t + 100, usuario, &rol) == 0);
	assert(strcmp(usuario, "user_64") == 0);

	printf("\033[0;32m✓ Caso 7: Tabla llena (65 sesiones) expulsa la mas antigua y conserva la nueva\033[0m\n");
}

static void test_rol_de_grupos(void)
{
	assert(rol_de_grupos(1, 1) == ROL_ADMIN);
	assert(rol_de_grupos(1, 0) == ROL_ADMIN);
	assert(rol_de_grupos(0, 1) == ROL_COMUN);
	assert(rol_de_grupos(0, 0) == ROL_NINGUNO);

	assert(strcmp(rol_nombre(ROL_ADMIN), "admin_user") == 0);
	assert(strcmp(rol_nombre(ROL_COMUN), "common_user") == 0);
	assert(strcmp(rol_nombre(ROL_NINGUNO), "ninguno") == 0);

	printf("\033[0;32m✓ Caso 8: rol_de_grupos resuelve admin, comun y ninguno segun pertenencia\033[0m\n");
}

static void test_rol_suficiente(void)
{
	assert(rol_suficiente(ROL_ADMIN, ROL_ADMIN) == 1);
	assert(rol_suficiente(ROL_ADMIN, ROL_COMUN) == 1);
	assert(rol_suficiente(ROL_ADMIN, ROL_NINGUNO) == 1);

	assert(rol_suficiente(ROL_COMUN, ROL_ADMIN) == 0);
	assert(rol_suficiente(ROL_COMUN, ROL_COMUN) == 1);
	assert(rol_suficiente(ROL_COMUN, ROL_NINGUNO) == 1);

	assert(rol_suficiente(ROL_NINGUNO, ROL_ADMIN) == 0);
	assert(rol_suficiente(ROL_NINGUNO, ROL_COMUN) == 0);
	assert(rol_suficiente(ROL_NINGUNO, ROL_NINGUNO) == 1);

	printf("\033[0;32m✓ Caso 9: rol_suficiente validado en sus 9 combinaciones\033[0m\n");
}

static void test_permiso_buscar(void)
{
	enum rol minimo;

	assert(permiso_buscar("POST", "/api/login", &minimo) == 0);
	assert(minimo == ROL_NINGUNO);

	assert(permiso_buscar("POST", "/api/logout", &minimo) == 0);
	assert(minimo == ROL_NINGUNO);

	assert(permiso_buscar("GET", "/api/sesion", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("GET", "/api/estado", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("GET", "/api/eventos", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("GET", "/api/alertas", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("GET", "/api/metricas", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("GET", "/api/monitoreo", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("POST", "/api/monitoreo/metricas", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("GET", "/api/proceso", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("GET", "/api/procesos", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("POST", "/api/monitoreo/procesos", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("POST", "/api/proceso/kill", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("GET", "/api/archivos", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("POST", "/api/monitoreo/archivos", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("POST", "/api/ruta", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("GET", "/api/blacklist", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("POST", "/api/blacklist", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("GET", "/api/amenazas", &minimo) == 0);
	assert(minimo == ROL_COMUN);

	assert(permiso_buscar("GET", "/api/cuarentena", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("POST", "/api/cuarentena", &minimo) == 0);
	assert(minimo == ROL_ADMIN);

	assert(permiso_buscar("GET", "/api/login", &minimo) == -2);
	assert(permiso_buscar("POST", "/api/estado", &minimo) == -2);
	assert(permiso_buscar("POST", "/api/metricas", &minimo) == -2);
	assert(permiso_buscar("GET", "/api/monitoreo/metricas", &minimo) == -2);
	assert(permiso_buscar("POST", "/api/procesos", &minimo) == -2);
	assert(permiso_buscar("GET", "/api/monitoreo/procesos", &minimo) == -2);
	assert(permiso_buscar("GET", "/api/proceso/kill", &minimo) == -2);
	assert(permiso_buscar("POST", "/api/archivos", &minimo) == -2);
	assert(permiso_buscar("GET", "/api/monitoreo/archivos", &minimo) == -2);
	assert(permiso_buscar("GET", "/api/ruta", &minimo) == -2);
	assert(permiso_buscar("POST", "/api/amenazas", &minimo) == -2);
	assert(permiso_buscar("DELETE", "/api/blacklist", &minimo) == -2);
	assert(permiso_buscar("DELETE", "/api/cuarentena", &minimo) == -2);

	assert(permiso_buscar("GET", "/api/logwatch", &minimo) == 0 && minimo == ROL_ADMIN);
	assert(permiso_buscar("POST", "/api/logwatch", &minimo) == 0 && minimo == ROL_ADMIN);
	assert(permiso_buscar("DELETE", "/api/logwatch", &minimo) == 0 && minimo == ROL_ADMIN);
	assert(permiso_buscar("PUT", "/api/logwatch", &minimo) == -2);

	assert(permiso_buscar("GET", "/api/noexiste", &minimo) == -1);
	assert(permiso_buscar("POST", "/api/desconocida", &minimo) == -1);

	printf("\033[0;32m✓ Caso 10: permiso_buscar valida 24 filas, metodo no permitido (-2) y ruta no encontrada (-1)\033[0m\n");
}

static void test_usuario_inexistente(void)
{
	enum rol r = auth_rol_de_usuario("usuario_que_no_existe_so2");
	assert(r == ROL_NINGUNO);

	printf("\033[0;32m✓ Caso 11: auth_rol_de_usuario con usuario inexistente retorna ROL_NINGUNO\033[0m\n");
}

int main(void)
{
	printf("Iniciando pruebas de auth.c:\n");
	test_sesion_crear_y_buscar();
	test_tokens_distintos();
	test_token_desconocido();
	test_expiracion_y_renovacion();
	test_vigente_no_renueva();
	test_eliminar();
	test_expulsion_lru();
	test_rol_de_grupos();
	test_rol_suficiente();
	test_permiso_buscar();
	test_usuario_inexistente();
	printf("Todas las pruebas de auth.c pasaron exitosamente.\n");
	return 0;
}
