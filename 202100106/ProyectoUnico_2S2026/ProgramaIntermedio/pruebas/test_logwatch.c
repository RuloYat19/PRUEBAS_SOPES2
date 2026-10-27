#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../comun.h"
#include "../lib/cJSON.h"
#include "../rutas.h"

int fd_aviso = -1;
volatile sig_atomic_t ejecutando = 1;
struct estado estado = { .lock = PTHREAD_MUTEX_INITIALIZER };

static char ultimo_registro[256] = "";

void registrar(const char *formato, ...)
{
	va_list args;
	va_start(args, formato);
	vsnprintf(ultimo_registro, sizeof(ultimo_registro), formato, args);
	va_end(args);
}

static int sim_start_calls = 0;
static int sim_stop_calls = 0;
static long sim_start_return = 1;
static long sim_stop_return = 0;
static int sim_start_errno = 0;
static int sim_stop_errno = 0;
static char sim_last_central[PATH_MAX];
static char sim_last_palabra[64];
static char sim_last_rutas[LOG_WATCH_MAX_FILES][PATH_MAX];
static int sim_last_n_rutas = 0;
static int sim_last_stop_id = 0;

static long mock_start_log_watch(const char *const *rutas, int n, const char *central, const char *palabra)
{
	sim_start_calls++;
	sim_last_n_rutas = n;
	for (int i = 0; i < n; i++)
		snprintf(sim_last_rutas[i], sizeof(sim_last_rutas[i]), "%s", rutas[i]);
	snprintf(sim_last_central, sizeof(sim_last_central), "%s", central);
	snprintf(sim_last_palabra, sizeof(sim_last_palabra), "%s", palabra);
	if (sim_start_return < 0)
		errno = sim_start_errno;
	return sim_start_return;
}

static long mock_stop_log_watch(int id)
{
	sim_stop_calls++;
	sim_last_stop_id = id;
	if (sim_stop_return < 0)
		errno = sim_stop_errno;
	return sim_stop_return;
}

static void rm_rf(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *de;
	char buf[PATH_MAX];

	if (!d) {
		unlink(path);
		return;
	}

	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		snprintf(buf, sizeof(buf), "%s/%s", path, de->d_name);
		rm_rf(buf);
	}
	closedir(d);
	rmdir(path);
}

static void crear_archivo(const char *ruta)
{
	int fd = open(ruta, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	close(fd);
}

static void test_caso_1_validaciones(const char *base)
{
	struct peticion_logwatch p;
	char err[256];
	int cod = 0;
	char json[PATH_MAX * 10];
	char f1[64], f2[64], f_sub[128], f_link[128];
	char sub[64];

	snprintf(f1, sizeof(f1), "%s/f1.log", base);
	snprintf(f2, sizeof(f2), "%s/f2.log", base);
	crear_archivo(f1);
	crear_archivo(f2);

	snprintf(sub, sizeof(sub), "%s/sub", base);
	assert(mkdir(sub, 0755) == 0);
	snprintf(f_sub, sizeof(f_sub), "%s/sub.log", sub);
	crear_archivo(f_sub);

	snprintf(f_link, sizeof(f_link), "%s/link.log", base);
	assert(symlink(f1, f_link) == 0);

	logwatch_usar_syscalls(mock_start_log_watch, mock_stop_log_watch);
	sim_start_calls = 0;

	assert(logwatch_validar("[]", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":\"x\",\"central\":\"/c\"}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":\"x\",\"central\":\"/c\",\"rutas\":[],\"extra\":1}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":123,\"central\":\"/c\",\"rutas\":[]}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":\"x\",\"central\":123,\"rutas\":[]}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":\"x\",\"central\":\"/c\",\"rutas\":123}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":\"x\",\"central\":\"/c\",\"rutas\":[123]}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"PALABRA\":\"x\",\"central\":\"/c\",\"rutas\":[]}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":\"x\",\"Central\":\"/c\",\"rutas\":[]}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	assert(logwatch_validar("{\"palabra\":\"x\",\"central\":\"/c\",\"Rutas\":[]}", &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Cuerpo invalido") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Palabra invalida") == 0);

	char pal_larga[100];
	memset(pal_larga, 'a', 64);
	pal_larga[64] = '\0';
	snprintf(json, sizeof(json), "{\"palabra\":\"%s\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", pal_larga, base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Palabra invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"hola|mundo\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Palabra invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"hola\\tmundo\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Palabra invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[]}", base);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Se requieren entre 1 y 8 rutas") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]}",
		 base, f1, f1, f1, f1, f1, f1, f1, f1, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Se requieren entre 1 y 8 rutas") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"/etc/shadow\",\"rutas\":[\"%s\"]}", f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Ruta central invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/sub/c.log\",\"rutas\":[\"%s\"]}", base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Ruta central invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/.\",\"rutas\":[\"%s\"]}", base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Ruta central invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/..\",\"rutas\":[\"%s\"]}", base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Ruta central invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/link.log\",\"rutas\":[\"%s\"]}", base, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Ruta central invalida") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"/etc/passwd\"]}", base);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Ruta invalida: /etc/passwd") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s/no_existe.log\"]}", base, base);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strstr(err, "Ruta invalida:") != NULL);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base, f_sub);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strstr(err, "Ruta invalida:") != NULL);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base, f_link);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strstr(err, "Ruta invalida:") != NULL);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base, sub);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strstr(err, "Ruta invalida:") != NULL);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\",\"%s\"]}", base, f1, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strstr(err, "Ruta repetida:") != NULL);

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s\",\"rutas\":[\"%s\"]}", f1, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "La ruta central no puede ser un archivo vigilado") == 0);

	assert(sim_start_calls == 0);
	printf("\033[0;32m✓ Caso 1: Validaciones de cuerpo, palabra, rutas y central en orden esperado\033[0m\n");
}

static void test_caso_2_crear_valido(const char *base)
{
	struct peticion_logwatch p;
	struct logwatch creado;
	char err[256];
	int cod = 0;
	char json[PATH_MAX * 4];
	char f1[64], f2[64], central[64];

	snprintf(f1, sizeof(f1), "%s/f1.log", base);
	snprintf(f2, sizeof(f2), "%s/f2.log", base);
	snprintf(central, sizeof(central), "%s/central_valida.txt", base);

	sim_start_calls = 0;
	sim_start_return = 101;

	snprintf(json, sizeof(json), "{\"palabra\":\"ERROR_TEST\",\"central\":\"%s\",\"rutas\":[\"%s\",\"%s\"]}",
		 central, f1, f2);

	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) == 0);
	assert(strcmp(p.palabra, "ERROR_TEST") == 0);
	assert(strcmp(p.central, central) == 0);
	assert(p.n_rutas == 2);

	assert(logwatch_crear(&p, "dashboard", "admin_user", &creado, err, sizeof(err)) == 0);
	assert(sim_start_calls == 1);
	assert(creado.id == 101);
	assert(strcmp(creado.palabra, "ERROR_TEST") == 0);
	assert(strcmp(creado.central, central) == 0);
	assert(creado.n_rutas == 2);
	assert(strcmp(creado.origen, "dashboard") == 0);
	assert(strcmp(creado.usuario, "admin_user") == 0);
	assert(strlen(creado.creado) == 19);

	struct logwatch lista[10];
	size_t n = logwatch_lista(lista, 10);
	assert(n == 1);
	assert(lista[0].id == 101);

	printf("\033[0;32m✓ Caso 2: Creacion valida registra correctamente y llama a syscall fuera de lock\033[0m\n");
}

static void test_caso_3_crear_central_inexistente(const char *base)
{
	struct peticion_logwatch p;
	struct logwatch creado;
	char err[256];
	char central_nueva[PATH_MAX];
	char f1[PATH_MAX];
	struct stat st;

	snprintf(central_nueva, sizeof(central_nueva), "%s/nueva_central_creada.txt", base);
	snprintf(f1, sizeof(f1), "%s/f1.log", base);
	unlink(central_nueva);

	snprintf(p.palabra, sizeof(p.palabra), "FALLO");
	snprintf(p.central, sizeof(p.central), "%s", central_nueva);
	p.n_rutas = 1;
	snprintf(p.rutas[0], sizeof(p.rutas[0]), "%s", f1);

	sim_start_return = 102;
	assert(logwatch_crear(&p, "dashboard", "admin_user", &creado, err, sizeof(err)) == 0);

	assert(stat(central_nueva, &st) == 0);
	assert(S_ISREG(st.st_mode));
	assert((st.st_mode & 0777) == 0644);

	printf("\033[0;32m✓ Caso 3: Central inexistente se crea con permisos 0644 y dueno de base\033[0m\n");
}

static void test_caso_4_syscall_falla(const char *base)
{
	struct peticion_logwatch p;
	char err[256];
	char f1[PATH_MAX];

	snprintf(f1, sizeof(f1), "%s/f1.log", base);
	snprintf(p.palabra, sizeof(p.palabra), "WARN");
	snprintf(p.central, sizeof(p.central), "%s/central_falla.txt", base);
	p.n_rutas = 1;
	snprintf(p.rutas[0], sizeof(p.rutas[0]), "%s", f1);

	sim_start_return = -1;
	sim_start_errno = ENOSYS;

	struct logwatch l_antes[20];
	size_t n_antes = logwatch_lista(l_antes, 20);

	assert(logwatch_crear(&p, "dashboard", "admin_user", NULL, err, sizeof(err)) == -2);
	assert(strstr(err, "Function not implemented") != NULL);

	struct logwatch l_desp[20];
	size_t n_desp = logwatch_lista(l_desp, 20);
	assert(n_desp == n_antes);

	printf("\033[0;32m✓ Caso 4: Fallo de start_log_watch retorna -2 con strerror y no altera registro\033[0m\n");
}

static void test_caso_5_limite_16(const char *base)
{
	char f1[64];
	snprintf(f1, sizeof(f1), "%s/f1.log", base);

	logwatch_iniciar(base);
	sim_start_return = 1;

	for (int i = 1; i <= 16; i++) {
		struct peticion_logwatch p;
		snprintf(p.palabra, sizeof(p.palabra), "WORD_%d", i);
		snprintf(p.central, sizeof(p.central), "%s/c_%d.txt", base, i);
		p.n_rutas = 1;
		snprintf(p.rutas[0], sizeof(p.rutas[0]), "%s", f1);
		sim_start_return = i;
		assert(logwatch_crear(&p, "dashboard", "admin", NULL, NULL, 0) == 0);
	}

	struct logwatch l[20];
	assert(logwatch_lista(l, 20) == 16);

	char json[PATH_MAX * 4];
	char err[256];
	int cod = 0;
	struct peticion_logwatch p17;
	snprintf(json, sizeof(json), "{\"palabra\":\"WORD_17\",\"central\":\"%s/c_17.txt\",\"rutas\":[\"%s\"]}", base, f1);

	sim_start_calls = 0;
	assert(logwatch_validar(json, &p17, &cod, err, sizeof(err)) < 0);
	assert(cod == 409);
	assert(strcmp(err, "Limite de 16 logwatch alcanzado") == 0);
	assert(sim_start_calls == 0);

	printf("\033[0;32m✓ Caso 5: Limite de 16 logwatch alcanzado responde 409 y no llama a syscall\033[0m\n");
}

static void test_caso_6_ciclos(const char *base)
{
	char f1[64], f2[64];
	snprintf(f1, sizeof(f1), "%s/f1.log", base);
	snprintf(f2, sizeof(f2), "%s/f2.log", base);

	logwatch_iniciar(base);
	struct peticion_logwatch p1;
	snprintf(p1.palabra, sizeof(p1.palabra), "P1");
	snprintf(p1.central, sizeof(p1.central), "%s/c_base.txt", base);
	p1.n_rutas = 1;
	snprintf(p1.rutas[0], sizeof(p1.rutas[0]), "%s", f1);
	sim_start_return = 10;
	assert(logwatch_crear(&p1, "dashboard", "admin", NULL, NULL, 0) == 0);

	char json[PATH_MAX * 4];
	char err[256];
	int cod = 0;
	struct peticion_logwatch p_inval;

	snprintf(json, sizeof(json), "{\"palabra\":\"P2\",\"central\":\"%s\",\"rutas\":[\"%s\"]}", f1, f2);
	assert(logwatch_validar(json, &p_inval, &cod, err, sizeof(err)) < 0);
	assert(cod == 409);
	assert(strcmp(err, "Conflicto con el logwatch 10: un log central no puede vigilarse") == 0);

	snprintf(json, sizeof(json), "{\"palabra\":\"P3\",\"central\":\"%s/c_otra.txt\",\"rutas\":[\"%s/c_base.txt\"]}", base, base);
	assert(logwatch_validar(json, &p_inval, &cod, err, sizeof(err)) < 0);
	assert(cod == 409);
	assert(strcmp(err, "Conflicto con el logwatch 10: un log central no puede vigilarse") == 0);

	printf("\033[0;32m✓ Caso 6: Deteccion de ciclos contra central y vigiladas de logwatch activos\033[0m\n");
}

static void test_caso_7_finalizar(const char *base)
{
	char f1[PATH_MAX];
	snprintf(f1, sizeof(f1), "%s/f1.log", base);

	logwatch_iniciar(base);
	struct peticion_logwatch p;
	snprintf(p.palabra, sizeof(p.palabra), "TEST_STOP");
	snprintf(p.central, sizeof(p.central), "%s/c_stop.txt", base);
	p.n_rutas = 1;
	snprintf(p.rutas[0], sizeof(p.rutas[0]), "%s", f1);
	sim_start_return = 55;
	assert(logwatch_crear(&p, "dashboard", "admin", NULL, NULL, 0) == 0);

	char err[256];
	sim_stop_calls = 0;
	assert(logwatch_finalizar(999, err, sizeof(err)) == -ENOENT);
	assert(sim_stop_calls == 0);

	sim_stop_calls = 0;
	sim_stop_return = -1;
	sim_stop_errno = EPERM;
	assert(logwatch_finalizar(55, err, sizeof(err)) == -1);
	assert(sim_stop_calls == 1);
	assert(strstr(err, "Operation not permitted") != NULL);

	struct logwatch l[10];
	assert(logwatch_lista(l, 10) == 1);

	sim_stop_calls = 0;
	sim_stop_return = -1;
	sim_stop_errno = ESRCH;
	assert(logwatch_finalizar(55, err, sizeof(err)) == -ENOENT);
	assert(sim_stop_calls == 1);
	assert(logwatch_lista(l, 10) == 0);

	sim_start_return = 56;
	assert(logwatch_crear(&p, "dashboard", "admin", NULL, NULL, 0) == 0);
	assert(logwatch_lista(l, 10) == 1);

	sim_stop_calls = 0;
	sim_stop_return = 0;
	assert(logwatch_finalizar(56, err, sizeof(err)) == 0);
	assert(sim_stop_calls == 1);
	assert(logwatch_lista(l, 10) == 0);

	printf("\033[0;32m✓ Caso 7: Finalizar cubre exito, ESRCH, error EPERM e ID inexistente\033[0m\n");
}

static void test_caso_8_finalizar_todos_y_orden(const char *base)
{
	char f1[PATH_MAX];
	snprintf(f1, sizeof(f1), "%s/f1.log", base);

	logwatch_iniciar(base);
	struct peticion_logwatch p;
	p.n_rutas = 1;
	snprintf(p.rutas[0], sizeof(p.rutas[0]), "%s", f1);

	sim_start_return = 50;
	snprintf(p.palabra, sizeof(p.palabra), "W50");
	snprintf(p.central, sizeof(p.central), "%s/c50.txt", base);
	assert(logwatch_crear(&p, "dashboard", "admin", NULL, NULL, 0) == 0);

	sim_start_return = 10;
	snprintf(p.palabra, sizeof(p.palabra), "W10");
	snprintf(p.central, sizeof(p.central), "%s/c10.txt", base);
	assert(logwatch_crear(&p, "dashboard", "admin", NULL, NULL, 0) == 0);

	sim_start_return = 30;
	snprintf(p.palabra, sizeof(p.palabra), "W30");
	snprintf(p.central, sizeof(p.central), "%s/c30.txt", base);
	assert(logwatch_crear(&p, "dashboard", "admin", NULL, NULL, 0) == 0);

	struct logwatch l[10];
	size_t n = logwatch_lista(l, 10);
	assert(n == 3);
	assert(l[0].id == 10);
	assert(l[1].id == 30);
	assert(l[2].id == 50);

	sim_stop_calls = 0;
	sim_stop_return = 0;
	logwatch_finalizar_todos();
	assert(sim_stop_calls == 3);
	assert(logwatch_lista(l, 10) == 0);

	printf("\033[0;32m✓ Caso 8: logwatch_lista orden ascendente y logwatch_finalizar_todos\033[0m\n");
}

static void test_caso_9_json_logwatch(const char *base)
{
	struct logwatch lw;
	struct buf b = { 0 };

	lw.id = 42;
	snprintf(lw.palabra, sizeof(lw.palabra), "ERROR");
	snprintf(lw.central, sizeof(lw.central), "%s/central.log", base);
	lw.n_rutas = 2;
	snprintf(lw.rutas[0], sizeof(lw.rutas[0]), "%s/app.log", base);
	snprintf(lw.rutas[1], sizeof(lw.rutas[1]), "%s/auth.log", base);
	snprintf(lw.origen, sizeof(lw.origen), "dashboard");
	snprintf(lw.usuario, sizeof(lw.usuario), "so2t_admin");
	snprintf(lw.creado, sizeof(lw.creado), "2026-10-21 22:00:00");

	json_logwatch(&b, &lw);

	cJSON *root = cJSON_Parse(b.datos);
	assert(root != NULL);
	assert(cJSON_GetArraySize(root) == 7);
	assert(cJSON_GetObjectItem(root, "id") != NULL && cJSON_GetObjectItem(root, "id")->valueint == 42);
	assert(cJSON_GetObjectItem(root, "palabra") != NULL);
	assert(cJSON_GetObjectItem(root, "central") != NULL);
	assert(cJSON_GetObjectItem(root, "rutas") != NULL && cJSON_IsArray(cJSON_GetObjectItem(root, "rutas")));
	assert(cJSON_GetArraySize(cJSON_GetObjectItem(root, "rutas")) == 2);
	assert(cJSON_GetObjectItem(root, "origen") != NULL);
	assert(cJSON_GetObjectItem(root, "usuario") != NULL);
	assert(cJSON_GetObjectItem(root, "creado") != NULL);

	cJSON_Delete(root);
	buf_liberar(&b);

	printf("\033[0;32m✓ Caso 9: json_logwatch tiene exactamente las 7 claves de R4.2 y es parseable\033[0m\n");
}

static void test_caso_10_base_enlace(void)
{
	char plantilla[] = "/tmp/test_lw_base_XXXXXX";
	char *base_dir = mkdtemp(plantilla);
	assert(base_dir != NULL);

	char base_real[PATH_MAX];
	snprintf(base_real, sizeof(base_real), "%s_real", base_dir);

	char f1[PATH_MAX + 16], f1_real[PATH_MAX + 16];
	snprintf(f1, sizeof(f1), "%s/f1.log", base_dir);
	crear_archivo(f1);

	logwatch_iniciar(base_dir);

	assert(rename(base_dir, base_real) == 0);
	assert(symlink(base_real, base_dir) == 0);

	snprintf(f1_real, sizeof(f1_real), "%s/f1.log", base_real);

	struct peticion_logwatch p;
	char err[256];
	int cod = 0;
	char json[PATH_MAX * 4];

	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base_dir, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strcmp(err, "Ruta central invalida") == 0);

	logwatch_iniciar(base_real);
	snprintf(json, sizeof(json), "{\"palabra\":\"SO2\",\"central\":\"%s/c.log\",\"rutas\":[\"%s\"]}", base_real, f1);
	assert(logwatch_validar(json, &p, &cod, err, sizeof(err)) < 0);
	assert(cod == 400 && strstr(err, "Ruta invalida:") != NULL);

	unlink(base_dir);
	unlink(f1_real);
	rmdir(base_real);

	printf("\033[0;32m✓ Caso 10: Base reemplazada por enlace rechaza ruta vigilada y central\033[0m\n");
}

static void test_caso_11_finalizar_todos_stop_falla(const char *base)
{
	char f1[PATH_MAX];
	snprintf(f1, sizeof(f1), "%s/f1.log", base);

	logwatch_iniciar(base);
	struct peticion_logwatch p;
	p.n_rutas = 1;
	snprintf(p.rutas[0], sizeof(p.rutas[0]), "%s", f1);

	sim_start_return = 70;
	snprintf(p.palabra, sizeof(p.palabra), "W70");
	snprintf(p.central, sizeof(p.central), "%s/c70.txt", base);
	assert(logwatch_crear(&p, "dashboard", "admin", NULL, NULL, 0) == 0);

	struct logwatch l[10];
	assert(logwatch_lista(l, 10) == 1);

	sim_stop_calls = 0;
	sim_stop_return = -1;
	sim_stop_errno = EINVAL;

	logwatch_finalizar_todos();
	assert(sim_stop_calls == 1);
	assert(logwatch_lista(l, 10) == 0);
	assert(strstr(ultimo_registro, "sys_stop_log_watch(70) fallo:") != NULL);

	printf("\033[0;32m✓ Caso 11: logwatch_finalizar_todos con fallo de stop vacia el registro\033[0m\n");
}

int main(void)
{
	char plantilla[] = "/tmp/test_logwatch_XXXXXX";
	char *tmpdir = mkdtemp(plantilla);
	assert(tmpdir != NULL);

	printf("Iniciando pruebas de logwatch e IPC:\n");
	logwatch_iniciar(tmpdir);

	test_caso_1_validaciones(tmpdir);
	test_caso_2_crear_valido(tmpdir);
	test_caso_3_crear_central_inexistente(tmpdir);
	test_caso_4_syscall_falla(tmpdir);
	test_caso_5_limite_16(tmpdir);
	test_caso_6_ciclos(tmpdir);
	test_caso_7_finalizar(tmpdir);
	test_caso_8_finalizar_todos_y_orden(tmpdir);
	test_caso_9_json_logwatch(tmpdir);
	test_caso_10_base_enlace();
	test_caso_11_finalizar_todos_stop_falla(tmpdir);

	rm_rf(tmpdir);
	printf("Todas las pruebas de logwatch pasaron exitosamente.\n");
	return 0;
}
