#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/time64.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/err.h>

#define LOG_WATCH_MAX_FILES	8
#define LOG_WATCH_MAX_KEYWORD	64
#define LOG_WATCH_BUF_SIZE	4096
#define LOG_WATCH_INTERVAL_MS	3000

struct log_watch_file {
	char *ruta;
	loff_t offset;
};

struct log_watch_entry {
	struct list_head list;
	int id;
	int cantidad;
	struct task_struct *hilo;
	struct log_watch_file archivos[LOG_WATCH_MAX_FILES];
	char ruta_central[PATH_MAX];
	char palabra[LOG_WATCH_MAX_KEYWORD];
};

static LIST_HEAD(log_watch_list);
static DEFINE_MUTEX(log_watch_lock);
static int log_watch_next_id = 1;

static void log_watch_marca_de_tiempo(char *destino, size_t tamano)
{
	struct timespec64 ahora;
	struct tm desglose;

	ktime_get_real_ts64(&ahora);
	time64_to_tm(ahora.tv_sec, 0, &desglose);

	snprintf(destino, tamano, "%04ld-%02d-%02d %02d:%02d:%02d",
		 desglose.tm_year + 1900, desglose.tm_mon + 1, desglose.tm_mday,
		 desglose.tm_hour, desglose.tm_min, desglose.tm_sec);
}

static int log_watch_registrar_hallazgo(struct log_watch_entry *entrada,
					const char *ruta_origen,
					const char *linea)
{
	struct file *archivo;
	char *registro;
	char fecha[32];
	int largo;
	loff_t pos = 0;
	ssize_t escritos;

	registro = kmalloc(LOG_WATCH_BUF_SIZE, GFP_KERNEL);
	if (!registro)
		return -ENOMEM;

	log_watch_marca_de_tiempo(fecha, sizeof(fecha));

	largo = snprintf(registro, LOG_WATCH_BUF_SIZE,
			 "[%s] watch_id=%d origen=%s palabra=%s linea=%s\n",
			 fecha, entrada->id, ruta_origen, entrada->palabra, linea);

	archivo = filp_open(entrada->ruta_central,
			    O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (IS_ERR(archivo)) {
		kfree(registro);
		return PTR_ERR(archivo);
	}

	escritos = kernel_write(archivo, registro, largo, &pos);
	filp_close(archivo, NULL);
	kfree(registro);

	if (escritos < 0)
		return (int)escritos;

	pr_info("log_watch: coincidencia de '%s' en %s registrada por watch_id %d\n",
		entrada->palabra, ruta_origen, entrada->id);
	return 0;
}

static void log_watch_revisar_archivo(struct log_watch_entry *entrada,
				      struct log_watch_file *vigilado)
{
	struct file *archivo;
	char *buffer;
	char *resto;
	char *linea;
	char *ultimo_salto;
	loff_t inicio;
	loff_t tamano;
	ssize_t leidos;

	archivo = filp_open(vigilado->ruta, O_RDONLY, 0);
	if (IS_ERR(archivo))
		return;

	tamano = i_size_read(file_inode(archivo));
	if (tamano < vigilado->offset)
		vigilado->offset = 0;

	buffer = kmalloc(LOG_WATCH_BUF_SIZE, GFP_KERNEL);
	if (!buffer) {
		filp_close(archivo, NULL);
		return;
	}

	inicio = vigilado->offset;
	leidos = kernel_read(archivo, buffer, LOG_WATCH_BUF_SIZE - 1,
			     &vigilado->offset);
	if (leidos <= 0)
		goto salir;

	buffer[leidos] = '\0';

	ultimo_salto = strrchr(buffer, '\n');
	if (!ultimo_salto) {
		vigilado->offset = inicio;
		goto salir;
	}

	*ultimo_salto = '\0';
	vigilado->offset = inicio + (ultimo_salto - buffer) + 1;

	resto = buffer;
	while ((linea = strsep(&resto, "\n")) != NULL) {
		if (*linea && strstr(linea, entrada->palabra))
			log_watch_registrar_hallazgo(entrada, vigilado->ruta, linea);
	}

salir:
	kfree(buffer);
	filp_close(archivo, NULL);
}

static int log_watch_hilo(void *dato)
{
	struct log_watch_entry *entrada = dato;
	int i;

	pr_info("log_watch: hilo de monitoreo %d iniciado sobre %d archivos\n",
		entrada->id, entrada->cantidad);

	while (!kthread_should_stop()) {
		for (i = 0; i < entrada->cantidad; i++)
			log_watch_revisar_archivo(entrada, &entrada->archivos[i]);

		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			set_current_state(TASK_RUNNING);
			break;
		}
		schedule_timeout(msecs_to_jiffies(LOG_WATCH_INTERVAL_MS));
	}

	pr_info("log_watch: hilo de monitoreo %d finalizado\n", entrada->id);
	return 0;
}

static void log_watch_liberar_entrada(struct log_watch_entry *entrada)
{
	int i;

	for (i = 0; i < entrada->cantidad; i++)
		kfree(entrada->archivos[i].ruta);

	kfree(entrada);
}

SYSCALL_DEFINE4(start_log_watch, const char __user *const __user *, rutas,
		int, cantidad, const char __user *, ruta_log_central,
		const char __user *, palabra_clave)
{
	struct log_watch_entry *entrada;
	const char __user **punteros;
	char *ruta_kernel;
	long copiados;
	int resultado;
	int id;
	int i;

	if (!rutas || !ruta_log_central || !palabra_clave)
		return -EINVAL;

	if (cantidad <= 0 || cantidad > LOG_WATCH_MAX_FILES)
		return -EINVAL;

	entrada = kzalloc(sizeof(*entrada), GFP_KERNEL);
	if (!entrada)
		return -ENOMEM;

	copiados = strncpy_from_user(entrada->ruta_central, ruta_log_central, PATH_MAX);
	if (copiados <= 0 || copiados >= PATH_MAX) {
		resultado = (copiados < 0) ? -EFAULT : -EINVAL;
		goto error;
	}

	copiados = strncpy_from_user(entrada->palabra, palabra_clave, LOG_WATCH_MAX_KEYWORD);
	if (copiados <= 0 || copiados >= LOG_WATCH_MAX_KEYWORD) {
		resultado = (copiados < 0) ? -EFAULT : -EINVAL;
		goto error;
	}

	punteros = kmalloc_array(cantidad, sizeof(*punteros), GFP_KERNEL);
	if (!punteros) {
		resultado = -ENOMEM;
		goto error;
	}

	if (copy_from_user(punteros, rutas, cantidad * sizeof(*punteros))) {
		kfree(punteros);
		resultado = -EFAULT;
		goto error;
	}

	for (i = 0; i < cantidad; i++) {
		ruta_kernel = strndup_user(punteros[i], PATH_MAX);
		if (IS_ERR(ruta_kernel)) {
			resultado = PTR_ERR(ruta_kernel);
			kfree(punteros);
			goto error;
		}
		entrada->archivos[i].ruta = ruta_kernel;
		entrada->archivos[i].offset = 0;
		entrada->cantidad++;
	}
	kfree(punteros);

	mutex_lock(&log_watch_lock);
	id = log_watch_next_id;
	entrada->id = id;
	entrada->hilo = kthread_run(log_watch_hilo, entrada, "log_watch_%d", id);
	if (IS_ERR(entrada->hilo)) {
		resultado = PTR_ERR(entrada->hilo);
		mutex_unlock(&log_watch_lock);
		goto error;
	}
	log_watch_next_id++;
	list_add_tail(&entrada->list, &log_watch_list);
	mutex_unlock(&log_watch_lock);

	pr_info("start_log_watch: monitoreo %d creado por PID %d con palabra '%s'\n",
		id, current->pid, entrada->palabra);
	return id;

error:
	log_watch_liberar_entrada(entrada);
	return resultado;
}

SYSCALL_DEFINE1(stop_log_watch, int, id_monitoreo)
{
	struct log_watch_entry *entrada;
	struct log_watch_entry *encontrada = NULL;

	if (id_monitoreo <= 0)
		return -EINVAL;

	mutex_lock(&log_watch_lock);
	list_for_each_entry(entrada, &log_watch_list, list) {
		if (entrada->id == id_monitoreo) {
			encontrada = entrada;
			list_del(&entrada->list);
			break;
		}
	}
	mutex_unlock(&log_watch_lock);

	if (!encontrada) {
		pr_info("stop_log_watch: el monitoreo %d no existe o ya fue detenido\n",
			id_monitoreo);
		return -ESRCH;
	}

	kthread_stop(encontrada->hilo);
	log_watch_liberar_entrada(encontrada);

	pr_info("stop_log_watch: monitoreo %d detenido y recursos liberados\n",
		id_monitoreo);
	return 0;
}
