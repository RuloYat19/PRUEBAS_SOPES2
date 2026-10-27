#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../comun.h"
#include "../lib/cJSON.h"

int fd_aviso = -1;
volatile sig_atomic_t ejecutando = 1;
struct estado estado = { .lock = PTHREAD_MUTEX_INITIALIZER };

void registrar(const char *formato, ...)
{
	(void)formato;
}

static void escribir_archivo(const char *ruta, const char *contenido)
{
	FILE *f = fopen(ruta, "w");
	assert(f != NULL);
	fputs(contenido, f);
	fclose(f);
}

static void test_caso_1_meminfo_valido(const char *dir)
{
	char ruta[512];
	struct muestra m;
	const char *contenido =
		"MemTotal:        16384000 kB\n"
		"MemFree:          4096000 kB\n"
		"MemAvailable:     8192000 kB\n"
		"Buffers:           512000 kB\n"
		"Cached:           2048000 kB\n"
		"Active:           6144000 kB\n"
		"Inactive:         3072000 kB\n"
		"SwapTotal:        2097152 kB\n"
		"SwapFree:         1048576 kB\n"
		"Dirty:                 96 kB\n"
		"AnonPages:         123456 kB\n";

	snprintf(ruta, sizeof(ruta), "%s/meminfo_ok", dir);
	escribir_archivo(ruta, contenido);

	memset(&m, 0, sizeof(m));
	assert(metricas_leer_meminfo(ruta, &m) == 0);
	assert(m.mem_total == 16384000);
	assert(m.mem_libre == 4096000);
	assert(m.mem_disponible == 8192000);
	assert(m.buffers == 512000);
	assert(m.cached == 2048000);
	assert(m.activas == 6144000);
	assert(m.inactivas == 3072000);
	assert(m.swap_total == 2097152);
	assert(m.swap_libre == 1048576);

	unlink(ruta);
	printf("\033[0;32m✓ Caso 1: meminfo con 9 claves y extras\033[0m\n");
}

static void test_caso_2_meminfo_sin_active(const char *dir)
{
	char ruta[512];
	struct muestra m;
	const char *contenido =
		"MemTotal:        16384000 kB\n"
		"MemFree:          4096000 kB\n"
		"MemAvailable:     8192000 kB\n"
		"Buffers:           512000 kB\n"
		"Cached:           2048000 kB\n"
		"Inactive:         3072000 kB\n"
		"SwapTotal:        2097152 kB\n"
		"SwapFree:         1048576 kB\n";

	snprintf(ruta, sizeof(ruta), "%s/meminfo_sin_active", dir);
	escribir_archivo(ruta, contenido);

	memset(&m, 0, sizeof(m));
	assert(metricas_leer_meminfo(ruta, &m) == -1);

	unlink(ruta);
	printf("\033[0;32m✓ Caso 2: meminfo sin Active devuelve -1\033[0m\n");
}

static void test_caso_3_vmstat_exacto_y_faltante(const char *dir)
{
	char ruta_ok[512];
	char ruta_sin_maj[512];
	struct muestra m;
	const char *ok =
		"nr_free_pages 1024\n"
		"pgfault 500000\n"
		"pgfault_file 999999\n"
		"pgmajfault 12000\n"
		"pgmajfault_file 888888\n";
	const char *sin_maj =
		"nr_free_pages 1024\n"
		"pgfault 500000\n"
		"pgfault_file 999999\n";

	snprintf(ruta_ok, sizeof(ruta_ok), "%s/vmstat_ok", dir);
	escribir_archivo(ruta_ok, ok);

	snprintf(ruta_sin_maj, sizeof(ruta_sin_maj), "%s/vmstat_sin_maj", dir);
	escribir_archivo(ruta_sin_maj, sin_maj);

	memset(&m, 0, sizeof(m));
	assert(metricas_leer_vmstat(ruta_ok, &m) == 0);
	assert(m.pgfault == 500000);
	assert(m.pgmajfault == 12000);

	memset(&m, 0, sizeof(m));
	assert(metricas_leer_vmstat(ruta_sin_maj, &m) == -1);

	unlink(ruta_ok);
	unlink(ruta_sin_maj);
	printf("\033[0;32m✓ Caso 3: vmstat exacto y sin distractor\033[0m\n");
}

static void test_caso_4_calcular_primera(void)
{
	struct muestra m;

	memset(&m, 0, sizeof(m));
	m.mem_total = 16000;
	m.mem_libre = 4000;
	m.buffers = 1000;
	m.cached = 2000;
	m.swap_total = 5000;
	m.swap_libre = 2000;
	m.pgfault = 1000;
	m.pgmajfault = 50;

	metricas_calcular(&m, NULL);
	assert(m.primera == 1);
	assert(m.cache == 3000);
	assert(m.libre == 4000);
	assert(m.usada == 9000);
	assert(m.swap_usada == 3000);
	assert(m.pgfault_delta == 0);
	assert(m.pgmajfault_delta == 0);

	printf("\033[0;32m✓ Caso 4: metricas_calcular primera muestra\033[0m\n");
}

static void test_caso_5_calcular_con_anterior(void)
{
	struct muestra ant;
	struct muestra m;

	memset(&ant, 0, sizeof(ant));
	ant.pgfault = 1000;
	ant.pgmajfault = 50;

	memset(&m, 0, sizeof(m));
	m.mem_total = 16000;
	m.mem_libre = 4000;
	m.buffers = 1000;
	m.cached = 2000;
	m.pgfault = 1500;
	m.pgmajfault = 75;

	metricas_calcular(&m, &ant);
	assert(m.primera == 0);
	assert(m.pgfault_delta == 500);
	assert(m.pgmajfault_delta == 25);

	printf("\033[0;32m✓ Caso 5: metricas_calcular deltas con anterior\033[0m\n");
}

static void test_caso_6_contador_menor_delta_cero(void)
{
	struct muestra ant;
	struct muestra m;

	memset(&ant, 0, sizeof(ant));
	ant.pgfault = 2000;
	ant.pgmajfault = 100;

	memset(&m, 0, sizeof(m));
	m.pgfault = 1000;
	m.pgmajfault = 50;

	metricas_calcular(&m, &ant);
	assert(m.primera == 0);
	assert(m.pgfault_delta == 0);
	assert(m.pgmajfault_delta == 0);

	printf("\033[0;32m✓ Caso 6: contador menor que anterior produce delta 0\033[0m\n");
}

static void test_caso_7_derivados_y_saturacion(void)
{
	struct muestra m;
	struct muestra m_over;

	memset(&m, 0, sizeof(m));
	m.mem_total = 10000;
	m.mem_libre = 3000;
	m.buffers = 1000;
	m.cached = 2000;
	metricas_calcular(&m, NULL);
	assert(m.usada + m.cache + m.libre == m.mem_total);

	memset(&m_over, 0, sizeof(m_over));
	m_over.mem_total = 1000;
	m_over.mem_libre = 600;
	m_over.buffers = 300;
	m_over.cached = 200;
	metricas_calcular(&m_over, NULL);
	assert(m_over.usada == 0);
	assert(m_over.cache == 500);
	assert(m_over.libre == 600);

	printf("\033[0;32m✓ Caso 7: derivados suman mem_total y saturacion usada == 0\033[0m\n");
}

static void test_caso_8_metricas_tomar_dos_veces(const char *dir)
{
	char ruta_mem[512];
	char ruta_vm1[512];
	char ruta_vm2[512];
	struct muestra d[2];
	unsigned long id1, id2;
	size_t n;

	const char *mem =
		"MemTotal:        16384000 kB\n"
		"MemFree:          4096000 kB\n"
		"MemAvailable:     8192000 kB\n"
		"Buffers:           512000 kB\n"
		"Cached:           2048000 kB\n"
		"Active:           6144000 kB\n"
		"Inactive:         3072000 kB\n"
		"SwapTotal:        2097152 kB\n"
		"SwapFree:         1048576 kB\n";
	const char *vm1 = "pgfault 100000\npgmajfault 5000\n";
	const char *vm2 = "pgfault 110000\npgmajfault 5050\n";

	snprintf(ruta_mem, sizeof(ruta_mem), "%s/meminfo_t8", dir);
	snprintf(ruta_vm1, sizeof(ruta_vm1), "%s/vmstat_t8_1", dir);
	snprintf(ruta_vm2, sizeof(ruta_vm2), "%s/vmstat_t8_2", dir);

	escribir_archivo(ruta_mem, mem);
	escribir_archivo(ruta_vm1, vm1);
	escribir_archivo(ruta_vm2, vm2);

	metricas_reiniciar();
	id1 = metricas_tomar(ruta_mem, ruta_vm1);
	assert(id1 == 1);

	id2 = metricas_tomar(ruta_mem, ruta_vm2);
	assert(id2 == 2);

	n = metricas_desde(1, d, 2);
	assert(n == 1);
	assert(d[0].id == 2);
	assert(d[0].primera == 0);
	assert(d[0].pgfault_delta == 10000);
	assert(d[0].pgmajfault_delta == 50);

	unlink(ruta_mem);
	unlink(ruta_vm1);
	unlink(ruta_vm2);
	printf("\033[0;32m✓ Caso 8: metricas_tomar dos veces con deltas correctos\033[0m\n");
}

static void test_caso_10_historial_155_muestras(const char *dir)
{
	char ruta_mem[512];
	char ruta_vm[512];
	struct muestra lote[160];
	size_t n;

	const char *mem =
		"MemTotal:        16384000 kB\n"
		"MemFree:          4096000 kB\n"
		"MemAvailable:     8192000 kB\n"
		"Buffers:           512000 kB\n"
		"Cached:           2048000 kB\n"
		"Active:           6144000 kB\n"
		"Inactive:         3072000 kB\n"
		"SwapTotal:        2097152 kB\n"
		"SwapFree:         1048576 kB\n";
	const char *vm = "pgfault 1000\npgmajfault 10\n";

	snprintf(ruta_mem, sizeof(ruta_mem), "%s/meminfo_t10", dir);
	snprintf(ruta_vm, sizeof(ruta_vm), "%s/vmstat_t10", dir);

	escribir_archivo(ruta_mem, mem);
	escribir_archivo(ruta_vm, vm);

	metricas_reiniciar();
	for (int i = 1; i <= 155; i++) {
		unsigned long id = metricas_tomar(ruta_mem, ruta_vm);
		assert(id == (unsigned long)i);
	}

	n = metricas_historial(lote, 160);
	assert(n == 150);
	for (size_t i = 0; i < 150; i++)
		assert(lote[i].id == (unsigned long)(6 + i));

	n = metricas_desde(150, lote, 10);
	assert(n == 5);
	for (size_t i = 0; i < 5; i++)
		assert(lote[i].id == (unsigned long)(151 + i));

	unlink(ruta_mem);
	unlink(ruta_vm);
	printf("\033[0;32m✓ Caso 10: historial de 155 muestras devuelve 150 (ids 6..155) y metricas_desde\033[0m\n");
}

static void test_caso_9_cambio_estado(const char *dir)
{
	char ruta_mem[512];
	char ruta_vm[512];
	struct muestra m;
	unsigned long id;
	const char *mem =
		"MemTotal:        16384000 kB\n"
		"MemFree:          4096000 kB\n"
		"MemAvailable:     8192000 kB\n"
		"Buffers:           512000 kB\n"
		"Cached:           2048000 kB\n"
		"Active:           6144000 kB\n"
		"Inactive:         3072000 kB\n"
		"SwapTotal:        2097152 kB\n"
		"SwapFree:         1048576 kB\n";
	const char *vm = "pgfault 1000\npgmajfault 10\n";

	snprintf(ruta_mem, sizeof(ruta_mem), "%s/meminfo_t9", dir);
	snprintf(ruta_vm, sizeof(ruta_vm), "%s/vmstat_t9", dir);
	escribir_archivo(ruta_mem, mem);
	escribir_archivo(ruta_vm, vm);

	metricas_reiniciar();
	assert(metricas_activas() == 1);
	id = metricas_tomar(ruta_mem, ruta_vm);
	assert(id == 1);

	assert(metricas_cambiar_estado(0) == 1);
	assert(metricas_activas() == 0);
	assert(metricas_cambiar_estado(0) == 0);
	assert(metricas_activas() == 0);

	assert(metricas_cambiar_estado(1) == 1);
	assert(metricas_activas() == 1);

	id = metricas_tomar(ruta_mem, ruta_vm);
	assert(id == 2);
	assert(metricas_desde(1, &m, 1) == 1);
	assert(m.primera == 1);

	unlink(ruta_mem);
	unlink(ruta_vm);
	printf("\033[0;32m✓ Caso 9: metricas_cambiar_estado transiciones y primera == 1 al reactivar\033[0m\n");
}

static void test_caso_11_json_muestra(void)
{
	struct muestra m;
	struct buf b = { 0 };
	cJSON *root;
	cJSON *item;

	memset(&m, 0, sizeof(m));
	m.id = 42;
	snprintf(m.timestamp, sizeof(m.timestamp), "2026-10-18 16:00:00");
	m.primera = 1;
	m.mem_total = 16000000;
	m.mem_libre = 4000000;
	m.mem_disponible = 8000000;
	m.buffers = 500000;
	m.cached = 2000000;
	m.activas = 6000000;
	m.inactivas = 3000000;
	m.swap_total = 2000000;
	m.swap_libre = 1000000;
	m.usada = 9500000;
	m.cache = 2500000;
	m.libre = 4000000;
	m.swap_usada = 1000000;
	m.pgfault = 123456789ULL;
	m.pgmajfault = 98765ULL;
	m.pgfault_delta = 1000ULL;
	m.pgmajfault_delta = 10ULL;

	json_muestra(&b, &m);
	assert(!b.fallo && b.datos != NULL);

	root = cJSON_Parse(b.datos);
	assert(root != NULL);
	assert(cJSON_IsObject(root));

	item = cJSON_GetObjectItemCaseSensitive(root, "id");
	assert(item && cJSON_IsNumber(item) && (unsigned long)item->valuedouble == 42);

	item = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
	assert(item && cJSON_IsString(item) && strcmp(item->valuestring, "2026-10-18 16:00:00") == 0);

	item = cJSON_GetObjectItemCaseSensitive(root, "primera");
	assert(item && cJSON_IsBool(item) && cJSON_IsTrue(item));

	assert(cJSON_GetObjectItemCaseSensitive(root, "mem_total") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "mem_libre") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "mem_disponible") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "buffers") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "cached") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "activas") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "inactivas") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "swap_total") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "swap_libre") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "usada") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "cache") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "libre") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "swap_usada") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "pgfault") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "pgmajfault") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "pgfault_delta") != NULL);
	assert(cJSON_GetObjectItemCaseSensitive(root, "pgmajfault_delta") != NULL);

	cJSON_Delete(root);
	buf_liberar(&b);

	printf("\033[0;32m✓ Caso 11: json_muestra parsea con cJSON y primera es booleano\033[0m\n");
}

int main(void)
{
	char plantilla[] = "/tmp/test_metricas_XXXXXX";
	char *dir = mkdtemp(plantilla);
	assert(dir != NULL);

	test_caso_1_meminfo_valido(dir);
	test_caso_2_meminfo_sin_active(dir);
	test_caso_3_vmstat_exacto_y_faltante(dir);
	test_caso_4_calcular_primera();
	test_caso_5_calcular_con_anterior();
	test_caso_6_contador_menor_delta_cero();
	test_caso_7_derivados_y_saturacion();
	test_caso_8_metricas_tomar_dos_veces(dir);
	test_caso_9_cambio_estado(dir);
	test_caso_10_historial_155_muestras(dir);
	test_caso_11_json_muestra();

	rmdir(dir);
	printf("\033[0;32mTodas las pruebas de metricas pasaron exitosamente.\033[0m\n");
	return 0;
}
