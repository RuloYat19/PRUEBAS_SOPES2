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

#include "cJSON.h"
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
	char peticion_fija[TAM_PETICION];
	char *peticion;
	size_t capacidad;
	struct buf salida;
	size_t fin_encabezados;
	char token[TAM_TOKEN + 1];
	char cookie_salida[160];
};

static struct cliente clientes[MAX_CLIENTES];
static unsigned long ultimo_difundido;
static unsigned long ultimo_metrica_difundida;
static int config_intervalo;
static const char *frontend;

static void enviar_a_suscriptores(const char *datos, size_t largo);

static void cliente_cerrar(struct cliente *c)
{
	close(c->fd);
	buf_liberar(&c->salida);
	if (c->peticion != c->peticion_fija)
		free(c->peticion);
	c->peticion = c->peticion_fija;
	c->capacidad = sizeof(c->peticion_fija);
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
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	default:  return "Internal Server Error";
	}
}

static void responder(struct cliente *c, int codigo, const char *tipo,
		      const char *cuerpo, size_t largo)
{
	if (c->cookie_salida[0] != '\0') {
		buf_printf(&c->salida,
			   "HTTP/1.1 %d %s\r\n"
			   "Content-Type: %s\r\n"
			   "Content-Length: %zu\r\n"
			   "Cache-Control: no-store\r\n"
			   "Set-Cookie: %s\r\n"
			   "Connection: close\r\n\r\n",
			   codigo, texto_de_codigo(codigo), tipo, largo, c->cookie_salida);
		c->cookie_salida[0] = '\0';
	} else {
		buf_printf(&c->salida,
			   "HTTP/1.1 %d %s\r\n"
			   "Content-Type: %s\r\n"
			   "Content-Length: %zu\r\n"
			   "Cache-Control: no-store\r\n"
			   "Connection: close\r\n\r\n",
			   codigo, texto_de_codigo(codigo), tipo, largo);
	}
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

static void atender_alertas(struct cliente *c)
{
	struct alerta *lista;
	size_t n;
	size_t i;
	struct buf resp = { 0 };

	lista = malloc(MAX_ALERTAS * sizeof(struct alerta));
	if (!lista) {
		responder_error(c, 500, "Memoria insuficiente");
		return;
	}

	n = alertas_recientes(lista, MAX_ALERTAS);

	buf_printf(&resp, "{\"alertas\":[");
	for (i = 0; i < n; i++) {
		if (i > 0)
			buf_agregar(&resp, ",", 1);
		json_alerta(&resp, &lista[i]);
	}
	buf_agregar(&resp, "]}", 2);

	free(lista);

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_metricas(struct cliente *c)
{
	struct muestra *lote;
	struct buf resp = { 0 };
	size_t n;
	size_t i;

	lote = malloc(MAX_MUESTRAS * sizeof(struct muestra));
	if (!lote) {
		responder_error(c, 500, "Memoria insuficiente");
		return;
	}

	n = metricas_historial(lote, MAX_MUESTRAS);

	buf_printf(&resp, "{\"activo\":%s,\"intervalo_s\":%d,\"muestras\":[",
		   metricas_activas() ? "true" : "false", config_intervalo);
	for (i = 0; i < n; i++) {
		if (i > 0)
			buf_agregar(&resp, ",", 1);
		json_muestra(&resp, &lote[i]);
	}
	buf_agregar(&resp, "]}", 2);

	free(lote);

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_monitoreo(struct cliente *c)
{
	struct buf resp = { 0 };

	json_monitoreo(&resp);
	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static int leer_activo(struct cliente *c, int *activo)
{
	const char *cuerpo = c->peticion + c->fin_encabezados;
	long len_cuerpo = peticion_content_length(c->peticion);
	cJSON *json;
	cJSON *j_activo;

	json = cJSON_ParseWithLength(cuerpo, (size_t)len_cuerpo);
	if (!json || !cJSON_IsObject(json)) {
		if (json)
			cJSON_Delete(json);
		responder_error(c, 400, "Solicitud invalida");
		return -1;
	}

	j_activo = cJSON_GetObjectItemCaseSensitive(json, "activo");
	if (!j_activo || !cJSON_IsBool(j_activo)) {
		cJSON_Delete(json);
		responder_error(c, 400, "Solicitud invalida");
		return -1;
	}

	*activo = cJSON_IsTrue(j_activo);
	cJSON_Delete(json);
	return 0;
}

static void atender_monitoreo_metricas(struct cliente *c, const char *usuario)
{
	int activo;
	int cambio;
	struct buf resp = { 0 };

	if (leer_activo(c, &activo) < 0)
		return;

	cambio = metricas_cambiar_estado(activo);
	if (cambio) {
		if (activo)
			alerta_emitir(MOT_MONITOREO_ACTIVADO,
				      "Métricas del sistema activadas por %s", usuario);
		else
			alerta_emitir(MOT_MONITOREO_DESACTIVADO,
				      "Métricas del sistema desactivadas por %s", usuario);

		struct buf trama = { 0 };
		buf_printf(&trama, "event: monitoreo\ndata: ");
		json_monitoreo(&trama);
		buf_agregar(&trama, "\n\n", 2);
		if (trama.largo && !trama.fallo)
			enviar_a_suscriptores(trama.datos, trama.largo);
		buf_liberar(&trama);
	}

	buf_printf(&resp, "{\"metricas\":%s,\"cambio\":%s}",
		   metricas_activas() ? "true" : "false",
		   cambio ? "true" : "false");

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_procesos(struct cliente *c)
{
	struct instantanea_procesos inst;
	struct buf resp = { 0 };

	procesos_instantanea(&inst);
	json_procesos(&resp, &inst);

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_monitoreo_procesos(struct cliente *c, const char *usuario)
{
	int activo;
	int cambio;
	struct buf resp = { 0 };

	if (leer_activo(c, &activo) < 0)
		return;

	cambio = procesos_cambiar_estado(activo);
	if (cambio) {
		if (activo)
			alerta_emitir(MOT_MONITOREO_ACTIVADO,
				      "Escaneo de procesos activado por %s", usuario);
		else
			alerta_emitir(MOT_MONITOREO_DESACTIVADO,
				      "Escaneo de procesos desactivado por %s", usuario);

		struct buf trama = { 0 };
		buf_printf(&trama, "event: monitoreo\ndata: ");
		json_monitoreo(&trama);
		buf_agregar(&trama, "\n\n", 2);
		if (trama.largo && !trama.fallo)
			enviar_a_suscriptores(trama.datos, trama.largo);
		buf_liberar(&trama);
	}

	buf_printf(&resp, "{\"procesos\":%s,\"cambio\":%s}",
		   procesos_activos() ? "true" : "false",
		   cambio ? "true" : "false");

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_archivos(struct cliente *c)
{
	struct buf resp = { 0 };

	json_archivos(&resp);

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_monitoreo_archivos(struct cliente *c, const char *usuario)
{
	int activo;
	int r;
	struct buf resp = { 0 };

	if (leer_activo(c, &activo) < 0)
		return;

	r = activo ? sensor_activar() : sensor_desactivar();
	if (r < 0) {
		responder_error(c, 500, "No se pudo cargar el sensor eBPF");
		return;
	}

	if (r == 1) {
		if (activo)
			alerta_emitir(MOT_MONITOREO_ACTIVADO,
				      "Escaneo de archivos activado por %s", usuario);
		else
			alerta_emitir(MOT_MONITOREO_DESACTIVADO,
				      "Escaneo de archivos desactivado por %s", usuario);

		struct buf trama = { 0 };
		buf_printf(&trama, "event: monitoreo\ndata: ");
		json_monitoreo(&trama);
		buf_agregar(&trama, "\n\n", 2);
		if (trama.largo && !trama.fallo)
			enviar_a_suscriptores(trama.datos, trama.largo);
		buf_liberar(&trama);
	}

	buf_printf(&resp, "{\"archivos\":%s,\"cambio\":%s}",
		   sensor_activo() ? "true" : "false",
		   (r == 1) ? "true" : "false");

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_ruta(struct cliente *c, const char *usuario)
{
	const char *cuerpo = c->peticion + c->fin_encabezados;
	long len_cuerpo = peticion_content_length(c->peticion);
	cJSON *json;
	cJSON *j_ruta;
	char canonica[PATH_MAX];
	int r;
	struct buf resp = { 0 };

	json = cJSON_ParseWithLength(cuerpo, (size_t)len_cuerpo);
	if (!json || !cJSON_IsObject(json)) {
		if (json)
			cJSON_Delete(json);
		responder_error(c, 400, "Ruta invalida");
		return;
	}

	j_ruta = cJSON_GetObjectItemCaseSensitive(json, "ruta");
	if (!j_ruta || !cJSON_IsString(j_ruta)) {
		cJSON_Delete(json);
		responder_error(c, 400, "Ruta invalida");
		return;
	}

	r = sensor_cambiar_ruta(j_ruta->valuestring, canonica, sizeof(canonica));
	cJSON_Delete(json);

	if (r == RUTA_INVALIDA) {
		responder_error(c, 400, "Ruta invalida");
		return;
	}
	if (r == RUTA_NO_DIRECTORIO) {
		responder_error(c, 400, "La ruta no es un directorio existente");
		return;
	}
	if (r == RUTA_LARGA) {
		responder_error(c, 400, "Ruta demasiado larga");
		return;
	}
	if (r == RUTA_ERROR_MAPA) {
		responder_error(c, 500, "No se pudo actualizar el sensor");
		return;
	}

	if (r == 1) {
		registrar("ruta vigilada cambiada a %s por %s", canonica, usuario);

		struct buf trama = { 0 };
		buf_printf(&trama, "event: monitoreo\ndata: ");
		json_monitoreo(&trama);
		buf_agregar(&trama, "\n\n", 2);
		if (trama.largo && !trama.fallo)
			enviar_a_suscriptores(trama.datos, trama.largo);
		buf_liberar(&trama);
	}

	buf_printf(&resp, "{\"ruta\":");
	json_cadena(&resp, canonica);
	buf_printf(&resp, ",\"cambio\":%s}", (r == 1) ? "true" : "false");

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_kill(struct cliente *c, const char *usuario)
{
	const char *cuerpo = c->peticion + c->fin_encabezados;
	long len_cuerpo = peticion_content_length(c->peticion);
	cJSON *json;
	cJSON *j_pid;
	long pid;
	char nombre[TASK_COMM_LEN];
	enum resultado_kill res;
	struct buf resp = { 0 };

	json = cJSON_ParseWithLength(cuerpo, (size_t)len_cuerpo);
	if (!json || !cJSON_IsObject(json)) {
		if (json)
			cJSON_Delete(json);
		responder_error(c, 400, "PID invalido");
		return;
	}

	j_pid = cJSON_GetObjectItemCaseSensitive(json, "pid");
	if (!j_pid || !cJSON_IsNumber(j_pid) ||
	    j_pid->valuedouble < 1.0 || j_pid->valuedouble > (double)PID_MAXIMO ||
	    j_pid->valuedouble != (double)(long)j_pid->valuedouble) {
		cJSON_Delete(json);
		responder_error(c, 400, "PID invalido");
		return;
	}

	pid = (long)j_pid->valuedouble;
	cJSON_Delete(json);

	memset(nombre, 0, sizeof(nombre));
	res = procesos_validar_kill("/proc", pid, getpid(), nombre);

	switch (res) {
	case KILL_PID_INVALIDO:
		responder_error(c, 400, "PID invalido");
		return;
	case KILL_PID_UNO:
		responder_error(c, 403, "No se permite terminar el proceso con PID 1");
		return;
	case KILL_DAEMON:
		responder_error(c, 403, "No se permite terminar el propio daemon");
		return;
	case KILL_KERNEL:
		responder_error(c, 403, "No se permite terminar hilos del kernel");
		return;
	case KILL_NO_LIDER:
		responder_error(c, 400, "El PID no corresponde a un proceso");
		return;
	case KILL_NO_EXISTE:
		responder_error(c, 404, "Proceso no encontrado");
		return;
	case KILL_OK:
		break;
	}

	if (kill((pid_t)pid, SIGKILL) < 0) {
		if (errno == ESRCH) {
			responder_error(c, 404, "Proceso no encontrado");
		} else {
			registrar("kill(%ld, SIGKILL) fallo: %s", pid, strerror(errno));
			responder_error(c, 500, "No se pudo terminar el proceso");
		}
		return;
	}

	alerta_emitir(MOT_PROCESO_MATADO,
		      "Proceso %s (PID %ld) terminado por %s", nombre, pid, usuario);

	buf_printf(&resp, "{\"pid\":%ld,\"nombre\":", pid);
	json_cadena(&resp, nombre);
	buf_printf(&resp, ",\"senal\":\"SIGKILL\"}");

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
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

static void atender_sse(struct cliente *c, const char *token)
{
	buf_printf(&c->salida,
		   "HTTP/1.1 200 OK\r\n"
		   "Content-Type: text/event-stream\r\n"
		   "Cache-Control: no-cache\r\n"
		   "Connection: keep-alive\r\n\r\n"
		   "retry: 3000\n: conectado\n\n");
	c->sse = 1;
	if (token && strlen(token) == TAM_TOKEN)
		memcpy(c->token, token, TAM_TOKEN + 1);
	else
		c->token[0] = '\0';
}

static void atender_login(struct cliente *c)
{
	const char *cuerpo = c->peticion + c->fin_encabezados;
	long len_cuerpo = peticion_content_length(c->peticion);
	cJSON *json;
	cJSON *j_user;
	cJSON *j_pass;
	const char *u_str;
	char *p_str;
	size_t u_len;
	size_t p_len;
	char usuario[MAX_USUARIO + 1];
	char password[MAX_PASSWORD + 1];
	int pam_res;
	enum rol rol;
	char token[TAM_TOKEN + 1];
	struct buf resp = { 0 };

	json = cJSON_ParseWithLength(cuerpo, (size_t)len_cuerpo);
	if (!json || !cJSON_IsObject(json)) {
		if (json)
			cJSON_Delete(json);
		responder_error(c, 400, "Solicitud de login invalida");
		return;
	}

	j_user = cJSON_GetObjectItemCaseSensitive(json, "usuario");
	j_pass = cJSON_GetObjectItemCaseSensitive(json, "password");

	if (!j_user || !j_pass || !cJSON_IsString(j_user) || !cJSON_IsString(j_pass)) {
		cJSON_Delete(json);
		responder_error(c, 400, "Solicitud de login invalida");
		return;
	}

	u_str = j_user->valuestring;
	p_str = j_pass->valuestring;
	u_len = strlen(u_str);
	p_len = strlen(p_str);

	if (u_len == 0 || u_len > MAX_USUARIO || p_len == 0 || p_len > MAX_PASSWORD) {
		explicit_bzero(p_str, p_len);
		cJSON_Delete(json);
		responder_error(c, 400, "Solicitud de login invalida");
		return;
	}

	for (size_t i = 0; i < u_len; i++) {
		if ((unsigned char)u_str[i] < 0x21 || (unsigned char)u_str[i] > 0x7e) {
			explicit_bzero(p_str, p_len);
			cJSON_Delete(json);
			responder_error(c, 400, "Solicitud de login invalida");
			return;
		}
	}

	memcpy(usuario, u_str, u_len);
	usuario[u_len] = '\0';
	memcpy(password, p_str, p_len);
	password[p_len] = '\0';

	explicit_bzero(p_str, p_len);
	cJSON_Delete(json);

	pam_res = auth_verificar_pam(usuario, password);
	explicit_bzero(password, sizeof(password));

	if (pam_res != 0) {
		registrar("Login fallido: %s", usuario);
		alerta_emitir(MOT_SESION_FALLIDA, "Credenciales inválidas para el usuario %s", usuario);
		responder_error(c, 401, "Credenciales invalidas");
		return;
	}

	rol = auth_rol_de_usuario(usuario);
	if (rol == ROL_NINGUNO) {
		registrar("Login sin grupo: %s", usuario);
		alerta_emitir(MOT_SESION_FALLIDA, "El usuario %s no pertenece a admin_user ni common_user", usuario);
		responder_error(c, 403, "El usuario no pertenece a admin_user ni common_user");
		return;
	}

	if (sesion_crear(usuario, rol, time(NULL), token) < 0) {
		responder_error(c, 500, "No se pudo crear la sesion");
		return;
	}

	snprintf(c->cookie_salida, sizeof(c->cookie_salida),
		 "so2_sesion=%s; Path=/; HttpOnly; SameSite=Strict", token);
	registrar("Login exitoso: %s (%s)", usuario, rol_nombre(rol));
	alerta_emitir(MOT_SESION_EXITOSA, "Usuario %s inició sesión como %s", usuario, rol_nombre(rol));

	buf_printf(&resp, "{\"usuario\":");
	json_cadena(&resp, usuario);
	buf_printf(&resp, ",\"rol\":");
	json_cadena(&resp, rol_nombre(rol));
	buf_agregar(&resp, "}", 1);

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_logout(struct cliente *c, const char *token)
{
	if (token && strlen(token) == TAM_TOKEN) {
		sesion_eliminar(token);
		for (int i = 0; i < MAX_CLIENTES; i++) {
			if (clientes[i].fd >= 0 && clientes[i].sse &&
			    strcmp(clientes[i].token, token) == 0) {
				cliente_cerrar(&clientes[i]);
			}
		}
	}

	snprintf(c->cookie_salida, sizeof(c->cookie_salida),
		 "so2_sesion=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
	responder(c, 200, "application/json", "{\"ok\":true}", 11);
}

static void atender_sesion(struct cliente *c, const char *usuario, enum rol rol)
{
	struct buf resp = { 0 };

	buf_printf(&resp, "{\"usuario\":");
	json_cadena(&resp, usuario);
	buf_printf(&resp, ",\"rol\":");
	json_cadena(&resp, rol_nombre(rol));
	buf_agregar(&resp, "}", 1);

	if (resp.fallo)
		responder_error(c, 500, "Memoria insuficiente");
	else
		responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
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

static void atender_blacklist_get(struct cliente *c)
{
	struct buf cuerpo = { 0 };
	json_blacklist(&cuerpo);
	if (cuerpo.fallo) {
		buf_liberar(&cuerpo);
		responder_error(c, 500, "Memoria insuficiente");
	} else {
		responder(c, 200, "application/json", cuerpo.datos, cuerpo.largo);
		buf_liberar(&cuerpo);
	}
}

static void atender_blacklist_post(struct cliente *c, const char *usuario)
{
	const char *cuerpo = c->peticion + c->fin_encabezados;
	long largo = peticion_content_length(c->peticion);
	if (largo < 0) {
		responder_error(c, 411, "Content-Length requerido");
		return;
	}
	size_t firmas = 0;
	char err[256] = "";
	int res = blacklist_actualizar(cuerpo, (size_t)largo, usuario, &firmas, err, sizeof(err));
	if (res == -1) {
		char msg[256];
		snprintf(msg, sizeof(msg), "Blacklist invalida: %s", err);
		responder_error(c, 400, msg);
		return;
	}
	if (res == -2) {
		responder_error(c, 500, "No se pudo guardar la blacklist");
		return;
	}

	alerta_emitir(MOT_BLACKLIST_ACTUALIZADA,
		      "Blacklist actualizada por %s: %zu firmas",
		      usuario ? usuario : "", firmas);
	archivos_despertar();

	struct info_blacklist info;
	blacklist_info(&info);
	struct buf resp = { 0 };
	buf_printf(&resp, "{\"firmas\":%zu,\"nombre\":", firmas);
	json_cadena(&resp, info.nombre);
	buf_agregar(&resp, "}", 1);
	responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_amenazas(struct cliente *c)
{
	struct buf cuerpo = { 0 };
	struct amenaza *ams = malloc(MAX_AMENAZAS * sizeof(struct amenaza));
	if (!ams) {
		responder_error(c, 500, "Memoria insuficiente");
		return;
	}
	size_t n = analisis_amenazas(ams, MAX_AMENAZAS);
	buf_printf(&cuerpo, "{\"amenazas\":[");
	for (size_t i = 0; i < n; i++) {
		if (i > 0)
			buf_agregar(&cuerpo, ",", 1);
		json_amenaza(&cuerpo, &ams[i]);
	}
	buf_agregar(&cuerpo, "]}", 2);
	free(ams);

	if (cuerpo.fallo) {
		buf_liberar(&cuerpo);
		responder_error(c, 500, "Memoria insuficiente");
	} else {
		responder(c, 200, "application/json", cuerpo.datos, cuerpo.largo);
		buf_liberar(&cuerpo);
	}
}

static void atender_cuarentena_get(struct cliente *c)
{
	struct buf cuerpo = { 0 };
	struct registro_cuarentena *rcs = malloc(MAX_CUARENTENA * sizeof(struct registro_cuarentena));
	if (!rcs) {
		responder_error(c, 500, "Memoria insuficiente");
		return;
	}
	size_t n = analisis_cuarentena(rcs, MAX_CUARENTENA);
	buf_printf(&cuerpo, "{\"cuarentena\":[");
	for (size_t i = 0; i < n; i++) {
		if (i > 0)
			buf_agregar(&cuerpo, ",", 1);
		json_registro_cuarentena(&cuerpo, &rcs[i]);
	}
	buf_agregar(&cuerpo, "]}", 2);
	free(rcs);

	if (cuerpo.fallo) {
		buf_liberar(&cuerpo);
		responder_error(c, 500, "Memoria insuficiente");
	} else {
		responder(c, 200, "application/json", cuerpo.datos, cuerpo.largo);
		buf_liberar(&cuerpo);
	}
}

static void atender_cuarentena_post(struct cliente *c, const char *usuario)
{
	const char *cuerpo = c->peticion + c->fin_encabezados;
	cJSON *root = cJSON_Parse(cuerpo);
	if (!root) {
		responder_error(c, 400, "Ruta invalida");
		return;
	}
	cJSON *ruta_item = cJSON_GetObjectItem(root, "ruta");
	if (!ruta_item || !cJSON_IsString(ruta_item) || !ruta_item->valuestring) {
		cJSON_Delete(root);
		responder_error(c, 400, "Ruta invalida");
		return;
	}
	char ruta[PATH_MAX];
	snprintf(ruta, sizeof(ruta), "%s", ruta_item->valuestring);
	cJSON_Delete(root);

	enum resultado_cuarentena res = analisis_cuarentena_manual(ruta, usuario);
	switch (res) {
	case CUARENTENA_OK: {
		struct buf resp = { 0 };
		buf_printf(&resp, "{\"ruta\":");
		json_cadena(&resp, ruta);
		buf_printf(&resp, ",\"cuarentena\":true}");
		responder(c, 200, "application/json", resp.datos, resp.largo);
		buf_liberar(&resp);
		break;
	}
	case CUARENTENA_NO_LISTADO:
		responder_error(c, 404, "El archivo no esta en el listado de analizados");
		break;
	case CUARENTENA_YA:
		responder_error(c, 409, "El archivo ya esta en cuarentena");
		break;
	case CUARENTENA_ERROR:
		responder_error(c, 500, "No se pudo poner en cuarentena");
		break;
	}
}

static long extraer_id(const char *consulta)
{
	const char *parametro = consulta;
	char *fin;
	long id;

	while (parametro && *parametro) {
		if (!strncmp(parametro, "id=", 3)) {
			errno = 0;
			id = strtol(parametro + 3, &fin, 10);
			if (errno || fin == parametro + 3 ||
			    (*fin && *fin != '&') || id <= 0 || id > INT_MAX)
				return -1;
			return id;
		}
		parametro = strchr(parametro, '&');
		if (parametro)
			parametro++;
	}
	return -1;
}

static void atender_logwatch_get(struct cliente *c)
{
	struct logwatch lista[MAX_LOGWATCH];
	char base[PATH_MAX];
	size_t n;
	struct buf resp = { 0 };

	logwatch_base(base, sizeof(base));
	n = logwatch_lista(lista, MAX_LOGWATCH);

	buf_agregar(&resp, "{\"base\":", 8);
	json_cadena(&resp, base);
	buf_agregar(&resp, ",\"logwatch\":[", 13);
	for (size_t i = 0; i < n; i++) {
		if (i > 0)
			buf_agregar(&resp, ",", 1);
		json_logwatch(&resp, &lista[i]);
	}
	buf_agregar(&resp, "]}", 2);

	responder(c, 200, "application/json", resp.datos, resp.largo);
	buf_liberar(&resp);
}

static void atender_logwatch_post(struct cliente *c, const char *usuario)
{
	struct peticion_logwatch p;
	struct logwatch creado;
	char error[256];
	int codigo = 0;
	int ret;

	if (c->fin_encabezados >= c->leidos) {
		responder_error(c, 400, "Cuerpo invalido");
		return;
	}

	const char *cuerpo = c->peticion + c->fin_encabezados;
	if (logwatch_validar(cuerpo, &p, &codigo, error, sizeof(error)) < 0) {
		responder_error(c, codigo, error);
		return;
	}

	ret = logwatch_crear(&p, "dashboard", usuario, &creado, error, sizeof(error));
	if (ret == 0) {
		registrar("logwatch %d creado por %s: '%s' en %d archivos",
			  creado.id, usuario ? usuario : "", creado.palabra, creado.n_rutas);
		struct buf resp = { 0 };
		json_logwatch(&resp, &creado);
		responder(c, 200, "application/json", resp.datos, resp.largo);
		buf_liberar(&resp);
	} else if (ret == -1) {
		responder_error(c, 409, "Limite de 16 logwatch alcanzado");
	} else {
		char msg[512];
		snprintf(msg, sizeof(msg), "No se pudo crear el logwatch: %s", error);
		responder_error(c, 500, msg);
	}
}

static void atender_logwatch_delete(struct cliente *c, const char *consulta, const char *usuario)
{
	char error[256];
	long id;
	int ret;

	id = extraer_id(consulta);
	if (id < 0) {
		responder_error(c, 400, "ID invalido");
		return;
	}

	ret = logwatch_finalizar((int)id, error, sizeof(error));
	if (ret == 0) {
		registrar("logwatch %ld finalizado por %s", id, usuario ? usuario : "");
		char resp[64];
		snprintf(resp, sizeof(resp), "{\"id\":%ld,\"finalizado\":true}", id);
		responder(c, 200, "application/json", resp, strlen(resp));
	} else if (ret == -ENOENT) {
		responder_error(c, 404, "Logwatch no encontrado");
	} else {
		char msg[512];
		snprintf(msg, sizeof(msg), "No se pudo finalizar el logwatch: %s", error);
		responder_error(c, 500, msg);
	}
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

	consulta = strchr(ruta, '?');
	if (consulta)
		*consulta++ = '\0';

	if (strncmp(ruta, "/api/", 5) == 0) {
		enum rol minimo = ROL_NINGUNO;
		int r = permiso_buscar(metodo, ruta, &minimo);
		if (r == -1) {
			responder_error(c, 404, "Recurso no encontrado");
			return;
		}
		if (r == -2) {
			responder_error(c, 405, "Metodo no permitido");
			return;
		}

		char usuario[MAX_USUARIO + 1];
		enum rol rol = ROL_NINGUNO;
		char token[TAM_TOKEN + 1];
		int tiene_cookie;

		memset(usuario, 0, sizeof(usuario));
		memset(token, 0, sizeof(token));

		tiene_cookie = (peticion_cookie(c->peticion, COOKIE_SESION, token, sizeof(token)) == 0 &&
				strlen(token) == TAM_TOKEN);

		if (minimo > ROL_NINGUNO) {
			if (!tiene_cookie || sesion_buscar(token, time(NULL), usuario, &rol) < 0) {
				responder_error(c, 401, "Sesion requerida");
				return;
			}
			if (!rol_suficiente(rol, minimo)) {
				responder_error(c, 403, "Permiso denegado");
				return;
			}
		} else if (tiene_cookie) {
			sesion_buscar(token, time(NULL), usuario, &rol);
		}

		if (!strcmp(ruta, "/api/login"))
			atender_login(c);
		else if (!strcmp(ruta, "/api/logout"))
			atender_logout(c, token);
		else if (!strcmp(ruta, "/api/sesion"))
			atender_sesion(c, usuario, rol);
		else if (!strcmp(ruta, "/api/estado"))
			atender_estado(c);
		else if (!strcmp(ruta, "/api/alertas"))
			atender_alertas(c);
		else if (!strcmp(ruta, "/api/metricas"))
			atender_metricas(c);
		else if (!strcmp(ruta, "/api/monitoreo/metricas"))
			atender_monitoreo_metricas(c, usuario);
		else if (!strcmp(ruta, "/api/monitoreo/procesos"))
			atender_monitoreo_procesos(c, usuario);
		else if (!strcmp(ruta, "/api/monitoreo"))
			atender_monitoreo(c);
		else if (!strcmp(ruta, "/api/procesos"))
			atender_procesos(c);
		else if (!strcmp(ruta, "/api/proceso/kill"))
			atender_kill(c, usuario);
		else if (!strcmp(ruta, "/api/proceso"))
			atender_proceso(c, consulta);
		else if (!strcmp(ruta, "/api/archivos"))
			atender_archivos(c);
		else if (!strcmp(ruta, "/api/monitoreo/archivos"))
			atender_monitoreo_archivos(c, usuario);
		else if (!strcmp(ruta, "/api/ruta"))
			atender_ruta(c, usuario);
		else if (!strcmp(ruta, "/api/blacklist")) {
			if (!strcmp(metodo, "GET"))
				atender_blacklist_get(c);
			else
				atender_blacklist_post(c, usuario);
		} else if (!strcmp(ruta, "/api/amenazas"))
			atender_amenazas(c);
		else if (!strcmp(ruta, "/api/cuarentena")) {
			if (!strcmp(metodo, "GET"))
				atender_cuarentena_get(c);
			else
				atender_cuarentena_post(c, usuario);
		} else if (!strcmp(ruta, "/api/logwatch")) {
			if (!strcmp(metodo, "GET"))
				atender_logwatch_get(c);
			else if (!strcmp(metodo, "POST"))
				atender_logwatch_post(c, usuario);
			else if (!strcmp(metodo, "DELETE"))
				atender_logwatch_delete(c, consulta, usuario);
		} else if (!strcmp(ruta, "/api/eventos"))
			atender_sse(c, token);
		else
			responder_error(c, 404, "Recurso no encontrado");
	} else {
		if (strcmp(metodo, "GET") != 0) {
			responder_error(c, 405, "Metodo no permitido");
			return;
		}
		atender_estatico(c, ruta);
	}
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
			 c->capacidad - 1 - c->leidos, 0);

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

	char *fin_enc = strstr(c->peticion, "\r\n\r\n");
	if (fin_enc) {
		c->fin_encabezados = (size_t)(fin_enc - c->peticion + 4);
		char metodo[8];
		if (sscanf(c->peticion, "%7s", metodo) == 1 && strcmp(metodo, "POST") == 0) {
			long largo = peticion_content_length(c->peticion);
			if (largo < 0) {
				responder_error(c, 411, "Content-Length requerido");
				cliente_vaciar(c);
				return;
			}
			if (c->fin_encabezados + (size_t)largo > c->capacidad - 1) {
				char met[8], ruta_pet[1024];
				if (sscanf(c->peticion, "%7s %1023s", met, ruta_pet) == 2 &&
				    strcmp(met, "POST") == 0 &&
				    strcmp(ruta_pet, "/api/blacklist") == 0 &&
				    (size_t)largo <= MAX_CUERPO_BLACKLIST) {
					char token[TAM_TOKEN + 1];
					char usuario[MAX_USUARIO + 1];
					enum rol rol = ROL_NINGUNO;
					int tiene_cookie;

					memset(token, 0, sizeof(token));
					memset(usuario, 0, sizeof(usuario));
					tiene_cookie = (peticion_cookie(c->peticion, COOKIE_SESION, token, sizeof(token)) == 0 &&
							strlen(token) == TAM_TOKEN);
					if (!tiene_cookie || sesion_buscar(token, time(NULL), usuario, &rol) < 0) {
						responder_error(c, 401, "Sesion requerida");
						cliente_vaciar(c);
						return;
					}
					if (!rol_suficiente(rol, ROL_ADMIN)) {
						responder_error(c, 403, "Permiso denegado");
						cliente_vaciar(c);
						return;
					}

					size_t nuevo_tam = c->fin_encabezados + (size_t)largo + 1;
					char *nuevo_buf = malloc(nuevo_tam);
					if (!nuevo_buf) {
						responder_error(c, 500, "Memoria insuficiente");
						cliente_vaciar(c);
						return;
					}
					memcpy(nuevo_buf, c->peticion, c->leidos);
					nuevo_buf[c->leidos] = '\0';
					if (c->peticion != c->peticion_fija)
						free(c->peticion);
					c->peticion = nuevo_buf;
					c->capacidad = nuevo_tam;
					if (c->leidos < c->fin_encabezados + (size_t)largo)
						return;
				} else {
					responder_error(c, 413, "Peticion demasiado grande");
					cliente_vaciar(c);
					return;
				}
			} else if (c->leidos < c->fin_encabezados + (size_t)largo) {
				return;
			}
		}
		atender(c);
	} else if (c->leidos == c->capacidad - 1) {
		responder_error(c, 431, "Encabezados demasiado grandes");
	} else {
		return;
	}

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

static void difundir_alertas(void)
{
	struct buf tramas = { 0 };
	struct alerta *lote;
	size_t n;
	size_t i;

	lote = malloc(64 * sizeof(struct alerta));
	if (!lote)
		return;

	for (;;) {
		n = alertas_desde(ultimo_difundido, lote, 64);
		if (n == 0)
			break;

		for (i = 0; i < n; i++) {
			buf_printf(&tramas, "id: %lu\nevent: alerta\ndata: ", lote[i].id);
			json_alerta(&tramas, &lote[i]);
			buf_agregar(&tramas, "\n\n", 2);
		}
		ultimo_difundido = lote[n - 1].id;
		if (n < 64)
			break;
	}

	free(lote);

	if (tramas.largo && !tramas.fallo)
		enviar_a_suscriptores(tramas.datos, tramas.largo);
	buf_liberar(&tramas);
}

static void difundir_metricas(void)
{
	struct buf tramas = { 0 };
	struct muestra *lote;
	size_t n;
	size_t i;

	lote = malloc(32 * sizeof(struct muestra));
	if (!lote)
		return;

	for (;;) {
		n = metricas_desde(ultimo_metrica_difundida, lote, 32);
		if (n == 0)
			break;

		for (i = 0; i < n; i++) {
			buf_printf(&tramas, "id: %lu\nevent: metricas\ndata: ", lote[i].id);
			json_muestra(&tramas, &lote[i]);
			buf_agregar(&tramas, "\n\n", 2);
		}
		ultimo_metrica_difundida = lote[n - 1].id;
		if (n < 32)
			break;
	}

	free(lote);

	if (tramas.largo && !tramas.fallo)
		enviar_a_suscriptores(tramas.datos, tramas.largo);
	buf_liberar(&tramas);
}

extern volatile sig_atomic_t hay_procesos_nuevos;

static void difundir_procesos(void)
{
	struct instantanea_procesos inst;
	struct buf tramas = { 0 };

	if (!__atomic_exchange_n(&hay_procesos_nuevos, 0, __ATOMIC_SEQ_CST))
		return;

	procesos_instantanea(&inst);
	buf_printf(&tramas, "event: procesos\ndata: ");
	json_procesos(&tramas, &inst);
	buf_agregar(&tramas, "\n\n", 2);

	if (tramas.largo && !tramas.fallo)
		enviar_a_suscriptores(tramas.datos, tramas.largo);
	buf_liberar(&tramas);
}

static void difundir_archivos(void)
{
	static unsigned long gen_difundida = 0;
	static unsigned long seq_difundido = 0;
	unsigned long gen = analisis_generacion();
	struct buf tramas = { 0 };

	if (gen != gen_difundida) {
		unsigned long mayor_seq;

		buf_printf(&tramas, "event: archivos\ndata: ");
		mayor_seq = json_archivos(&tramas);
		buf_agregar(&tramas, "\n\n", 2);

		gen_difundida = gen;
		seq_difundido = mayor_seq;
	} else {
		struct archivo_analizado *lote;
		size_t n;

		lote = malloc(64 * sizeof(struct archivo_analizado));
		if (!lote)
			return;

		for (;;) {
			n = analisis_desde(seq_difundido, lote, 64);
			if (n == 0)
				break;

			for (size_t i = 0; i < n; i++) {
				buf_printf(&tramas, "event: archivo\ndata: ");
				json_archivo(&tramas, &lote[i]);
				buf_agregar(&tramas, "\n\n", 2);
				if (lote[i].seq > seq_difundido)
					seq_difundido = lote[i].seq;
			}
			if (n < 64)
				break;
		}
		free(lote);
	}

	if (tramas.largo && !tramas.fallo)
		enviar_a_suscriptores(tramas.datos, tramas.largo);
	buf_liberar(&tramas);
}

static void difundir_amenazas(void)
{
	static unsigned long ultima_amenaza = 0;
	struct buf tramas = { 0 };
	struct amenaza *lote;
	size_t n;
	size_t i;

	lote = malloc(32 * sizeof(struct amenaza));
	if (!lote)
		return;

	for (;;) {
		n = analisis_amenazas_desde(ultima_amenaza, lote, 32);
		if (n == 0)
			break;

		for (i = 0; i < n; i++) {
			buf_printf(&tramas, "id: %lu\nevent: amenaza\ndata: ", lote[i].id);
			json_amenaza(&tramas, &lote[i]);
			buf_agregar(&tramas, "\n\n", 2);
		}
		ultima_amenaza = lote[n - 1].id;
		if (n < 32)
			break;
	}

	free(lote);

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
		c->peticion = c->peticion_fija;
		c->capacidad = sizeof(c->peticion_fija);
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

void servidor_bucle(int fd_escucha, const char *dir_frontend, int intervalo_s)
{
	struct pollfd sondeo[MAX_CLIENTES + 2];
	int ranura[MAX_CLIENTES + 2];
	time_t ultimo_latido = time(NULL);
	struct cliente *c;
	eventfd_t avisos;
	time_t ahora;
	int n, i;

	frontend = dir_frontend;
	config_intervalo = intervalo_s;
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
			difundir_alertas();
			difundir_metricas();
			difundir_procesos();
			difundir_archivos();
			difundir_amenazas();
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

		sesion_purgar(ahora);

		if (ahora - ultimo_latido >= LATIDO_S) {
			enviar_a_suscriptores(": latido\n\n", 10);
			ultimo_latido = ahora;
		}
		for (i = 0; i < MAX_CLIENTES; i++) {
			c = &clientes[i];
			if (c->fd >= 0) {
				if (!c->sse && ahora - c->ultimo_uso > INACTIVIDAD_S) {
					cliente_cerrar(c);
				} else if (c->sse && c->token[0] != '\0' && !sesion_vigente(c->token, ahora)) {
					cliente_cerrar(c);
				}
			}
		}

		if (sondeo[0].revents & POLLIN)
			aceptar(fd_escucha);
	}

	for (i = 0; i < MAX_CLIENTES; i++)
		if (clientes[i].fd >= 0)
			cliente_cerrar(&clientes[i]);
	close(fd_escucha);
}
