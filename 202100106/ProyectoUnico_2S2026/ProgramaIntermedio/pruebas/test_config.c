#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../comun.h"

void registrar(const char *formato, ...)
{
	(void)formato;
}

static int crear_archivo_temp(const char *contenido, char *ruta_salida, size_t tamano)
{
	char plantilla[] = "/tmp/test_conf_XXXXXX";
	int fd = mkstemp(plantilla);
	if (fd < 0)
		return -1;
	ssize_t escrito = write(fd, contenido, strlen(contenido));
	(void)escrito;
	close(fd);
	snprintf(ruta_salida, tamano, "%s", plantilla);
	return 0;
}

static void test_caso_1_claves_validas(void)
{
	char ruta[PATH_MAX];
	const char *conf_str =
		"puerto = 9090\n"
		"intervalo = 5\n"
		"dashboard = /var/www\n"
		"log_central = /var/log/central.txt\n"
		"log_rutas = /var/log/a.log, /var/log/b.log\n"
		"log_palabras = WARN, CRIT\n"
		"ruta_vigilada = /var/vigilado\n"
		"blacklist = /etc/blacklist.json\n";

	assert(crear_archivo_temp(conf_str, ruta, sizeof(ruta)) == 0);
	struct configuracion c;
	config_por_defecto(&c, ".");
	assert(config_cargar(&c, ruta) == 0);
	unlink(ruta);

	assert(c.puerto == 9090);
	assert(c.intervalo == 5);
	assert(strcmp(c.dashboard, "/var/www") == 0);
	assert(strcmp(c.log_central, "/var/log/central.txt") == 0);
	assert(c.n_log_rutas == 2);
	assert(strcmp(c.log_rutas[0], "/var/log/a.log") == 0);
	assert(strcmp(c.log_rutas[1], "/var/log/b.log") == 0);
	assert(c.n_log_palabras == 2);
	assert(strcmp(c.log_palabras[0], "WARN") == 0);
	assert(strcmp(c.log_palabras[1], "CRIT") == 0);
	assert(strcmp(c.ruta_vigilada, "/var/vigilado") == 0);
	assert(strcmp(c.blacklist, "/etc/blacklist.json") == 0);

	printf("\033[0;32m✓ Caso 1: Todas las claves validas leidas correctamente\033[0m\n");
}

static void test_caso_2_comentarios_y_espacios(void)
{
	char ruta[PATH_MAX];
	const char *conf_str =
		"# Comentario inicial\n"
		"\n"
		"   puerto   =    9999   \n"
		"  # Comentario intermedio\n"
		"intervalo=10\n"
		"\n";

	assert(crear_archivo_temp(conf_str, ruta, sizeof(ruta)) == 0);
	struct configuracion c;
	config_por_defecto(&c, ".");
	assert(config_cargar(&c, ruta) == 0);
	unlink(ruta);

	assert(c.puerto == 9999);
	assert(c.intervalo == 10);

	printf("\033[0;32m✓ Caso 2: Comentarios, lineas vacias y espacios procesados correctamente\033[0m\n");
}

static void test_caso_3_clave_desconocida(void)
{
	char ruta[PATH_MAX];
	const char *conf_str =
		"clave_desconocida = valor_desconocido\n"
		"puerto = 8081\n"
		"otra_invalida = 123\n"
		"intervalo = 3\n";

	assert(crear_archivo_temp(conf_str, ruta, sizeof(ruta)) == 0);
	struct configuracion c;
	config_por_defecto(&c, ".");
	assert(config_cargar(&c, ruta) == 0);
	unlink(ruta);

	assert(c.puerto == 8081);
	assert(c.intervalo == 3);

	printf("\033[0;32m✓ Caso 3: Claves desconocidas ignoradas, resto leido\033[0m\n");
}

static void test_caso_4_valores_fuera_de_rango(void)
{
	char ruta[PATH_MAX];
	const char *conf_str =
		"puerto = 70000\n"
		"intervalo = 0\n";

	assert(crear_archivo_temp(conf_str, ruta, sizeof(ruta)) == 0);
	struct configuracion c;
	config_por_defecto(&c, ".");
	assert(config_cargar(&c, ruta) == 0);
	unlink(ruta);

	assert(c.puerto == 8080);
	assert(c.intervalo == 2);

	printf("\033[0;32m✓ Caso 4: Valores fuera de rango conservan valores por defecto\033[0m\n");
}

static void test_caso_5_lista_exceso_elementos(void)
{
	char ruta[PATH_MAX];
	const char *conf_str =
		"log_palabras = P1, P2, P3, P4, P5, P6, P7, P8, P9, P10\n";

	assert(crear_archivo_temp(conf_str, ruta, sizeof(ruta)) == 0);
	struct configuracion c;
	config_por_defecto(&c, ".");
	assert(config_cargar(&c, ruta) == 0);
	unlink(ruta);

	assert(c.n_log_palabras == 8);
	assert(strcmp(c.log_palabras[0], "P1") == 0);
	assert(strcmp(c.log_palabras[7], "P8") == 0);

	printf("\033[0;32m✓ Caso 5: log_palabras con 10 elementos acotado a 8\033[0m\n");
}

static void test_caso_6_expansion_tilde(void)
{
	char ruta[PATH_MAX];
	const char *conf_str =
		"ruta_vigilada = ~/x\n";

	assert(crear_archivo_temp(conf_str, ruta, sizeof(ruta)) == 0);
	unsetenv("SUDO_USER");
	struct configuracion c;
	config_por_defecto(&c, ".");
	assert(config_cargar(&c, ruta) == 0);
	unlink(ruta);

	const char *home = getenv("HOME");
	char esperado[PATH_MAX];
	snprintf(esperado, sizeof(esperado), "%s/x", home ? home : ".");
	assert(strcmp(c.ruta_vigilada, esperado) == 0);

	printf("\033[0;32m✓ Caso 6: Expansion ~/x a $HOME/x correcta\033[0m\n");
}

static void test_caso_7_archivo_inexistente(void)
{
	struct configuracion c;
	config_por_defecto(&c, ".");
	int ret = config_cargar(&c, "/ruta/inexistente/para/prueba.conf");
	assert(ret == -1);

	printf("\033[0;32m✓ Caso 7: Archivo inexistente retorna -1\033[0m\n");
}

static void test_caso_8_directorio_retorna_error(void)
{
	struct configuracion c;
	config_por_defecto(&c, ".");
	int ret = config_cargar(&c, "/tmp");
	assert(ret == -1);

	printf("\033[0;32m✓ Caso 8: Ruta a directorio retorna -1\033[0m\n");
}

static void test_caso_9_sudo_user_ignorado_sin_root(void)
{
	char ruta[PATH_MAX];
	const char *conf_str = "ruta_vigilada = ~/x\n";

	assert(crear_archivo_temp(conf_str, ruta, sizeof(ruta)) == 0);

	setenv("SUDO_USER", "root", 1);
	setenv("HOME", "/tmp/fake_home", 1);

	struct configuracion c;
	config_por_defecto(&c, ".");
	assert(config_cargar(&c, ruta) == 0);
	unlink(ruta);

	if (geteuid() == 0) {
		assert(strcmp(c.ruta_vigilada, "/root/x") == 0);
	} else {
		assert(strcmp(c.ruta_vigilada, "/tmp/fake_home/x") == 0);
	}

	unsetenv("SUDO_USER");

	printf("\033[0;32m✓ Caso 9: SUDO_USER procesado segun euid (root: /root, no root: se ignora)\033[0m\n");
}

int main(void)
{
	printf("Iniciando pruebas del parser de configuracion:\n");
	test_caso_1_claves_validas();
	test_caso_2_comentarios_y_espacios();
	test_caso_3_clave_desconocida();
	test_caso_4_valores_fuera_de_rango();
	test_caso_5_lista_exceso_elementos();
	test_caso_6_expansion_tilde();
	test_caso_7_archivo_inexistente();
	test_caso_8_directorio_retorna_error();
	test_caso_9_sudo_user_ignorado_sin_root();
	printf("Todas las pruebas pasaron exitosamente.\n");
	return 0;
}
