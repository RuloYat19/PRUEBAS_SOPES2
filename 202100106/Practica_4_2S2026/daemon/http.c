#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

#define MAX_CLIENTES		64
#define TAM_PETICION		4096
#define MAX_PENDIENTE_SSE	(256 * 1024)
#define MAX_ESTATICO		(8 * 1024 * 1024)
#define ESPERA_POLL_MS		15000
#define LATIDO_S		15
#define INACTIVIDAD_S		10

struct cliente {
	int fd;
	int sse;
	int cerrar_al_vaciar;
	time_t ultimo_uso;
	size_t leidos;
	size_t enviados;
	char peticion[TAM_PETICION];
	struct buf salida;
};

static struct cliente clientes[MAX_CLIENTES];
static unsigned long ultimo_difundido;
static const char *frontend;

static void cliente_cerrar(struct cliente *c)
{
	close(c->fd);
	buf_liberar(&c->salida);
	c->fd = -1;
}

static void cliente_vaciar(struct cliente *c)
{
	ssize_t n;

	if (c->salida.fallo) {
		cliente_cerrar(c);
		return;
	}

	while (c->enviados < c->salida.largo) {
		n = send(c->fd, c->salida.datos + c->enviados,
			 c->salida.largo - c->enviados, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			cliente_cerrar(c);
			return;
		}
		c->enviados += (size_t)n;
	}

	c->salida.largo = 0;
	c->enviados = 0;
	if (c->cerrar_al_vaciar)
		cliente_cerrar(c);
}

static const char *texto_de_codigo(int codigo)
{
	switch (codigo) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 431: return "Request Header Fields Too Large";
	default:  return "Internal Server Error";
	}
}

static void responder(struct cliente *c, int codigo, const char *tipo,
		      const char *cuerpo, size_t largo)
{
	buf_printf(&c->salida,
		   "HTTP/1.1 %d %s\r\n"
		   "Content-Type: %s\r\n"
		   "Content-Length: %zu\r\n"
		   "Cache-Control: no-store\r\n"
		   "Access-Control-Allow-Origin: *\r\n"
		   "Connection: close\r\n\r\n",
		   codigo, texto_de_codigo(codigo), tipo, largo);
	buf_agregar(&c->salida, cuerpo, largo);
	c->cerrar_al_vaciar = 1;
}

static void responder_error(struct cliente *c, int codigo, const char *mensaje)
{
	char cuerpo[256];
	int largo;

	largo = snprintf(cuerpo, sizeof(cuerpo), "{\"error\":\"%s\"}", mensaje);
	responder(c, codigo, "application/json", cuerpo, (size_t)largo);
}

static void atender_estado(struct cliente *c)
{
	struct buf cuerpo = { 0 };

	json_estado(&cuerpo);
	if (cuerpo.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", cuerpo.datos, cuerpo.largo);
	buf_liberar(&cuerpo);
}

static long extraer_pid(const char *consulta)
{
	const char *parametro = consulta;
	char *fin;
	long pid;

	while (parametro && *parametro) {
		if (!strncmp(parametro, "pid=", 4)) {
			errno = 0;
			pid = strtol(parametro + 4, &fin, 10);
			if (errno || fin == parametro + 4 ||
			    (*fin && *fin != '&') || pid <= 0 || pid > INT_MAX)
				return -1;
			return pid;
		}
		parametro = strchr(parametro, '&');
		if (parametro)
			parametro++;
	}
	return -1;
}

static void atender_proceso(struct cliente *c, const char *consulta)
{
	struct process_info info;
	struct buf cuerpo = { 0 };
	long pid;

	pid = extraer_pid(consulta);
	if (pid < 0) {
		responder_error(c, 400, "Parametro pid invalido");
		return;
	}

	memset(&info, 0, sizeof(info));
	if (syscall(SYS_GET_PROCESS_INFO, (pid_t)pid, &info) < 0) {
		if (errno == ESRCH) {
			responder_error(c, 404, "Proceso no encontrado (ESRCH)");
		} else {
			registrar("sys_get_process_info(%ld) fallo: %s", pid, strerror(errno));
			responder_error(c, 500, "Fallo sys_get_process_info");
		}
		return;
	}
	info.name[TASK_COMM_LEN - 1] = '\0';

	buf_printf(&cuerpo, "{\"pid\":%d,\"nombre\":", info.pid);
	json_cadena(&cuerpo, info.name);
	buf_printf(&cuerpo, ",\"runtime_ns\":%llu,\"memory_kb\":%lu}",
		   info.runtime_ns, info.memory_kb);
	if (cuerpo.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", cuerpo.datos, cuerpo.largo);
	buf_liberar(&cuerpo);
}

static void atender_sse(struct cliente *c)
{
	buf_printf(&c->salida,
		   "HTTP/1.1 200 OK\r\n"
		   "Content-Type: text/event-stream\r\n"
		   "Cache-Control: no-cache\r\n"
		   "Connection: keep-alive\r\n"
		   "Access-Control-Allow-Origin: *\r\n\r\n"
		   "retry: 3000\n: conectado\n\n");
	c->sse = 1;
}

static const char *tipo_de_archivo(const char *ruta)
{
	const char *extension = strrchr(ruta, '.');

	if (!extension)
		return "application/octet-stream";
	if (!strcmp(extension, ".html"))
		return "text/html; charset=utf-8";
	if (!strcmp(extension, ".css"))
		return "text/css; charset=utf-8";
	if (!strcmp(extension, ".js"))
		return "application/javascript; charset=utf-8";
	if (!strcmp(extension, ".json"))
		return "application/json";
	if (!strcmp(extension, ".svg"))
		return "image/svg+xml";
	if (!strcmp(extension, ".png"))
		return "image/png";
	if (!strcmp(extension, ".ico"))
		return "image/x-icon";
	return "application/octet-stream";
}

static void atender_estatico(struct cliente *c, const char *ruta)
{
	char completa[PATH_MAX];
	struct stat info;
	char *contenido;
	ssize_t leidos;
	size_t total = 0;
	int fd;

	if (!strcmp(ruta, "/"))
		ruta = "/index.html";

	if (strstr(ruta, "..") ||
	    snprintf(completa, sizeof(completa), "%s%s", frontend, ruta) >=
	    (int)sizeof(completa)) {
		responder_error(c, 400, "Ruta invalida");
		return;
	}

	fd = open(completa, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) ||
	    info.st_size > MAX_ESTATICO) {
		if (fd >= 0)
			close(fd);
		responder_error(c, 404, "Recurso no encontrado");
		return;
	}

	contenido = malloc((size_t)info.st_size + 1);
	if (!contenido) {
		close(fd);
		responder_error(c, 500, "Memoria insuficiente");
		return;
	}
	while (total < (size_t)info.st_size) {
		leidos = read(fd, contenido + total, (size_t)info.st_size - total);
		if (leidos <= 0)
			break;
		total += (size_t)leidos;
	}
	close(fd);

	responder(c, 200, tipo_de_archivo(completa), contenido, total);
	free(contenido);
}

static void atender(struct cliente *c)
{
	char metodo[8];
	char ruta[1024];
	char *consulta;

	if (sscanf(c->peticion, "%7s %1023s", metodo, ruta) != 2) {
		responder_error(c, 400, "Peticion mal formada");
		return;
	}
	if (strcmp(metodo, "GET")) {
		responder_error(c, 405, "Solo se admite GET");
		return;
	}

	consulta = strchr(ruta, '?');
	if (consulta)
		*consulta++ = '\0';

	if (!strcmp(ruta, "/api/estado"))
		atender_estado(c);
	else if (!strcmp(ruta, "/api/proceso"))
		atender_proceso(c, consulta);
	else if (!strcmp(ruta, "/api/eventos"))
		atender_sse(c);
	else
		atender_estatico(c, ruta);
}

static void cliente_leer(struct cliente *c)
{
	char descarte[512];
	int atendido = c->sse || c->cerrar_al_vaciar;
	ssize_t n;

	if (atendido)
		n = recv(c->fd, descarte, sizeof(descarte), 0);
	else
		n = recv(c->fd, c->peticion + c->leidos,
			 sizeof(c->peticion) - 1 - c->leidos, 0);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return;
	if (n <= 0) {
		cliente_cerrar(c);
		return;
	}
	if (atendido)
		return;

	c->leidos += (size_t)n;
	c->peticion[c->leidos] = '\0';

	if (strstr(c->peticion, "\r\n\r\n"))
		atender(c);
	else if (c->leidos == sizeof(c->peticion) - 1)
		responder_error(c, 431, "Encabezados demasiado grandes");
	else
		return;

	cliente_vaciar(c);
}

static void enviar_a_suscriptores(const char *datos, size_t largo)
{
	struct cliente *c;
	int i;

	for (i = 0; i < MAX_CLIENTES; i++) {
		c = &clientes[i];
		if (c->fd < 0 || !c->sse)
			continue;
		buf_agregar(&c->salida, datos, largo);
		if (c->salida.largo - c->enviados > MAX_PENDIENTE_SSE)
			cliente_cerrar(c);
		else
			cliente_vaciar(c);
	}
}

static void difundir_eventos(void)
{
	struct buf tramas = { 0 };
	unsigned long total;
	unsigned long s;

	pthread_mutex_lock(&estado.lock);
	total = estado.total_eventos;
	if (total - ultimo_difundido > MAX_EVENTOS)
		ultimo_difundido = total - MAX_EVENTOS;
	for (s = ultimo_difundido; s < total; s++) {
		const struct evento *e = &estado.eventos[s % MAX_EVENTOS];

		buf_printf(&tramas, "id: %lu\ndata: ", e->id);
		json_evento(&tramas, e);
		buf_agregar(&tramas, "\n\n", 2);
	}
	pthread_mutex_unlock(&estado.lock);
	ultimo_difundido = total;

	if (tramas.largo && !tramas.fallo)
		enviar_a_suscriptores(tramas.datos, tramas.largo);
	buf_liberar(&tramas);
}

static void aceptar(int fd_escucha)
{
	struct cliente *c;
	int fd;
	int i;

	for (;;) {
		fd = accept4(fd_escucha, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0)
			return;

		for (i = 0; i < MAX_CLIENTES && clientes[i].fd >= 0; i++)
			;
		if (i == MAX_CLIENTES) {
			close(fd);
			continue;
		}

		c = &clientes[i];
		memset(c, 0, sizeof(*c));
		c->fd = fd;
		c->ultimo_uso = time(NULL);
	}
}

int servidor_iniciar(int puerto)
{
	struct sockaddr_in direccion;
	int activo = 1;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &activo, sizeof(activo));

	memset(&direccion, 0, sizeof(direccion));
	direccion.sin_family = AF_INET;
	direccion.sin_addr.s_addr = htonl(INADDR_ANY);
	direccion.sin_port = htons((uint16_t)puerto);

	if (bind(fd, (struct sockaddr *)&direccion, sizeof(direccion)) < 0 ||
	    listen(fd, 32) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

void servidor_bucle(int fd_escucha, const char *dir_frontend)
{
	struct pollfd sondeo[MAX_CLIENTES + 2];
	int ranura[MAX_CLIENTES + 2];
	time_t ultimo_latido = time(NULL);
	struct cliente *c;
	eventfd_t avisos;
	time_t ahora;
	int n, i;

	frontend = dir_frontend;
	for (i = 0; i < MAX_CLIENTES; i++)
		clientes[i].fd = -1;

	while (ejecutando) {
		sondeo[0] = (struct pollfd){ .fd = fd_escucha, .events = POLLIN };
		sondeo[1] = (struct pollfd){ .fd = fd_aviso, .events = POLLIN };
		n = 2;
		for (i = 0; i < MAX_CLIENTES; i++) {
			c = &clientes[i];
			if (c->fd < 0)
				continue;
			sondeo[n] = (struct pollfd){ .fd = c->fd, .events = POLLIN };
			if (c->enviados < c->salida.largo)
				sondeo[n].events |= POLLOUT;
			ranura[n++] = i;
		}

		if (poll(sondeo, (nfds_t)n, ESPERA_POLL_MS) < 0) {
			if (errno == EINTR)
				continue;
			registrar("poll fallo: %s", strerror(errno));
			break;
		}

		if (sondeo[1].revents & POLLIN) {
			eventfd_read(fd_aviso, &avisos);
			difundir_eventos();
		}

		ahora = time(NULL);
		for (i = 2; i < n; i++) {
			c = &clientes[ranura[i]];
			if (c->fd != sondeo[i].fd || !sondeo[i].revents)
				continue;
			c->ultimo_uso = ahora;
			if (sondeo[i].revents & POLLIN)
				cliente_leer(c);
			else if (sondeo[i].revents & (POLLERR | POLLHUP | POLLNVAL))
				cliente_cerrar(c);
			if (c->fd >= 0 && (sondeo[i].revents & POLLOUT))
				cliente_vaciar(c);
		}

		if (ahora - ultimo_latido >= LATIDO_S) {
			enviar_a_suscriptores(": latido\n\n", 10);
			ultimo_latido = ahora;
		}
		for (i = 0; i < MAX_CLIENTES; i++) {
			c = &clientes[i];
			if (c->fd >= 0 && !c->sse &&
			    ahora - c->ultimo_uso > INACTIVIDAD_S)
				cliente_cerrar(c);
		}

		if (sondeo[0].revents & POLLIN)
			aceptar(fd_escucha);
	}

	for (i = 0; i < MAX_CLIENTES; i++)
		if (clientes[i].fd >= 0)
			cliente_cerrar(&clientes[i]);
	close(fd_escucha);
}
