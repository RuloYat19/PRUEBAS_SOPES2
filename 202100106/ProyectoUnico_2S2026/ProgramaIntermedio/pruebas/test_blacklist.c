#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../comun.h"
#include "../lib/cJSON.h"

void registrar(const char *formato, ...)
{
	(void)formato;
}

static void test_caso_1_valido_repositorio(void)
{
	const char *ruta = "blacklist.json";
	struct stat st;
	if (stat(ruta, &st) < 0)
		ruta = "ProgramaIntermedio/blacklist.json";

	assert(blacklist_iniciar(ruta) == 0);
	struct info_blacklist info;
	blacklist_info(&info);

	assert(info.firmas == 4);
	assert(strcmp(info.nombre, "Base local de firmas SO2") == 0);
	assert(info.version == 1);
	assert(strcmp(info.actualizado, "2026-10-21") == 0);
	assert(strcmp(info.origen, "archivo") == 0);
	assert(info.error[0] == '\0');
	assert(blacklist_version() == 1);

	struct firma f;
	assert(blacklist_buscar("8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660", &f) == 1);
	assert(strcmp(f.nombre, "SO2.Demo.Troyano") == 0);
	assert(strstr(f.descripcion, "troyano") != NULL);

	assert(blacklist_buscar("501e1e69c486c462ff11499f2086dcde083a3743ded7bdc178137b12ad39845f", &f) == 1);
	assert(strcmp(f.nombre, "SO2.Demo.Ransomware") == 0);

	assert(blacklist_buscar("431caf694d5a52919d212d6f5c90091a8ce01a9b3d89cb927b81d65030788b4f", &f) == 1);
	assert(strcmp(f.nombre, "SO2.Demo.Keylogger") == 0);

	assert(blacklist_buscar("275a021bbfb6489e54d471899f7db9d1663fc695ec2fe2a2c4538aabf651fd0f", &f) == 1);
	assert(strcmp(f.nombre, "EICAR-Test-File") == 0);

	struct firma lista[4];
	assert(blacklist_lista(lista, 4) == 4);

	blacklist_finalizar();
}

static void test_caso_2_textos_error(void)
{
	char err[256];

	const char *j1 = "{invalid json";
	assert(blacklist_validar(j1, strlen(j1), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "JSON mal formado") == 0);

	const char *j2 = "[]";
	assert(blacklist_validar(j2, strlen(j2), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "La raiz debe ser un objeto") == 0);

	const char *j3 = "{\"desconocida\": 1, \"firmas\": []}";
	assert(blacklist_validar(j3, strlen(j3), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Clave desconocida: desconocida") == 0);

	const char *j4 = "{\"nombre\": \"Test\"}";
	assert(blacklist_validar(j4, strlen(j4), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Falta el arreglo firmas") == 0);

	const char *j5 = "{\"firmas\": []}";
	assert(blacklist_validar(j5, strlen(j5), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "firmas debe tener entre 1 y 5000 elementos") == 0);

	const char *j6a = "{\"nombre\": \"\", \"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"D\"}]}";
	assert(blacklist_validar(j6a, strlen(j6a), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Campo invalido: nombre") == 0);

	const char *j6b = "{\"version\": 0, \"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"D\"}]}";
	assert(blacklist_validar(j6b, strlen(j6b), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Campo invalido: version") == 0);

	const char *j6c = "{\"actualizado\": \"\", \"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"D\"}]}";
	assert(blacklist_validar(j6c, strlen(j6c), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Campo invalido: actualizado") == 0);

	const char *j7 = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"D\",\"extra\":1}]}";
	assert(blacklist_validar(j7, strlen(j7), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: clave desconocida extra") == 0);

	const char *j8a = "{\"firmas\": [{\"nombre\":\"T\",\"descripcion\":\"D\"}]}";
	assert(blacklist_validar(j8a, strlen(j8a), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: falta sha256") == 0);

	const char *j8b = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"descripcion\":\"D\"}]}";
	assert(blacklist_validar(j8b, strlen(j8b), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: falta nombre") == 0);

	const char *j8c = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\"}]}";
	assert(blacklist_validar(j8c, strlen(j8c), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: falta descripcion") == 0);

	const char *j9 = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626\",\"nombre\":\"T\",\"descripcion\":\"D\"}]}";
	assert(blacklist_validar(j9, strlen(j9), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: sha256 invalido") == 0);

	const char *j10 = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"\",\"descripcion\":\"D\"}]}";
	assert(blacklist_validar(j10, strlen(j10), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: nombre invalido") == 0);

	const char *j11 = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"\"}]}";
	assert(blacklist_validar(j11, strlen(j11), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: descripcion invalida") == 0);

	const char *j12 = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"D\"},{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T2\",\"descripcion\":\"D2\"}]}";
	assert(blacklist_validar(j12, strlen(j12), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 2: sha256 duplicado") == 0);
}

static void test_caso_3_mayusculas(void)
{
	char err[256];
	const char *j = "{\"firmas\": [{\"sha256\":\"8B5904E17BB98626A449E99313D77D5F49335F73A783C8B0800C3F1A8D9D3660\",\"nombre\":\"Troyano\",\"descripcion\":\"Desc\"}]}";
	struct tabla_firmas *t = NULL;
	struct info_blacklist info;

	assert(blacklist_validar(j, strlen(j), &t, &info, err, sizeof(err)) == 0);
	assert(t != NULL);
	tabla_firmas_liberar(t);

	char d[] = "/tmp/test_bl_XXXXXX";
	assert(mkdtemp(d) != NULL);
	char fpath[PATH_MAX];
	snprintf(fpath, sizeof(fpath), "%s/bl.json", d);

	int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, j, strlen(j)) == (ssize_t)strlen(j));
	close(fd);

	assert(blacklist_iniciar(fpath) == 0);
	struct firma f;
	assert(blacklist_buscar("8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660", &f) == 1);
	assert(strcmp(f.sha256, "8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660") == 0);
	assert(blacklist_buscar("8B5904E17BB98626A449E99313D77D5F49335F73A783C8B0800C3F1A8D9D3660", &f) == 1);

	blacklist_finalizar();
	unlink(fpath);
	rmdir(d);
}

static void test_caso_4_limites(void)
{
	char err[256];

	char nombre64[65];
	memset(nombre64, 'a', 64);
	nombre64[64] = '\0';

	char nombre65[66];
	memset(nombre65, 'a', 65);
	nombre65[65] = '\0';

	char desc256[257];
	memset(desc256, 'b', 256);
	desc256[256] = '\0';

	char desc257[258];
	memset(desc257, 'b', 257);
	desc257[257] = '\0';

	char j_buf[1024];
	snprintf(j_buf, sizeof(j_buf),
		 "{\"nombre\":\"%s\",\"firmas\":[{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"%s\",\"descripcion\":\"%s\"}]}",
		 nombre64, nombre64, desc256);
	assert(blacklist_validar(j_buf, strlen(j_buf), NULL, NULL, err, sizeof(err)) == 0);

	snprintf(j_buf, sizeof(j_buf),
		 "{\"nombre\":\"%s\",\"firmas\":[{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"D\"}]}",
		 nombre65);
	assert(blacklist_validar(j_buf, strlen(j_buf), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Campo invalido: nombre") == 0);

	snprintf(j_buf, sizeof(j_buf),
		 "{\"firmas\":[{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"%s\",\"descripcion\":\"D\"}]}",
		 nombre65);
	assert(blacklist_validar(j_buf, strlen(j_buf), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: nombre invalido") == 0);

	snprintf(j_buf, sizeof(j_buf),
		 "{\"firmas\":[{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T\",\"descripcion\":\"%s\"}]}",
		 desc257);
	assert(blacklist_validar(j_buf, strlen(j_buf), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "Firma 1: descripcion invalida") == 0);

	cJSON *root5000 = cJSON_CreateObject();
	cJSON *arr5000 = cJSON_CreateArray();
	cJSON_AddItemToObject(root5000, "firmas", arr5000);
	for (int i = 0; i < 5000; i++) {
		cJSON *fi = cJSON_CreateObject();
		char sha[65];
		snprintf(sha, sizeof(sha), "%064x", i);
		cJSON_AddStringToObject(fi, "sha256", sha);
		cJSON_AddStringToObject(fi, "nombre", "F");
		cJSON_AddStringToObject(fi, "descripcion", "D");
		cJSON_AddItemToArray(arr5000, fi);
	}
	char *str5000 = cJSON_PrintUnformatted(root5000);
	assert(str5000 != NULL);
	assert(blacklist_validar(str5000, strlen(str5000), NULL, NULL, err, sizeof(err)) == 0);

	cJSON *fi_extra = cJSON_CreateObject();
	char sha_extra[65];
	snprintf(sha_extra, sizeof(sha_extra), "%064x", 5000);
	cJSON_AddStringToObject(fi_extra, "sha256", sha_extra);
	cJSON_AddStringToObject(fi_extra, "nombre", "F");
	cJSON_AddStringToObject(fi_extra, "descripcion", "D");
	cJSON_AddItemToArray(arr5000, fi_extra);

	free(str5000);
	char *str5001 = cJSON_PrintUnformatted(root5000);
	assert(str5001 != NULL);
	assert(blacklist_validar(str5001, strlen(str5001), NULL, NULL, err, sizeof(err)) < 0);
	assert(strcmp(err, "firmas debe tener entre 1 y 5000 elementos") == 0);

	free(str5001);
	cJSON_Delete(root5000);
}

static void test_caso_5_buscar(void)
{
	blacklist_finalizar();
	struct firma f;
	assert(blacklist_buscar("8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660", &f) == 0);

	char d[] = "/tmp/test_bl_XXXXXX";
	assert(mkdtemp(d) != NULL);
	char fpath[PATH_MAX];
	snprintf(fpath, sizeof(fpath), "%s/bl.json", d);

	const char *j = "{\"firmas\": [{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"SO2.Demo.Troyano\",\"descripcion\":\"Desc\"}]}";
	int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(fd >= 0);
	assert(write(fd, j, strlen(j)) == (ssize_t)strlen(j));
	close(fd);

	assert(blacklist_iniciar(fpath) == 0);
	assert(blacklist_buscar("8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660", &f) == 1);
	assert(strcmp(f.nombre, "SO2.Demo.Troyano") == 0);
	assert(blacklist_buscar("1111111111111111111111111111111111111111111111111111111111111111", &f) == 0);

	blacklist_finalizar();
	assert(blacklist_buscar("8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660", &f) == 0);

	unlink(fpath);
	rmdir(d);
}

static void test_caso_6_actualizar_atomico(void)
{
	char d[] = "/tmp/test_bl_XXXXXX";
	assert(mkdtemp(d) != NULL);
	char fpath[PATH_MAX];
	snprintf(fpath, sizeof(fpath), "%s/bl.json", d);

	const char *j_ini = "{\"nombre\":\"Base 1\",\"version\":1,\"firmas\":[{\"sha256\":\"8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660\",\"nombre\":\"T1\",\"descripcion\":\"D1\"}]}";
	int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	assert(fd >= 0);
	assert(write(fd, j_ini, strlen(j_ini)) == (ssize_t)strlen(j_ini));
	close(fd);
	assert(chmod(fpath, 0640) == 0);

	assert(blacklist_iniciar(fpath) == 0);
	assert(blacklist_version() == 1);

	char err[256];
	size_t cant = 0;
	const char *j_invalido = "{\"firmas\": []}";
	assert(blacklist_actualizar(j_invalido, strlen(j_invalido), "admin", &cant, err, sizeof(err)) < 0);
	assert(blacklist_version() == 1);

	char leido[512] = "";
	fd = open(fpath, O_RDONLY);
	assert(fd >= 0);
	ssize_t n = read(fd, leido, sizeof(leido) - 1);
	assert(n > 0);
	leido[n] = '\0';
	close(fd);
	assert(strcmp(leido, j_ini) == 0);

	const char *j_valido = "{\"nombre\":\"Base 2\",\"version\":2,\"firmas\":[{\"sha256\":\"501e1e69c486c462ff11499f2086dcde083a3743ded7bdc178137b12ad39845f\",\"nombre\":\"T2\",\"descripcion\":\"D2\"}]}";
	assert(blacklist_actualizar(j_valido, strlen(j_valido), "admin", &cant, err, sizeof(err)) == 0);
	assert(cant == 1);
	assert(blacklist_version() == 2);

	struct stat st;
	assert(stat(fpath, &st) == 0);
	assert((st.st_mode & 07777) == 0640);

	memset(leido, 0, sizeof(leido));
	fd = open(fpath, O_RDONLY);
	assert(fd >= 0);
	n = read(fd, leido, sizeof(leido) - 1);
	assert(n > 0);
	leido[n] = '\0';
	close(fd);
	assert(strcmp(leido, j_valido) == 0);

	struct info_blacklist info;
	blacklist_info(&info);
	assert(strcmp(info.origen, "subida") == 0);
	assert(strcmp(info.actualizada_por, "admin") == 0);
	assert(info.version == 2);
	assert(info.firmas == 1);

	blacklist_finalizar();
	unlink(fpath);
	rmdir(d);
}

static void test_caso_7_archivo_inexistente(void)
{
	blacklist_finalizar();
	assert(blacklist_iniciar("/tmp/archivo_inexistente_so2_99999.json") == 0);
	struct info_blacklist info;
	blacklist_info(&info);
	assert(info.firmas == 0);
	assert(info.error[0] != '\0');
	assert(blacklist_version() == 1);

	struct firma f;
	assert(blacklist_buscar("8b5904e17bb98626a449e99313d77d5f49335f73a783c8b0800c3f1a8d9d3660", &f) == 0);

	blacklist_finalizar();
}

static void test_caso_8_diccionario(void)
{
	blacklist_finalizar();
	const char *jd = "{\"Sim.Threat\":{\"hash\":\"470EED5B6F546E420020E5B6E0BC2813022334EEF3C0ED4794FE030EB7822ADB\",\"descripcion\":\"Desc test\"}}";
	char err[256];
	struct tabla_firmas *t = NULL;
	struct info_blacklist info;
	assert(blacklist_validar(jd, strlen(jd), &t, &info, err, sizeof(err)) == 0);
	assert(info.firmas == 1);
	assert(t != NULL);
	tabla_firmas_liberar(t);
}

int main(void)
{
	test_caso_1_valido_repositorio();
	test_caso_2_textos_error();
	test_caso_3_mayusculas();
	test_caso_4_limites();
	test_caso_5_buscar();
	test_caso_6_actualizar_atomico();
	test_caso_7_archivo_inexistente();
	test_caso_8_diccionario();
	printf("test_blacklist: OK\n");
	return 0;
}
