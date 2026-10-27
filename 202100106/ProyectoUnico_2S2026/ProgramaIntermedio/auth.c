#include <grp.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "comun.h"

struct permiso {
	const char *metodo;
	const char *ruta;
	enum rol minimo;
};

static const struct permiso permisos[] = {
	{ "POST", "/api/login",              ROL_NINGUNO },
	{ "POST", "/api/logout",             ROL_NINGUNO },
	{ "GET",  "/api/sesion",             ROL_COMUN },
	{ "GET",  "/api/estado",             ROL_COMUN },
	{ "GET",  "/api/eventos",            ROL_COMUN },
	{ "GET",  "/api/alertas",            ROL_COMUN },
	{ "GET",  "/api/metricas",           ROL_COMUN },
	{ "GET",  "/api/monitoreo",          ROL_COMUN },
	{ "POST", "/api/monitoreo/metricas", ROL_ADMIN },
	{ "GET",  "/api/proceso",            ROL_ADMIN },
	{ "GET",  "/api/procesos",           ROL_COMUN },
	{ "POST", "/api/monitoreo/procesos", ROL_ADMIN },
	{ "POST", "/api/proceso/kill",       ROL_ADMIN },
	{ "GET",  "/api/archivos",           ROL_COMUN },
	{ "POST", "/api/monitoreo/archivos", ROL_ADMIN },
	{ "POST", "/api/ruta",               ROL_ADMIN },
	{ "GET",  "/api/blacklist",          ROL_ADMIN },
	{ "POST", "/api/blacklist",          ROL_ADMIN },
	{ "GET",  "/api/amenazas",           ROL_COMUN },
	{ "GET",  "/api/cuarentena",         ROL_ADMIN },
	{ "POST", "/api/cuarentena",         ROL_ADMIN },
	{ "GET",  "/api/logwatch",           ROL_ADMIN },
	{ "POST", "/api/logwatch",           ROL_ADMIN },
	{ "DELETE", "/api/logwatch",         ROL_ADMIN },
};

static const size_t n_permisos = sizeof(permisos) / sizeof(permisos[0]);

struct sesion {
	int activa;
	char token[TAM_TOKEN + 1];
	char usuario[MAX_USUARIO + 1];
	enum rol rol;
	time_t ultimo_uso;
};

static struct sesion sesiones[MAX_SESIONES];
static pthread_mutex_t lock_sesiones = PTHREAD_MUTEX_INITIALIZER;

struct credenciales {
	const char *usuario;
	const char *password;
};

static int conversar(int n, const struct pam_message **mensajes,
		     struct pam_response **respuestas, void *datos)
{
	struct credenciales *cred = (struct credenciales *)datos;
	struct pam_response *resp;

	if (n <= 0 || n > 32 || !mensajes || !respuestas)
		return PAM_CONV_ERR;

	resp = calloc((size_t)n, sizeof(struct pam_response));
	if (!resp)
		return PAM_CONV_ERR;

	for (int i = 0; i < n; i++) {
		switch (mensajes[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			resp[i].resp = strdup(cred->password ? cred->password : "");
			if (!resp[i].resp)
				goto fallo;
			break;
		case PAM_PROMPT_ECHO_ON:
			resp[i].resp = strdup(cred->usuario ? cred->usuario : "");
			if (!resp[i].resp)
				goto fallo;
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			resp[i].resp = NULL;
			break;
		default:
			goto fallo;
		}
	}

	*respuestas = resp;
	return PAM_SUCCESS;

fallo:
	for (int i = 0; i < n; i++) {
		if (resp[i].resp) {
			explicit_bzero(resp[i].resp, strlen(resp[i].resp));
			free(resp[i].resp);
		}
	}
	free(resp);
	return PAM_CONV_ERR;
}

static void sin_espera(int estado, unsigned int microsegundos, void *datos)
{
	(void)estado;
	(void)microsegundos;
	(void)datos;
}

const char *rol_nombre(enum rol rol)
{
	if (rol == ROL_ADMIN)
		return "admin_user";
	if (rol == ROL_COMUN)
		return "common_user";
	return "ninguno";
}

enum rol rol_de_grupos(int en_admin, int en_comun)
{
	if (en_admin)
		return ROL_ADMIN;
	if (en_comun)
		return ROL_COMUN;
	return ROL_NINGUNO;
}

int rol_suficiente(enum rol rol, enum rol minimo)
{
	return (int)rol >= (int)minimo;
}

enum rol auth_rol_de_usuario(const char *usuario)
{
	struct passwd *pw;
	gid_t *grupos;
	int ngrupos = 64;
	gid_t gid_admin = 0;
	gid_t gid_comun = 0;
	int tiene_admin = 0;
	int tiene_comun = 0;
	struct group *gr;
	int en_admin = 0;
	int en_comun = 0;

	if (!usuario || *usuario == '\0')
		return ROL_NINGUNO;

	pw = getpwnam(usuario);
	if (!pw)
		return ROL_NINGUNO;

	grupos = malloc(sizeof(gid_t) * (size_t)ngrupos);
	if (!grupos)
		return ROL_NINGUNO;

	if (getgrouplist(usuario, pw->pw_gid, grupos, &ngrupos) == -1) {
		gid_t *tmp = realloc(grupos, sizeof(gid_t) * (size_t)ngrupos);
		if (!tmp) {
			free(grupos);
			return ROL_NINGUNO;
		}
		grupos = tmp;
		if (getgrouplist(usuario, pw->pw_gid, grupos, &ngrupos) == -1) {
			free(grupos);
			return ROL_NINGUNO;
		}
	}

	gr = getgrnam("admin_user");
	if (gr) {
		gid_admin = gr->gr_gid;
		tiene_admin = 1;
	}

	gr = getgrnam("common_user");
	if (gr) {
		gid_comun = gr->gr_gid;
		tiene_comun = 1;
	}

	for (int i = 0; i < ngrupos; i++) {
		if (tiene_admin && grupos[i] == gid_admin)
			en_admin = 1;
		if (tiene_comun && grupos[i] == gid_comun)
			en_comun = 1;
	}

	free(grupos);
	return rol_de_grupos(en_admin, en_comun);
}

int sesion_crear(const char *usuario, enum rol rol, time_t ahora, char token[TAM_TOKEN + 1])
{
	unsigned char bytes[32];
	int slot = -1;
	time_t mas_antiguo = 0;

	if (!usuario || !token)
		return -1;

	if (getrandom(bytes, sizeof(bytes), 0) != (ssize_t)sizeof(bytes))
		return -1;

	for (size_t i = 0; i < sizeof(bytes); i++)
		snprintf(token + i * 2, 3, "%02x", bytes[i]);
	token[TAM_TOKEN] = '\0';

	pthread_mutex_lock(&lock_sesiones);

	for (int i = 0; i < MAX_SESIONES; i++) {
		if (!sesiones[i].activa) {
			slot = i;
			break;
		}
		if (slot == -1 || sesiones[i].ultimo_uso < mas_antiguo) {
			mas_antiguo = sesiones[i].ultimo_uso;
			slot = i;
		}
	}

	if (slot >= 0) {
		sesiones[slot].activa = 1;
		memcpy(sesiones[slot].token, token, TAM_TOKEN + 1);
		size_t len_u = strlen(usuario);
		if (len_u >= MAX_USUARIO)
			len_u = MAX_USUARIO;
		memcpy(sesiones[slot].usuario, usuario, len_u);
		sesiones[slot].usuario[len_u] = '\0';
		sesiones[slot].rol = rol;
		sesiones[slot].ultimo_uso = ahora;
	}

	pthread_mutex_unlock(&lock_sesiones);
	return (slot >= 0) ? 0 : -1;
}

int sesion_buscar(const char *token, time_t ahora, char usuario[MAX_USUARIO + 1], enum rol *rol)
{
	int encontrado = -1;

	if (!token || strlen(token) != TAM_TOKEN)
		return -1;

	pthread_mutex_lock(&lock_sesiones);

	for (int i = 0; i < MAX_SESIONES; i++) {
		if (!sesiones[i].activa)
			continue;

		int dif = 0;
		for (int j = 0; j < TAM_TOKEN; j++)
			dif |= (sesiones[i].token[j] ^ token[j]);

		if (dif == 0) {
			encontrado = i;
			break;
		}
	}

	if (encontrado < 0) {
		pthread_mutex_unlock(&lock_sesiones);
		return -1;
	}

	if (difftime(ahora, sesiones[encontrado].ultimo_uso) > SESION_INACTIVIDAD_S) {
		sesiones[encontrado].activa = 0;
		explicit_bzero(sesiones[encontrado].token, sizeof(sesiones[encontrado].token));
		pthread_mutex_unlock(&lock_sesiones);
		return -1;
	}

	sesiones[encontrado].ultimo_uso = ahora;

	if (usuario) {
		memcpy(usuario, sesiones[encontrado].usuario, MAX_USUARIO + 1);
		usuario[MAX_USUARIO] = '\0';
	}
	if (rol)
		*rol = sesiones[encontrado].rol;

	pthread_mutex_unlock(&lock_sesiones);
	return 0;
}

int sesion_vigente(const char *token, time_t ahora)
{
	int encontrado = -1;

	if (!token || strlen(token) != TAM_TOKEN)
		return 0;

	pthread_mutex_lock(&lock_sesiones);

	for (int i = 0; i < MAX_SESIONES; i++) {
		if (!sesiones[i].activa)
			continue;

		int dif = 0;
		for (int j = 0; j < TAM_TOKEN; j++)
			dif |= (sesiones[i].token[j] ^ token[j]);

		if (dif == 0) {
			encontrado = i;
			break;
		}
	}

	if (encontrado < 0) {
		pthread_mutex_unlock(&lock_sesiones);
		return 0;
	}

	if (difftime(ahora, sesiones[encontrado].ultimo_uso) > SESION_INACTIVIDAD_S) {
		sesiones[encontrado].activa = 0;
		explicit_bzero(sesiones[encontrado].token, sizeof(sesiones[encontrado].token));
		pthread_mutex_unlock(&lock_sesiones);
		return 0;
	}

	pthread_mutex_unlock(&lock_sesiones);
	return 1;
}

void sesion_eliminar(const char *token)
{
	if (!token || strlen(token) != TAM_TOKEN)
		return;

	pthread_mutex_lock(&lock_sesiones);

	for (int i = 0; i < MAX_SESIONES; i++) {
		if (!sesiones[i].activa)
			continue;

		int dif = 0;
		for (int j = 0; j < TAM_TOKEN; j++)
			dif |= (sesiones[i].token[j] ^ token[j]);

		if (dif == 0) {
			sesiones[i].activa = 0;
			explicit_bzero(sesiones[i].token, sizeof(sesiones[i].token));
			break;
		}
	}

	pthread_mutex_unlock(&lock_sesiones);
}

void sesion_purgar(time_t ahora)
{
	pthread_mutex_lock(&lock_sesiones);

	for (int i = 0; i < MAX_SESIONES; i++) {
		if (!sesiones[i].activa)
			continue;

		if (difftime(ahora, sesiones[i].ultimo_uso) > SESION_INACTIVIDAD_S) {
			sesiones[i].activa = 0;
			explicit_bzero(sesiones[i].token, sizeof(sesiones[i].token));
		}
	}

	pthread_mutex_unlock(&lock_sesiones);
}

int permiso_buscar(const char *metodo, const char *ruta, enum rol *minimo)
{
	int ruta_encontrada = 0;

	if (!metodo || !ruta)
		return -1;

	for (size_t i = 0; i < n_permisos; i++) {
		if (strcmp(permisos[i].ruta, ruta) == 0) {
			ruta_encontrada = 1;
			if (strcmp(permisos[i].metodo, metodo) == 0) {
				if (minimo)
					*minimo = permisos[i].minimo;
				return 0;
			}
		}
	}

	if (ruta_encontrada)
		return -2;

	return -1;
}

int auth_verificar_pam(const char *usuario, const char *password)
{
	struct credenciales cred = { usuario, password };
	struct pam_conv conv = { conversar, &cred };
	pam_handle_t *h = NULL;
	int r;

	if (!usuario || !password)
		return -1;

	r = pam_start(SERVICIO_PAM, usuario, &conv, &h);
	if (r != PAM_SUCCESS)
		return -1;

	pam_set_item(h, PAM_FAIL_DELAY, (const void *)sin_espera);

	r = pam_authenticate(h, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK);
	if (r == PAM_SUCCESS)
		r = pam_acct_mgmt(h, PAM_SILENT | PAM_DISALLOW_NULL_AUTHTOK);

	pam_end(h, r);
	return (r == PAM_SUCCESS) ? 0 : -1;
}

void auth_iniciar(void)
{
	if (geteuid() != 0)
		registrar("La autenticacion PAM requiere ejecutar el daemon como root");

	if (access("/etc/pam.d/so2-monitor", R_OK) != 0 ||
	    !getgrnam("admin_user") || !getgrnam("common_user"))
		registrar("Falta configurar PAM o grupos: ejecuta scripts/setup_usuarios.sh");
}
