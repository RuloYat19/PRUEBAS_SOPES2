#include <assert.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../comun.h"
#include "../ebpf/filtro_ruta.h"
#include "../lib/cJSON.h"

int fd_aviso = -1;
volatile sig_atomic_t ejecutando = 1;
struct estado estado = { .lock = PTHREAD_MUTEX_INITIALIZER };

void registrar(const char *formato, ...)
{
	(void)formato;
}

static int rm_callback(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

static void rm_rf(const char *path)
{
	nftw(path, rm_callback, 64, FTW_DEPTH | FTW_PHYS);
}

static void test_filtro(void)
{
	char ruta256[256];

	assert(ruta_en_directorio("/d/v/a.txt", "/d/v/", 5) == 1);
	assert(ruta_en_directorio("/d/v/sub/a.txt", "/d/v/", 5) == 0);
	assert(ruta_en_directorio("/d/v/", "/d/v/", 5) == 0);
	assert(ruta_en_directorio("/d/va/a.txt", "/d/v/", 5) == 0);
	assert(ruta_en_directorio("/d/a.txt", "/d/v/", 5) == 0);
	assert(ruta_en_directorio("d/v/a.txt", "/d/v/", 5) == 0);
	assert(ruta_en_directorio("/d/v/a.txt", "/d/v/", 0) == 0);

	memset(ruta256, 'a', sizeof(ruta256));
	memcpy(ruta256, "/d/v/f", 6);
	assert(ruta_en_directorio(ruta256, "/d/v/", 5) == 0);

	printf("\033[0;32m✓ Caso 1: Filtro ruta_en_directorio valida prefijo y descarta subdirectorios\033[0m\n");
}

static void test_sha256(const char *tmpdir)
{
	char ruta_vacio[SENSOR_RUTA_MAX];
	char ruta_abc[SENSOR_RUTA_MAX];
	char ruta_1m[SENSOR_RUTA_MAX];
	char hex[65];
	char bloque_a[4096];
	size_t escrito;
	int fd;

	snprintf(ruta_vacio, sizeof(ruta_vacio), "%s/vacio.txt", tmpdir);
	fd = open(ruta_vacio, O_CREAT | O_RDWR | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(analisis_sha256_fd(fd, hex) == 0);
	close(fd);
	assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

	snprintf(ruta_abc, sizeof(ruta_abc), "%s/abc.txt", tmpdir);
	fd = open(ruta_abc, O_CREAT | O_RDWR | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "abc", 3) == 3);
	assert(lseek(fd, 0, SEEK_SET) == 0);
	assert(analisis_sha256_fd(fd, hex) == 0);
	close(fd);
	assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

	snprintf(ruta_1m, sizeof(ruta_1m), "%s/1m.txt", tmpdir);
	fd = open(ruta_1m, O_CREAT | O_RDWR | O_TRUNC, 0644);
	assert(fd >= 0);
	memset(bloque_a, 'a', sizeof(bloque_a));
	escrito = 0;
	while (escrito < 1000000) {
		size_t por_escribir = 1000000 - escrito;
		ssize_t n;
		if (por_escribir > sizeof(bloque_a))
			por_escribir = sizeof(bloque_a);
		n = write(fd, bloque_a, por_escribir);
		assert(n > 0);
		escrito += (size_t)n;
	}
	assert(lseek(fd, 0, SEEK_SET) == 0);
	assert(analisis_sha256_fd(fd, hex) == 0);
	close(fd);
	assert(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);

	printf("\033[0;32m✓ Caso 2: SHA-256 con EVP calcula hashes exactos de archivos\033[0m\n");
}

static void test_rutas(const char *tmpdir)
{
	char canonica[PATH_MAX];
	char ruta_reg[SENSOR_RUTA_MAX];
	char dir_norm[128];
	char dir_norm_barra[128];
	char enlace_dir[128];
	char ruta_larga[PATH_MAX];
	char dir_base_largo[PATH_MAX];
	int fd;

	assert(analisis_validar_ruta("", canonica, sizeof(canonica)) == RUTA_INVALIDA);
	assert(analisis_validar_ruta("relativa", canonica, sizeof(canonica)) == RUTA_INVALIDA);
	assert(analisis_validar_ruta("/", canonica, sizeof(canonica)) == RUTA_INVALIDA);
	assert(analisis_validar_ruta("/tmp/so2_inexistente_987654321", canonica, sizeof(canonica)) == RUTA_NO_DIRECTORIO);

	snprintf(ruta_reg, sizeof(ruta_reg), "%s/reg.txt", tmpdir);
	fd = open(ruta_reg, O_CREAT | O_WRONLY, 0644);
	assert(fd >= 0);
	close(fd);
	assert(analisis_validar_ruta(ruta_reg, canonica, sizeof(canonica)) == RUTA_NO_DIRECTORIO);

	snprintf(dir_norm, sizeof(dir_norm), "%s/dir_norm", tmpdir);
	assert(mkdir(dir_norm, 0755) == 0);
	snprintf(dir_norm_barra, sizeof(dir_norm_barra), "%s/./dir_norm/./", tmpdir);
	assert(analisis_validar_ruta(dir_norm_barra, canonica, sizeof(canonica)) == RUTA_OK);
	assert(strcmp(canonica, dir_norm) == 0);

	snprintf(enlace_dir, sizeof(enlace_dir), "%s/enlace_a_dir", tmpdir);
	assert(symlink(dir_norm, enlace_dir) == 0);
	assert(analisis_validar_ruta(enlace_dir, canonica, sizeof(canonica)) == RUTA_OK);
	assert(strcmp(canonica, dir_norm) == 0);

	snprintf(dir_base_largo, sizeof(dir_base_largo), "%s/largo", tmpdir);
	assert(mkdir(dir_base_largo, 0755) == 0);
	snprintf(ruta_larga, sizeof(ruta_larga), "%s", dir_base_largo);
	while (strlen(ruta_larga) + 12 < 220) {
		strcat(ruta_larga, "/d12345678");
		mkdir(ruta_larga, 0755);
	}
	assert(strlen(ruta_larga) > 200);
	assert(analisis_validar_ruta(ruta_larga, canonica, sizeof(canonica)) == RUTA_LARGA);

	printf("\033[0;32m✓ Caso 3: Validacion de rutas clasifica correctamente segun las 4 reglas\033[0m\n");
}

static void test_procesar(const char *tmpdir)
{
	char dir_v[100];
	char ruta_f4[SENSOR_RUTA_MAX];
	char dir_sub[128];
	char ruta_sub_x[SENSOR_RUTA_MAX];
	char ruta_enlace[SENSOR_RUTA_MAX];
	char ruta_fifo[SENSOR_RUTA_MAX];
	char exp_desc[512];
	struct evento_archivo e;
	struct alerta ult[5];
	unsigned long gen;
	int fd;

	snprintf(dir_v, sizeof(dir_v), "%s/v", tmpdir);
	assert(mkdir(dir_v, 0755) == 0);

	analisis_reiniciar();
	alertas_reiniciar();
	gen = analisis_generacion();

	snprintf(ruta_f4, sizeof(ruta_f4), "%s/f4.txt", dir_v);
	fd = open(ruta_f4, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, "contenido original", 18) == 18);
	close(fd);

	memset(&e, 0, sizeof(e));
	e.pid = 4242;
	e.flags = O_WRONLY | O_CREAT;
	strncpy(e.proceso, "prueba", sizeof(e.proceso));
	snprintf(e.ruta, sizeof(e.ruta), "%s", ruta_f4);

	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_CREADO);
	assert(alertas_recientes(ult, 5) == 1);
	assert(ult[0].motivo == MOT_ARCHIVO_MODIFICADO);
	snprintf(exp_desc, sizeof(exp_desc), "El archivo %s fue creado por el PID 4242 (prueba)", e.ruta);
	assert(strcmp(ult[0].descripcion, exp_desc) == 0);

	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_SIN_CAMBIO);
	assert(alertas_recientes(ult, 5) == 1);

	fd = open(ruta_f4, O_WRONLY | O_TRUNC);
	assert(fd >= 0);
	assert(write(fd, "nuevo contenido modificado", 26) == 26);
	close(fd);

	e.flags = O_WRONLY;
	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_MODIFICADO);
	assert(alertas_recientes(ult, 5) == 2);
	snprintf(exp_desc, sizeof(exp_desc), "El archivo %s fue modificado por el PID 4242 (prueba)", e.ruta);
	assert(strcmp(ult[0].descripcion, exp_desc) == 0);

	e.flags = O_RDONLY;
	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_IGNORADO);
	assert(alertas_recientes(ult, 5) == 2);

	snprintf(dir_sub, sizeof(dir_sub), "%s/sub", dir_v);
	assert(mkdir(dir_sub, 0755) == 0);
	snprintf(ruta_sub_x, sizeof(ruta_sub_x), "%s/x", dir_sub);
	fd = open(ruta_sub_x, O_CREAT | O_WRONLY, 0644);
	assert(fd >= 0);
	close(fd);

	snprintf(e.ruta, sizeof(e.ruta), "%s", ruta_sub_x);
	e.flags = O_WRONLY;
	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_IGNORADO);

	snprintf(e.ruta, sizeof(e.ruta), "%s", dir_sub);
	e.flags = O_WRONLY;
	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_IGNORADO);

	snprintf(ruta_enlace, sizeof(ruta_enlace), "%s/enlace.txt", dir_v);
	assert(symlink(ruta_f4, ruta_enlace) == 0);
	snprintf(e.ruta, sizeof(e.ruta), "%s", ruta_enlace);
	e.flags = O_WRONLY;
	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_IGNORADO);

	snprintf(ruta_fifo, sizeof(ruta_fifo), "%s/fifo_prueba", dir_v);
	assert(mkfifo(ruta_fifo, 0644) == 0);
	snprintf(e.ruta, sizeof(e.ruta), "%s", ruta_fifo);
	e.flags = O_WRONLY;
	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_IGNORADO);

	snprintf(e.ruta, sizeof(e.ruta), "%s/inexistente.txt", dir_v);
	e.flags = O_WRONLY;
	assert(analisis_procesar(dir_v, gen, &e) == ANALISIS_IGNORADO);

	printf("\033[0;32m✓ Caso 4: analisis_procesar detecta creacion, modificacion, sin cambio e ignorados\033[0m\n");
}

static void test_generacion(const char *tmpdir)
{
	char dir_v[100];
	struct evento_archivo e;
	struct archivo_analizado lista_antes[5];
	struct archivo_analizado lista_despues[5];
	size_t n_antes;
	size_t n_despues;
	unsigned long gen;

	snprintf(dir_v, sizeof(dir_v), "%s/v", tmpdir);
	gen = analisis_generacion();

	memset(&e, 0, sizeof(e));
	e.pid = 4242;
	e.flags = O_WRONLY;
	strncpy(e.proceso, "prueba", sizeof(e.proceso));
	snprintf(e.ruta, sizeof(e.ruta), "%s/f4.txt", dir_v);

	n_antes = analisis_listado(lista_antes, 5);
	assert(n_antes > 0);

	assert(analisis_procesar(dir_v, gen - 1, &e) == ANALISIS_IGNORADO);

	n_despues = analisis_listado(lista_despues, 5);
	assert(n_antes == n_despues);
	assert(lista_antes[0].seq == lista_despues[0].seq);

	printf("\033[0;32m✓ Caso 5: Generacion obsoleta ignora eventos y no altera el listado\033[0m\n");
}

static void test_linea_base(const char *tmpdir)
{
	char dir_v6[100];
	char ruta[SENSOR_RUTA_MAX];
	struct archivo_analizado lista[10];
	struct alerta ult[5];
	unsigned long g6;
	size_t n_base;
	size_t n6;
	int fd;

	snprintf(dir_v6, sizeof(dir_v6), "%s/v6", tmpdir);
	assert(mkdir(dir_v6, 0755) == 0);

	for (int i = 1; i <= 3; i++) {
		snprintf(ruta, sizeof(ruta), "%s/f%d.txt", dir_v6, i);
		fd = open(ruta, O_CREAT | O_WRONLY, 0644);
		assert(fd >= 0);
		assert(write(fd, "base", 4) == 4);
		close(fd);
	}

	snprintf(ruta, sizeof(ruta), "%s/sub6", dir_v6);
	assert(mkdir(ruta, 0755) == 0);

	snprintf(ruta, sizeof(ruta), "%s/link6", dir_v6);
	assert(symlink("f1.txt", ruta) == 0);

	analisis_reiniciar();
	alertas_reiniciar();
	g6 = analisis_generacion();

	n_base = analisis_linea_base(dir_v6, g6);
	assert(n_base == 3);
	assert(alertas_recientes(ult, 5) == 0);

	n6 = analisis_listado(lista, 10);
	assert(n6 == 3);
	for (size_t i = 0; i < n6; i++) {
		assert(lista[i].accion == ACCION_INICIAL);
		assert(lista[i].pid == 0);
		assert(lista[i].proceso[0] == '\0');
	}

	analisis_reiniciar();
	assert(analisis_listado(lista, 10) == 0);
	assert(analisis_generacion() == g6 + 1);

	printf("\033[0;32m✓ Caso 6: Analisis inicial procesa solo archivos regulares y analisis_reiniciar vacia\033[0m\n");
}

static void test_limite(const char *tmpdir)
{
	char dir_v7[100];
	char ruta_f0[SENSOR_RUTA_MAX];
	char ruta_f256[SENSOR_RUTA_MAX];
	struct archivo_analizado *arr7;
	unsigned long g7;
	size_t n7;
	int encontrado_f0;
	int encontrado_f256;

	snprintf(dir_v7, sizeof(dir_v7), "%s/v7", tmpdir);
	assert(mkdir(dir_v7, 0755) == 0);

	analisis_reiniciar();
	g7 = analisis_generacion();

	for (int i = 0; i < 257; i++) {
		char rf[SENSOR_RUTA_MAX];
		struct evento_archivo ev7;
		int fd;

		snprintf(rf, sizeof(rf), "%s/f_%03d.txt", dir_v7, i);
		fd = open(rf, O_CREAT | O_WRONLY, 0644);
		assert(fd >= 0);
		assert(write(fd, "x", 1) == 1);
		close(fd);

		memset(&ev7, 0, sizeof(ev7));
		ev7.pid = 1000 + i;
		ev7.flags = O_WRONLY | O_CREAT;
		strncpy(ev7.proceso, "limite", sizeof(ev7.proceso));
		snprintf(ev7.ruta, sizeof(ev7.ruta), "%s", rf);
		assert(analisis_procesar(dir_v7, g7, &ev7) == ANALISIS_CREADO);
	}

	arr7 = malloc(sizeof(struct archivo_analizado) * 300);
	assert(arr7 != NULL);
	n7 = analisis_listado(arr7, 300);
	assert(n7 == 256);

	snprintf(ruta_f0, sizeof(ruta_f0), "%s/f_000.txt", dir_v7);
	encontrado_f0 = 0;
	for (size_t i = 0; i < n7; i++) {
		if (strcmp(arr7[i].ruta, ruta_f0) == 0)
			encontrado_f0 = 1;
	}
	assert(encontrado_f0 == 0);

	snprintf(ruta_f256, sizeof(ruta_f256), "%s/f_256.txt", dir_v7);
	encontrado_f256 = 0;
	for (size_t i = 0; i < n7; i++) {
		if (strcmp(arr7[i].ruta, ruta_f256) == 0)
			encontrado_f256 = 1;
	}
	assert(encontrado_f256 == 1);

	free(arr7);
	printf("\033[0;32m✓ Caso 7: Limite de 256 entradas descarta la mas antigua\033[0m\n");
}

static void test_json(const char *tmpdir)
{
	struct buf b = {0};
	cJSON *root;
	cJSON *activo;
	cJSON *ruta_item;
	cJSON *arr;
	cJSON *item0;
	struct archivo_analizado *asc;
	struct archivo_analizado *desc;
	int arr_sz;
	size_t n_asc;
	size_t n_desc;

	analisis_publicar_sensor(1, tmpdir, "");
	json_archivos(&b);

	root = cJSON_Parse(b.datos);
	assert(root != NULL);

	activo = cJSON_GetObjectItem(root, "activo");
	assert(activo && cJSON_IsTrue(activo));

	ruta_item = cJSON_GetObjectItem(root, "ruta");
	assert(ruta_item && cJSON_IsString(ruta_item) && strcmp(ruta_item->valuestring, tmpdir) == 0);

	arr = cJSON_GetObjectItem(root, "archivos");
	assert(arr && cJSON_IsArray(arr));

	arr_sz = cJSON_GetArraySize(arr);
	assert(arr_sz > 0);

	item0 = cJSON_GetArrayItem(arr, 0);
	assert(cJSON_GetObjectItem(item0, "ruta") != NULL);
	assert(cJSON_GetObjectItem(item0, "nombre") != NULL);
	assert(cJSON_GetObjectItem(item0, "sha256") != NULL);
	assert(cJSON_GetObjectItem(item0, "firma") != NULL);
	assert(cJSON_GetObjectItem(item0, "tamano") != NULL);
	assert(cJSON_GetObjectItem(item0, "mtime") != NULL);
	assert(cJSON_GetObjectItem(item0, "estado") != NULL);
	assert(cJSON_GetObjectItem(item0, "cuarentena") != NULL);
	assert(cJSON_GetObjectItem(item0, "accion") != NULL);
	assert(cJSON_GetObjectItem(item0, "pid") != NULL);
	assert(cJSON_GetObjectItem(item0, "proceso") != NULL);
	assert(cJSON_GetObjectItem(item0, "analizado") != NULL);

	asc = malloc(sizeof(struct archivo_analizado) * arr_sz);
	desc = malloc(sizeof(struct archivo_analizado) * arr_sz);
	assert(asc && desc);

	n_asc = analisis_desde(0, asc, (size_t)arr_sz);
	n_desc = analisis_listado(desc, (size_t)arr_sz);
	assert(n_asc == (size_t)arr_sz);
	assert(n_desc == (size_t)arr_sz);

	for (size_t i = 0; i + 1 < (size_t)arr_sz; i++) {
		assert(asc[i].seq <= asc[i + 1].seq);
		assert(desc[i].seq >= desc[i + 1].seq);
	}
	assert(asc[0].seq == desc[arr_sz - 1].seq);

	free(asc);
	free(desc);
	cJSON_Delete(root);
	buf_liberar(&b);

	printf("\033[0;32m✓ Caso 8: JSON parseable con 12 claves y ordenamiento correcto de seq\033[0m\n");
}

static void copiar_archivo(const char *origen, const char *destino)
{
	int in = open(origen, O_RDONLY);
	assert(in >= 0);
	int out = open(destino, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(out >= 0);
	char buf[4096];
	ssize_t n;
	while ((n = read(in, buf, sizeof(buf))) > 0)
		assert(write(out, buf, (size_t)n) == n);
	close(in);
	close(out);
}

static void test_caso_9_deteccion_evento(const char *tmpdir)
{
	const char *bl_repo = "blacklist.json";
	struct stat st_bl;
	if (stat(bl_repo, &st_bl) < 0)
		bl_repo = "ProgramaIntermedio/blacklist.json";

	char bl_temp[PATH_MAX];
	snprintf(bl_temp, sizeof(bl_temp), "%s/bl.json", tmpdir);
	copiar_archivo(bl_repo, bl_temp);

	assert(blacklist_iniciar(bl_temp) == 0);
	struct info_blacklist bl_info;
	blacklist_info(&bl_info);
	assert(bl_info.firmas == 4);

	alertas_reiniciar();
	analisis_reiniciar();
	unsigned long gen = analisis_generacion();

	char ruta[SENSOR_RUTA_MAX];
	snprintf(ruta, sizeof(ruta), "%s/troyano.txt", tmpdir);
	int fd = open(ruta, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	const char *payload = "SO2-MUESTRA-MALICIOSA-1\n";
	assert(write(fd, payload, strlen(payload)) == (ssize_t)strlen(payload));
	close(fd);
	assert(chmod(ruta, 0644) == 0);

	struct evento_archivo ev;
	memset(&ev, 0, sizeof(ev));
	snprintf(ev.ruta, sizeof(ev.ruta), "%s", ruta);
	ev.flags = O_WRONLY | O_CREAT;
	ev.pid = 1234;
	snprintf(ev.proceso, sizeof(ev.proceso), "%s", "troyano_proc");
	assert(analisis_procesar(tmpdir, gen, &ev) == ANALISIS_CREADO);

	struct archivo_analizado lista[10];
	size_t n = analisis_listado(lista, 10);
	assert(n == 1);
	assert(lista[0].estado == ARCHIVO_INFECTADO);
	assert(strcmp(lista[0].firma, "SO2.Demo.Troyano") == 0);
	assert(lista[0].cuarentena == 1);

	struct stat st;
	assert(stat(ruta, &st) == 0);
	assert((st.st_mode & 07777) == 0);

	struct alerta alts[10];
	size_t na = alertas_recientes(alts, 10);
	assert(na == 2);
	assert(alts[1].motivo == MOT_HASH_MALICIOSO);
	assert(alts[1].severidad == SEV_HIGH);
	char exp1[512];
	snprintf(exp1, sizeof(exp1), "Firma SO2.Demo.Troyano detectada en %s (PID 1234, troyano_proc)", ruta);
	assert(strcmp(alts[1].descripcion, exp1) == 0);

	assert(alts[0].motivo == MOT_ARCHIVO_CUARENTENA);
	assert(alts[0].severidad == SEV_HIGH);
	char exp2[512];
	snprintf(exp2, sizeof(exp2), "Archivo %s puesto en cuarentena automática (firma SO2.Demo.Troyano)", ruta);
	assert(strcmp(alts[0].descripcion, exp2) == 0);

	for (size_t i = 0; i < na; i++)
		assert(alts[i].motivo != MOT_ARCHIVO_MODIFICADO);

	struct amenaza ams[10];
	size_t nam = analisis_amenazas(ams, 10);
	assert(nam >= 1);
	assert(strcmp(ams[0].firma, "SO2.Demo.Troyano") == 0);
	assert(ams[0].cuarentena == 1);
	assert(strcmp(ams[0].ruta, ruta) == 0);
	assert(ams[0].pid == 1234);
	assert(strcmp(ams[0].proceso, "troyano_proc") == 0);

	struct registro_cuarentena rcs[10];
	size_t nrc = analisis_cuarentena(rcs, 10);
	assert(nrc >= 1);
	assert(strcmp(rcs[0].tipo, "automatica") == 0);
	assert(strcmp(rcs[0].motivo, "SO2.Demo.Troyano") == 0);
	assert(strcmp(rcs[0].permisos_anteriores, "644") == 0);

	printf("\033[0;32m✓ Caso 9: Evento con troyano produce infectado, alertas HIGH, amenaza y cuarentena\033[0m\n");
}

static void test_caso_10_linea_base_muestra(const char *tmpdir)
{
	alertas_reiniciar();
	analisis_reiniciar();
	unsigned long gen = analisis_generacion();

	char ruta[SENSOR_RUTA_MAX];
	snprintf(ruta, sizeof(ruta), "%s/ransomware.txt", tmpdir);
	int fd = open(ruta, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	const char *payload = "SO2-MUESTRA-MALICIOSA-2\n";
	assert(write(fd, payload, strlen(payload)) == (ssize_t)strlen(payload));
	close(fd);
	assert(chmod(ruta, 0644) == 0);

	assert(analisis_linea_base(tmpdir, gen) > 0);

	struct stat st;
	assert(stat(ruta, &st) == 0);
	assert((st.st_mode & 07777) == 0);

	struct alerta alts[10];
	size_t na = alertas_recientes(alts, 10);
	int hallada = 0;
	for (size_t i = 0; i < na; i++) {
		if (alts[i].motivo == MOT_HASH_MALICIOSO && strstr(alts[i].descripcion, "(análisis inicial)"))
			hallada = 1;
	}
	assert(hallada == 1);

	printf("\033[0;32m✓ Caso 10: Linea base con muestra aplica sufijo (análisis inicial) y cuarentena\033[0m\n");
}

static void test_caso_11_reevaluacion(const char *tmpdir)
{
	alertas_reiniciar();
	analisis_reiniciar();
	unsigned long gen = analisis_generacion();

	char ruta[SENSOR_RUTA_MAX];
	snprintf(ruta, sizeof(ruta), "%s/benigno.txt", tmpdir);
	int fd = open(ruta, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	const char *payload = "contenido benigno para prueba\n";
	assert(write(fd, payload, strlen(payload)) == (ssize_t)strlen(payload));
	close(fd);
	assert(chmod(ruta, 0644) == 0);

	fd = open(ruta, O_RDONLY);
	assert(fd >= 0);
	char sha_benigno[65];
	assert(analisis_sha256_fd(fd, sha_benigno) == 0);
	close(fd);

	assert(analisis_linea_base(tmpdir, gen) > 0);
	struct archivo_analizado l[10];
	size_t n = analisis_listado(l, 10);
	int idx_b = -1;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(l[i].ruta, ruta) == 0) {
			idx_b = (int)i;
			break;
		}
	}
	assert(idx_b >= 0);
	assert(l[idx_b].estado == ARCHIVO_BENIGNO);

	char json_nuevo[1024];
	snprintf(json_nuevo, sizeof(json_nuevo),
		 "{\"nombre\":\"Base Extra\",\"version\":2,\"firmas\":[{\"sha256\":\"%s\",\"nombre\":\"SO2.Demo.BenignoTest\",\"descripcion\":\"Test desc\"}]}",
		 sha_benigno);
	char err[256];
	size_t cnt = 0;
	assert(blacklist_actualizar(json_nuevo, strlen(json_nuevo), "admin", &cnt, err, sizeof(err)) == 0);

	alertas_reiniciar();
	assert(analisis_reevaluar(tmpdir, gen) > 0);

	n = analisis_listado(l, 10);
	idx_b = -1;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(l[i].ruta, ruta) == 0) {
			idx_b = (int)i;
			break;
		}
	}
	assert(idx_b >= 0);
	assert(l[idx_b].estado == ARCHIVO_INFECTADO);
	assert(strcmp(l[idx_b].firma, "SO2.Demo.BenignoTest") == 0);
	assert(l[idx_b].cuarentena == 1);

	struct stat st;
	assert(stat(ruta, &st) == 0);
	assert((st.st_mode & 07777) == 0);

	struct alerta alts[10];
	size_t na = alertas_recientes(alts, 10);
	int hallada = 0;
	for (size_t i = 0; i < na; i++) {
		assert(alts[i].motivo != MOT_ARCHIVO_MODIFICADO);
		if (alts[i].motivo == MOT_HASH_MALICIOSO && strstr(alts[i].descripcion, "(actualización de blacklist)"))
			hallada = 1;
	}
	assert(hallada == 1);

	const char *json_sin = "{\"nombre\":\"Base Sin\",\"version\":3,\"firmas\":[{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"Troyano\",\"descripcion\":\"D\"}]}";
	assert(blacklist_actualizar(json_sin, strlen(json_sin), "admin", &cnt, err, sizeof(err)) == 0);

	alertas_reiniciar();
	assert(analisis_reevaluar(tmpdir, gen) > 0);

	n = analisis_listado(l, 10);
	idx_b = -1;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(l[i].ruta, ruta) == 0) {
			idx_b = (int)i;
			break;
		}
	}
	assert(idx_b >= 0);
	assert(l[idx_b].estado == ARCHIVO_BENIGNO);
	assert(l[idx_b].firma[0] == '\0');
	assert(l[idx_b].cuarentena == 1);

	na = alertas_recientes(alts, 10);
	for (size_t i = 0; i < na; i++)
		assert(alts[i].motivo != MOT_ARCHIVO_MODIFICADO);

	printf("\033[0;32m✓ Caso 11: Re-evaluacion detecta firma nueva y revierte a benigno sin restaurar cuarentena\033[0m\n");
}

static void test_caso_12_ya_en_cuarentena(const char *tmpdir)
{
	unsigned long gen = analisis_generacion();
	struct registro_cuarentena rcs[MAX_CUARENTENA];
	size_t nrc_before = analisis_cuarentena(rcs, MAX_CUARENTENA);

	assert(analisis_reevaluar(tmpdir, gen) > 0);
	size_t nrc_after = analisis_cuarentena(rcs, MAX_CUARENTENA);
	assert(nrc_after == nrc_before);

	printf("\033[0;32m✓ Caso 12: Archivo ya en 0000 no duplica registro de cuarentena ni repite fchmod\033[0m\n");
}

static void test_caso_13_cuarentena_manual(const char *tmpdir)
{
	analisis_reiniciar();
	unsigned long gen = analisis_generacion();
	analisis_publicar_sensor(1, tmpdir, "");

	char ruta_man[SENSOR_RUTA_MAX];
	snprintf(ruta_man, sizeof(ruta_man), "%s/manual_test.txt", tmpdir);
	int fd = open(ruta_man, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	const char *payload = "prueba manual\n";
	assert(write(fd, payload, strlen(payload)) == (ssize_t)strlen(payload));
	close(fd);
	assert(chmod(ruta_man, 0644) == 0);

	assert(analisis_linea_base(tmpdir, gen) > 0);

	struct stat st;
	assert(stat(ruta_man, &st) == 0);
	assert((st.st_mode & 07777) == 0644);

	alertas_reiniciar();
	assert(analisis_cuarentena_manual(ruta_man, "prueba") == CUARENTENA_OK);

	assert(stat(ruta_man, &st) == 0);
	assert((st.st_mode & 07777) == 0);

	struct alerta alts[10];
	size_t na = alertas_recientes(alts, 10);
	assert(na > 0);
	assert(strstr(alts[0].descripcion, "manual por prueba") != NULL);

	assert(analisis_cuarentena_manual(ruta_man, "prueba") == CUARENTENA_YA);

	assert(analisis_cuarentena_manual("/tmp/archivo_fuera_del_listado_9999.txt", "prueba") == CUARENTENA_NO_LISTADO);

	char ruta_link[SENSOR_RUTA_MAX];
	snprintf(ruta_link, sizeof(ruta_link), "%s/enlace.txt", tmpdir);
	assert(symlink(ruta_man, ruta_link) == 0);
	assert(analisis_cuarentena_manual(ruta_link, "prueba") == CUARENTENA_NO_LISTADO);
	unlink(ruta_link);

	printf("\033[0;32m✓ Caso 13: Cuarentena manual cubre exito, YA, ruta fuera y enlace simbolico\033[0m\n");
}

static void test_caso_14_reiniciar_no_vacia(void)
{
	struct amenaza ams[10];
	size_t nam1 = analisis_amenazas(ams, 10);
	struct registro_cuarentena rcs[10];
	size_t nrc1 = analisis_cuarentena(rcs, 10);
	assert(nam1 > 0);
	assert(nrc1 > 0);

	analisis_reiniciar();

	size_t nam2 = analisis_amenazas(ams, 10);
	size_t nrc2 = analisis_cuarentena(rcs, 10);
	assert(nam2 == nam1);
	assert(nrc2 == nrc1);

	printf("\033[0;32m✓ Caso 14: analisis_reiniciar no vacia amenazas ni registros de cuarentena\033[0m\n");
}

static void test_caso_15_json_y_claves(const char *tmpdir)
{
	struct buf b = { 0 };
	struct archivo_analizado a = { 0 };
	snprintf(a.ruta, sizeof(a.ruta), "%s/test.txt", tmpdir);
	snprintf(a.sha256, sizeof(a.sha256), "8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660");
	snprintf(a.firma, sizeof(a.firma), "SO2.Demo.Troyano");
	a.tamano = 100;
	a.estado = ARCHIVO_INFECTADO;
	a.cuarentena = 1;
	a.accion = ACCION_MODIFICADO;
	a.pid = 999;
	snprintf(a.proceso, sizeof(a.proceso), "proc");
	snprintf(a.analizado, sizeof(a.analizado), "2026-10-21 12:00:00");

	json_archivo(&b, &a);
	cJSON *root = cJSON_Parse(b.datos);
	assert(root != NULL);
	assert(cJSON_GetArraySize(root) == 12);
	assert(cJSON_GetObjectItem(root, "ruta") != NULL);
	assert(cJSON_GetObjectItem(root, "nombre") != NULL);
	assert(cJSON_GetObjectItem(root, "sha256") != NULL);
	assert(cJSON_GetObjectItem(root, "firma") != NULL);
	assert(cJSON_GetObjectItem(root, "tamano") != NULL);
	assert(cJSON_GetObjectItem(root, "mtime") != NULL);
	assert(cJSON_GetObjectItem(root, "estado") != NULL);
	assert(cJSON_GetObjectItem(root, "cuarentena") != NULL);
	assert(cJSON_GetObjectItem(root, "accion") != NULL);
	assert(cJSON_GetObjectItem(root, "pid") != NULL);
	assert(cJSON_GetObjectItem(root, "proceso") != NULL);
	assert(cJSON_GetObjectItem(root, "analizado") != NULL);
	cJSON_Delete(root);
	buf_liberar(&b);

	struct amenaza am = { 0 };
	am.id = 1;
	snprintf(am.firma, sizeof(am.firma), "Firma1");
	snprintf(am.descripcion, sizeof(am.descripcion), "Desc1");
	snprintf(am.ruta, sizeof(am.ruta), "/a/b");
	snprintf(am.sha256, sizeof(am.sha256), "8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660");
	am.pid = 123;
	snprintf(am.proceso, sizeof(am.proceso), "p");
	snprintf(am.fecha, sizeof(am.fecha), "2026-10-21 12:00:00");
	am.cuarentena = 1;

	json_amenaza(&b, &am);
	root = cJSON_Parse(b.datos);
	assert(root != NULL);
	assert(cJSON_GetObjectItem(root, "id") != NULL);
	assert(cJSON_GetObjectItem(root, "firma") != NULL);
	assert(cJSON_GetObjectItem(root, "descripcion") != NULL);
	assert(cJSON_GetObjectItem(root, "cuarentena") != NULL);
	cJSON_Delete(root);
	buf_liberar(&b);

	struct registro_cuarentena rc = { 0 };
	snprintf(rc.ruta, sizeof(rc.ruta), "/a/b");
	snprintf(rc.sha256, sizeof(rc.sha256), "8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660");
	snprintf(rc.tipo, sizeof(rc.tipo), "manual");
	snprintf(rc.motivo, sizeof(rc.motivo), "manual");
	snprintf(rc.usuario, sizeof(rc.usuario), "user");
	snprintf(rc.fecha, sizeof(rc.fecha), "2026-10-21 12:00:00");
	snprintf(rc.permisos_anteriores, sizeof(rc.permisos_anteriores), "644");

	json_registro_cuarentena(&b, &rc);
	root = cJSON_Parse(b.datos);
	assert(root != NULL);
	assert(cJSON_GetObjectItem(root, "tipo") != NULL);
	assert(cJSON_GetObjectItem(root, "permisos_anteriores") != NULL);
	cJSON_Delete(root);
	buf_liberar(&b);

	printf("\033[0;32m✓ Caso 15: json_archivo tiene 12 claves; json_amenaza y json_registro_cuarentena parseables\033[0m\n");
}

static void test_caso_16_enlace_directorio(const char *tmpdir)
{
	char dir_d[128];
	char dir_d_viejo[128];
	char dir_e[128];
	char ruta_d_f[256];
	char ruta_e_f[256];
	char ruta_viejo_f[256];
	char sha_original[65] = "";
	struct stat st;
	int fd;

	snprintf(dir_d, sizeof(dir_d), "%s/dir_d", tmpdir);
	snprintf(dir_d_viejo, sizeof(dir_d_viejo), "%s/dir_d_viejo", tmpdir);
	snprintf(dir_e, sizeof(dir_e), "%s/dir_e", tmpdir);
	snprintf(ruta_d_f, sizeof(ruta_d_f), "%s/f", dir_d);
	snprintf(ruta_e_f, sizeof(ruta_e_f), "%s/f", dir_e);

	assert(mkdir(dir_d, 0755) == 0);
	assert(mkdir(dir_e, 0755) == 0);

	fd = open(ruta_d_f, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	const char *benigno = "contenido benigno original\n";
	assert(write(fd, benigno, strlen(benigno)) == (ssize_t)strlen(benigno));
	close(fd);
	assert(chmod(ruta_d_f, 0644) == 0);

	fd = open(ruta_e_f, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	const char *malicioso = "SO2-MUESTRA-MALICIOSA-1\n";
	assert(write(fd, malicioso, strlen(malicioso)) == (ssize_t)strlen(malicioso));
	close(fd);
	assert(chmod(ruta_e_f, 0644) == 0);

	char bl_temp[PATH_MAX];
	snprintf(bl_temp, sizeof(bl_temp), "%s/bl16.json", tmpdir);
	const char *json_bl = "{\"nombre\":\"Base 16\",\"version\":1,\"firmas\":[{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"SO2.Demo.Troyano\",\"descripcion\":\"Troyano demo\"}]}";
	fd = open(bl_temp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, json_bl, strlen(json_bl)) == (ssize_t)strlen(json_bl));
	close(fd);
	assert(blacklist_iniciar(bl_temp) == 0);

	analisis_reiniciar();
	unsigned long gen = analisis_generacion();
	analisis_publicar_sensor(1, dir_d, "");

	assert(analisis_linea_base(dir_d, gen) == 1);

	struct archivo_analizado l[10];
	size_t n = analisis_listado(l, 10);
	assert(n == 1);
	assert(strcmp(l[0].ruta, ruta_d_f) == 0);
	assert(l[0].estado == ARCHIVO_BENIGNO);
	assert(l[0].cuarentena == 0);
	snprintf(sha_original, sizeof(sha_original), "%s", l[0].sha256);

	assert(rename(dir_d, dir_d_viejo) == 0);
	assert(symlink(dir_e, dir_d) == 0);

	alertas_reiniciar();

	assert(analisis_cuarentena_manual(ruta_d_f, "test_admin") == CUARENTENA_NO_LISTADO);

	size_t reev = analisis_reevaluar(dir_d, gen);
	assert(reev == 0);

	assert(stat(ruta_e_f, &st) == 0);
	assert((st.st_mode & 07777) == 0644);

	struct alerta alts[10];
	size_t na = alertas_recientes(alts, 10);
	for (size_t i = 0; i < na; i++)
		assert(alts[i].severidad != SEV_HIGH);

	n = analisis_listado(l, 10);
	assert(n == 1);
	assert(strcmp(l[0].sha256, sha_original) == 0);
	assert(l[0].estado == ARCHIVO_BENIGNO);
	assert(l[0].cuarentena == 0);

	unlink(dir_d);
	unlink(ruta_e_f);
	rmdir(dir_e);
	snprintf(ruta_viejo_f, sizeof(ruta_viejo_f), "%s/f", dir_d_viejo);
	unlink(ruta_viejo_f);
	rmdir(dir_d_viejo);
	unlink(bl_temp);

	printf("\033[0;32m✓ Caso 16: Proteccion openat2 contra enlaces simbolicos en directorio vigilado\033[0m\n");
}

int main(void)
{
	char plantilla[] = "/tmp/test_analisis_XXXXXX";
	char *tmpdir = mkdtemp(plantilla);
	assert(tmpdir != NULL);

	printf("Iniciando pruebas de analisis de integridad:\n");
	test_filtro();
	test_sha256(tmpdir);
	test_rutas(tmpdir);
	test_procesar(tmpdir);
	test_generacion(tmpdir);
	test_linea_base(tmpdir);
	test_limite(tmpdir);
	test_json(tmpdir);
	test_caso_9_deteccion_evento(tmpdir);
	test_caso_10_linea_base_muestra(tmpdir);
	test_caso_11_reevaluacion(tmpdir);
	test_caso_12_ya_en_cuarentena(tmpdir);
	test_caso_13_cuarentena_manual(tmpdir);
	test_caso_14_reiniciar_no_vacia();
	test_caso_15_json_y_claves(tmpdir);
	test_caso_16_enlace_directorio(tmpdir);

	rm_rf(tmpdir);
	printf("Todas las pruebas de analisis pasaron exitosamente.\n");
	return 0;
}
