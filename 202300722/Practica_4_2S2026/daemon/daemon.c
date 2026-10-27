#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

/*
 * ============================================================
 * DAEMON PRACTICA 4 - SISTEMAS OPERATIVOS 2
 * Kernel 6.18.48
 *
 * Integra:
 *   - P2: monitoreo de procesos
 *   - P2: informacion de memoria del proceso
 *   - P3: log_watch
 *   - P3/P4: IPC
 *   - P4: servidor HTTP + API JSON
 * ============================================================
 */

/* ============================================================
 * NUMEROS DE SYSCALL
 * ============================================================ */

#define SYS_GET_PROCESS_INFO       470
#define SYS_GET_PROCESSES_STATE    471
#define SYS_IPC_CHANNEL_SEND       472
#define SYS_IPC_CHANNEL_RECEIVE    473
#define SYS_START_LOG_WATCH        474
#define SYS_STOP_LOG_WATCH         475

/* ============================================================
 * CONSTANTES
 * ============================================================ */

#define PROCESS_NAME_LEN       64
#define IPC_MESSAGE_LEN        512

#define MAX_PROCESSES          256
#define MAX_SECURITY_EVENTS    100

#define EVENT_TEXT_LEN         512
#define EVENT_FILE_LEN         256
#define EVENT_KEYWORD_LEN      128

#define HTTP_PORT              8080
#define HTTP_BUFFER_SIZE       8192

#define DASHBOARD_DIR          "../dashboard"

/* ============================================================
 * ESTRUCTURAS DE P2
 * ============================================================ */

struct process_info_user {
    char name[PROCESS_NAME_LEN];
    pid_t pid;
    unsigned long long runtime_ms;
    unsigned long long memory_kb;
};

struct processes_state_user {
    unsigned long total;
    unsigned long running;
    unsigned long sleeping;
    unsigned long zombie;
};

/* ============================================================
 * ESTRUCTURAS DE MEMORIA
 * ============================================================ */

struct memory_info {
    unsigned long long total_kb;
    unsigned long long free_kb;
    unsigned long long available_kb;
    unsigned long long buffers_kb;
    unsigned long long cached_kb;

    unsigned long long swap_total_kb;
    unsigned long long swap_free_kb;

    unsigned long long used_kb;
    unsigned long long swap_used_kb;
};

/* ============================================================
 * ESTRUCTURA DE EVENTOS DE SEGURIDAD
 * ============================================================ */

struct security_event {
    char archivo[EVENT_FILE_LEN];
    char palabra[EVENT_KEYWORD_LEN];
    char contenido[EVENT_TEXT_LEN];
    char timestamp[64];
};

/* ============================================================
 * VARIABLES GLOBALES
 * ============================================================ */

static struct security_event security_events[MAX_SECURITY_EVENTS];
static int security_event_count = 0;

static pthread_mutex_t security_events_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t running = 1;

static int monitor_id = -1;

static int server_fd = -1;

static pthread_t ipc_thread;

/* ============================================================
 * SIGNAL
 * ============================================================ */

static void handle_signal(int sig)
{
    (void)sig;

    running = 0;

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

/* ============================================================
 * WRAPPERS SYSCALL P2
 * ============================================================ */

static long syscall_get_processes_state(
    struct processes_state_user *state)
{
    return syscall(
        SYS_GET_PROCESSES_STATE,
        state
    );
}

static long syscall_get_process_info(
    pid_t pid,
    struct process_info_user *info)
{
    return syscall(
        SYS_GET_PROCESS_INFO,
        pid,
        info
    );
}

/* ============================================================
 * MEMORIA
 * ============================================================ */

static void get_memory_info(struct memory_info *mem)
{
    FILE *file;
    char line[256];

    memset(mem, 0, sizeof(*mem));

    file = fopen("/proc/meminfo", "r");

    if (!file) {
        perror("fopen /proc/meminfo");
        return;
    }

    while (fgets(line, sizeof(line), file)) {

        unsigned long long value = 0;

        if (sscanf(line, "MemTotal: %llu kB", &value) == 1) {
            mem->total_kb = value;
        }
        else if (sscanf(line, "MemFree: %llu kB", &value) == 1) {
            mem->free_kb = value;
        }
        else if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
            mem->available_kb = value;
        }
        else if (sscanf(line, "Buffers: %llu kB", &value) == 1) {
            mem->buffers_kb = value;
        }
        else if (sscanf(line, "Cached: %llu kB", &value) == 1) {
            mem->cached_kb = value;
        }
        else if (sscanf(line, "SwapTotal: %llu kB", &value) == 1) {
            mem->swap_total_kb = value;
        }
        else if (sscanf(line, "SwapFree: %llu kB", &value) == 1) {
            mem->swap_free_kb = value;
        }
    }

    fclose(file);

    /*
     * MemAvailable representa de mejor manera la memoria que
     * todavía puede utilizar el sistema.
     */
    if (mem->total_kb >= mem->available_kb) {
        mem->used_kb =
            mem->total_kb - mem->available_kb;
    }

    if (mem->swap_total_kb >= mem->swap_free_kb) {
        mem->swap_used_kb =
            mem->swap_total_kb - mem->swap_free_kb;
    }
}

/* ============================================================
 * MOSTRAR MEMORIA EN CONSOLA
 * ============================================================ */

static void print_memory_info(void)
{
    struct memory_info mem;

    get_memory_info(&mem);

    printf("\n[MEMORIA]\n");
    printf("  Total: %llu KB\n", mem.total_kb);
    printf("  Usada: %llu KB\n", mem.used_kb);
    printf("  Libre: %llu KB\n", mem.free_kb);
    printf("  Disponible: %llu KB\n", mem.available_kb);
    printf("  Cache: %llu KB\n", mem.cached_kb);
    printf("  Swap usada: %llu KB\n", mem.swap_used_kb);
    printf("  Swap total: %llu KB\n", mem.swap_total_kb);
}

/* ============================================================
 * OBTENER LISTA DE PROCESOS
 * ============================================================ */

static int collect_processes(
    struct process_info_user *processes,
    int max_processes)
{
    DIR *dir;
    struct dirent *entry;

    int count = 0;

    dir = opendir("/proc");

    if (!dir) {
        perror("opendir /proc");
        return 0;
    }

    while ((entry = readdir(dir)) != NULL) {

        if (count >= max_processes) {
            break;
        }

        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }

        char *endptr;

        long pid_long = strtol(
            entry->d_name,
            &endptr,
            10
        );

        if (*endptr != '\0') {
            continue;
        }

        if (pid_long <= 0) {
            continue;
        }

        pid_t pid = (pid_t)pid_long;

        struct process_info_user info;

        memset(&info, 0, sizeof(info));

        if (syscall_get_process_info(pid, &info) == 0) {
            processes[count] = info;
            count++;
        }
    }

    closedir(dir);

    return count;
}

/* ============================================================
 * MOSTRAR PROCESOS
 * ============================================================ */

static void print_processes(void)
{
    struct processes_state_user state;

    memset(&state, 0, sizeof(state));

    if (syscall_get_processes_state(&state) == 0) {

        printf("\n[P2] Procesos:\n");
        printf("  Total: %lu\n", state.total);
        printf("  Running: %lu\n", state.running);
        printf("  Sleeping: %lu\n", state.sleeping);
        printf("  Zombie: %lu\n", state.zombie);
    }
    else {
        perror("[P2] get_processes_state");
    }
}

/* ============================================================
 * AGREGAR EVENTO DE SEGURIDAD
 * ============================================================ */

static void add_security_event(
    const char *archivo,
    const char *palabra,
    const char *contenido)
{
    time_t now;
    struct tm tm_info;

    struct security_event event;

    memset(&event, 0, sizeof(event));

    snprintf(
        event.archivo,
        sizeof(event.archivo),
        "%s",
        archivo
    );

    snprintf(
        event.palabra,
        sizeof(event.palabra),
        "%s",
        palabra
    );

    snprintf(
        event.contenido,
        sizeof(event.contenido),
        "%s",
        contenido
    );

    time(&now);

    localtime_r(&now, &tm_info);

    strftime(
        event.timestamp,
        sizeof(event.timestamp),
        "%Y-%m-%d %H:%M:%S",
        &tm_info
    );

    pthread_mutex_lock(&security_events_mutex);

    /*
     * Si llegamos al máximo, desplazamos los eventos.
     * De esta forma siempre conservamos los más recientes.
     */
    if (security_event_count >= MAX_SECURITY_EVENTS) {

        memmove(
            &security_events[0],
            &security_events[1],
            sizeof(struct security_event) *
            (MAX_SECURITY_EVENTS - 1)
        );

        security_event_count =
            MAX_SECURITY_EVENTS - 1;
    }

    security_events[security_event_count] = event;

    security_event_count++;

    pthread_mutex_unlock(&security_events_mutex);

    printf("\n[SEGURIDAD] Evento recibido:\n");
    printf("  Archivo: %s\n", event.archivo);
    printf("  Palabra: %s\n", event.palabra);
    printf("  Contenido: %s\n", event.contenido);
    printf("  Timestamp: %s\n", event.timestamp);
}

/* ============================================================
 * PARSEAR EVENTO IPC
 *
 * Formato:
 *
 * archivo_origen=/tmp/practica4.log|
 * palabra_detectada=ERROR|
 * contenido=ERROR prueba
 * ============================================================ */

static void process_ipc_event(const char *message)
{
    char buffer[IPC_MESSAGE_LEN];

    char archivo[EVENT_FILE_LEN] = "";
    char palabra[EVENT_KEYWORD_LEN] = "";
    char contenido[EVENT_TEXT_LEN] = "";

    char *p;
    char *end;

    snprintf(
        buffer,
        sizeof(buffer),
        "%s",
        message
    );

    p = strstr(buffer, "archivo_origen=");

    if (p) {

        p += strlen("archivo_origen=");

        end = strchr(p, '|');

        if (end) {
            size_t len = end - p;

            if (len >= sizeof(archivo)) {
                len = sizeof(archivo) - 1;
            }

            memcpy(archivo, p, len);
            archivo[len] = '\0';
        }
        else {
            snprintf(
                archivo,
                sizeof(archivo),
                "%s",
                p
            );
        }
    }

    p = strstr(buffer, "palabra_detectada=");

    if (p) {

        p += strlen("palabra_detectada=");

        end = strchr(p, '|');

        if (end) {

            size_t len = end - p;

            if (len >= sizeof(palabra)) {
                len = sizeof(palabra) - 1;
            }

            memcpy(palabra, p, len);
            palabra[len] = '\0';
        }
        else {
            snprintf(
                palabra,
                sizeof(palabra),
                "%s",
                p
            );
        }
    }

    p = strstr(buffer, "contenido=");

    if (p) {

        p += strlen("contenido=");

        snprintf(
            contenido,
            sizeof(contenido),
            "%s",
            p
        );
    }

    add_security_event(
        archivo,
        palabra,
        contenido
    );
}

/* ============================================================
 * HILO IPC
 * ============================================================ */

static void *ipc_thread_function(void *arg)
{
    (void)arg;

    printf(
        "[DAEMON] Hilo IPC iniciado.\n"
    );

    printf(
        "[DAEMON] Esperando eventos de seguridad...\n"
    );

    while (running) {

        char message[IPC_MESSAGE_LEN];

        memset(
            message,
            0,
            sizeof(message)
        );

        long result = syscall(
            SYS_IPC_CHANNEL_RECEIVE,
            message
        );

        if (result < 0) {
            if (!running) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            usleep(100000);
            continue;
        }

        message[sizeof(message) - 1] = '\0';

        printf(
            "\n[DAEMON] Evento IPC recibido: %s\n",
            message
        );

        process_ipc_event(message);
            }

            printf(
                "[DAEMON] Hilo IPC finalizado.\n"
            );

            return NULL;
}

/* ============================================================
 * INICIAR LOG WATCH
 * ============================================================ */

static int start_log_monitor(void)
{
    char *files[1];

    files[0] = "/tmp/practica4.log";

    int id = syscall(
        SYS_START_LOG_WATCH,
        files,
        1,
        "/tmp/practica4_central.log",
        "ERROR"
    );

    return id;
}

/* ============================================================
 * DETENER LOG WATCH
 * ============================================================ */

static void stop_log_monitor(void)
{
    if (monitor_id >= 0) {

        long result = syscall(
            SYS_STOP_LOG_WATCH,
            monitor_id
        );

        if (result == 0) {

            printf(
                "[DAEMON] Log monitor detenido.\n"
            );
        }

        monitor_id = -1;
    }
}

/* ============================================================
 * JSON ESCAPE
 * ============================================================ */

static void json_escape(
    const char *input,
    char *output,
    size_t output_size)
{
    size_t i = 0;
    size_t j = 0;

    if (output_size == 0) {
        return;
    }

    while (
        input[i] != '\0' &&
        j + 2 < output_size
    ) {

        unsigned char c =
            (unsigned char)input[i];

        if (c == '"') {

            if (j + 2 >= output_size) {
                break;
            }

            output[j++] = '\\';
            output[j++] = '"';
        }
        else if (c == '\\') {

            if (j + 2 >= output_size) {
                break;
            }

            output[j++] = '\\';
            output[j++] = '\\';
        }
        else if (c == '\n') {

            if (j + 2 >= output_size) {
                break;
            }

            output[j++] = '\\';
            output[j++] = 'n';
        }
        else if (c == '\r') {

            if (j + 2 >= output_size) {
                break;
            }

            output[j++] = '\\';
            output[j++] = 'r';
        }
        else if (c == '\t') {

            if (j + 2 >= output_size) {
                break;
            }

            output[j++] = '\\';
            output[j++] = 't';
        }
        else if (c < 32) {

            /*
             * Caracteres de control.
             */
            output[j++] = ' ';
        }
        else {

            output[j++] = c;
        }

        i++;
    }

    output[j] = '\0';
}

/* ============================================================
 * CONSTRUIR JSON DE STATUS
 * ============================================================ */

static int build_status_json(
    char *json,
    size_t json_size)
{
    struct processes_state_user state;
    struct memory_info mem;

    struct process_info_user processes[MAX_PROCESSES];

    int process_count;

    size_t used = 0;

    memset(&state, 0, sizeof(state));

    syscall_get_processes_state(&state);

    get_memory_info(&mem);

    process_count = collect_processes(
        processes,
        MAX_PROCESSES
    );

    used += snprintf(
        json + used,
        json_size - used,
        "{"
        "\"kernel\":\"6.18.48\","
        "\"memory\":{"
        "\"total_kb\":%llu,"
        "\"used_kb\":%llu,"
        "\"free_kb\":%llu,"
        "\"available_kb\":%llu,"
        "\"cache_kb\":%llu,"
        "\"swap_total_kb\":%llu,"
        "\"swap_used_kb\":%llu"
        "},"
        "\"processes_state\":{"
        "\"total\":%lu,"
        "\"running\":%lu,"
        "\"sleeping\":%lu,"
        "\"zombie\":%lu"
        "},"
        "\"processes\":[",
        mem.total_kb,
        mem.used_kb,
        mem.free_kb,
        mem.available_kb,
        mem.cached_kb,
        mem.swap_total_kb,
        mem.swap_used_kb,
        state.total,
        state.running,
        state.sleeping,
        state.zombie
    );

    for (int i = 0; i < process_count; i++) {

        char escaped_name[
            PROCESS_NAME_LEN * 2
        ];

        json_escape(
            processes[i].name,
            escaped_name,
            sizeof(escaped_name)
        );

        int written = snprintf(
            json + used,
            json_size - used,
            "%s{"
            "\"pid\":%d,"
            "\"name\":\"%s\","
            "\"runtime_ms\":%llu,"
            "\"memory_kb\":%llu"
            "}",
            (i > 0) ? "," : "",
            processes[i].pid,
            escaped_name,
            processes[i].runtime_ms,
            processes[i].memory_kb
        );

        if (written < 0) {
            break;
        }

        if ((size_t)written >= json_size - used) {
            used = json_size - 1;
            break;
        }

        used += written;
    }

    if (used < json_size) {

        used += snprintf(
            json + used,
            json_size - used,
            "],"
            "\"security_events\":["
        );
    }

    pthread_mutex_lock(
        &security_events_mutex
    );

    for (int i = 0;
         i < security_event_count;
         i++) {

        char escaped_file[
            EVENT_FILE_LEN * 2
        ];

        char escaped_word[
            EVENT_KEYWORD_LEN * 2
        ];

        char escaped_content[
            EVENT_TEXT_LEN * 2
        ];

        char escaped_timestamp[
            128
        ];

        json_escape(
            security_events[i].archivo,
            escaped_file,
            sizeof(escaped_file)
        );

        json_escape(
            security_events[i].palabra,
            escaped_word,
            sizeof(escaped_word)
        );

        json_escape(
            security_events[i].contenido,
            escaped_content,
            sizeof(escaped_content)
        );

        json_escape(
            security_events[i].timestamp,
            escaped_timestamp,
            sizeof(escaped_timestamp)
        );

        int written = snprintf(
            json + used,
            json_size - used,
            "%s{"
            "\"archivo\":\"%s\","
            "\"palabra\":\"%s\","
            "\"contenido\":\"%s\","
            "\"timestamp\":\"%s\""
            "}",
            (i > 0) ? "," : "",
            escaped_file,
            escaped_word,
            escaped_content,
            escaped_timestamp
        );

        if (written < 0) {
            break;
        }

        if ((size_t)written >= json_size - used) {
            used = json_size - 1;
            break;
        }

        used += written;
    }

    pthread_mutex_unlock(
        &security_events_mutex
    );

    if (used < json_size) {

        used += snprintf(
            json + used,
            json_size - used,
            "],"
            "\"monitor_id\":%d"
            "}",
            monitor_id
        );
    }

    json[json_size - 1] = '\0';

    return (int)used;
}

/* ============================================================
 * LEER ARCHIVO ESTATICO
 * ============================================================ */

static int read_file(
    const char *path,
    char **data,
    size_t *size)
{
    FILE *file;

    long length;

    char *buffer;

    file = fopen(path, "rb");

    if (!file) {
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    length = ftell(file);

    if (length < 0) {
        fclose(file);
        return -1;
    }

    rewind(file);

    buffer = malloc(
        (size_t)length + 1
    );

    if (!buffer) {
        fclose(file);
        return -1;
    }

    size_t read_count = fread(
        buffer,
        1,
        (size_t)length,
        file
    );

    fclose(file);

    buffer[read_count] = '\0';

    *data = buffer;
    *size = read_count;

    return 0;
}

/* ============================================================
 * MIME TYPE
 * ============================================================ */

static const char *get_content_type(
    const char *path)
{
    const char *extension =
        strrchr(path, '.');

    if (!extension) {
        return "text/plain";
    }

    if (strcmp(extension, ".html") == 0) {
        return "text/html; charset=utf-8";
    }

    if (strcmp(extension, ".css") == 0) {
        return "text/css; charset=utf-8";
    }

    if (strcmp(extension, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }

    if (strcmp(extension, ".json") == 0) {
        return "application/json; charset=utf-8";
    }

    return "text/plain; charset=utf-8";
}

/* ============================================================
 * ENVIAR RESPUESTA HTTP
 * ============================================================ */

static void send_http_response(
    int client_fd,
    int status_code,
    const char *status_text,
    const char *content_type,
    const char *body,
    size_t body_length)
{
    char header[1024];

    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code,
        status_text,
        content_type,
        body_length
    );

    send(
        client_fd,
        header,
        (size_t)header_length,
        0
    );

    if (body && body_length > 0) {

        send(
            client_fd,
            body,
            body_length,
            0
        );
    }
}

/* ============================================================
 * SERVIR ARCHIVO DEL DASHBOARD
 * ============================================================ */

static void serve_static_file(
    int client_fd,
    const char *filename)
{
    char path[512];

    snprintf(
        path,
        sizeof(path),
        "%s/%s",
        DASHBOARD_DIR,
        filename
    );

    char *data = NULL;
    size_t size = 0;

    if (read_file(path, &data, &size) != 0) {

        const char *message =
            "404 - Archivo no encontrado";

        send_http_response(
            client_fd,
            404,
            "Not Found",
            "text/plain; charset=utf-8",
            message,
            strlen(message)
        );

        return;
    }

    send_http_response(
        client_fd,
        200,
        "OK",
        get_content_type(path),
        data,
        size
    );

    free(data);
}

/* ============================================================
 * PROCESAR PETICION HTTP
 * ============================================================ */

static void handle_http_client(
    int client_fd)
{
    char request[HTTP_BUFFER_SIZE];

    memset(
        request,
        0,
        sizeof(request)
    );

    ssize_t received = recv(
        client_fd,
        request,
        sizeof(request) - 1,
        0
    );

    if (received <= 0) {
        return;
    }

    request[received] = '\0';

    char method[16];
    char path[512];

    memset(method, 0, sizeof(method));
    memset(path, 0, sizeof(path));

    if (sscanf(
            request,
            "%15s %511s",
            method,
            path
        ) != 2) {

        const char *message =
            "400 - Bad Request";

        send_http_response(
            client_fd,
            400,
            "Bad Request",
            "text/plain; charset=utf-8",
            message,
            strlen(message)
        );

        return;
    }

    if (strcmp(method, "GET") != 0) {

        const char *message =
            "405 - Method Not Allowed";

        send_http_response(
            client_fd,
            405,
            "Method Not Allowed",
            "text/plain; charset=utf-8",
            message,
            strlen(message)
        );

        return;
    }

    /*
     * API
     */
    if (strcmp(path, "/api/status") == 0) {

        /*
         * El JSON puede crecer dependiendo de la cantidad
         * de procesos y eventos.
         */
        size_t json_capacity =
            512 * 1024;

        char *json =
            malloc(json_capacity);

        if (!json) {

            const char *message =
                "{\"error\":\"Memoria insuficiente\"}";

            send_http_response(
                client_fd,
                500,
                "Internal Server Error",
                "application/json",
                message,
                strlen(message)
            );

            return;
        }

        int json_size =
            build_status_json(
                json,
                json_capacity
            );

        send_http_response(
            client_fd,
            200,
            "OK",
            "application/json; charset=utf-8",
            json,
            (size_t)json_size
        );

        free(json);

        return;
    }

    /*
     * Dashboard
     */
    if (strcmp(path, "/") == 0 ||
        strcmp(path, "/index.html") == 0) {

        serve_static_file(
            client_fd,
            "index.html"
        );

        return;
    }

    if (strcmp(path, "/style.css") == 0) {

        serve_static_file(
            client_fd,
            "style.css"
        );

        return;
    }

    if (strcmp(path, "/app.js") == 0) {

        serve_static_file(
            client_fd,
            "app.js"
        );

        return;
    }

    /*
     * Endpoint de salud.
     */
    if (strcmp(path, "/api/health") == 0) {

        const char *message =
            "{\"status\":\"ok\",\"daemon\":\"running\"}";

        send_http_response(
            client_fd,
            200,
            "OK",
            "application/json",
            message,
            strlen(message)
        );

        return;
    }

    /*
     * 404
     */
    {
        const char *message =
            "404 - Recurso no encontrado";

        send_http_response(
            client_fd,
            404,
            "Not Found",
            "text/plain; charset=utf-8",
            message,
            strlen(message)
        );
    }
}

/* ============================================================
 * SERVIDOR HTTP
 * ============================================================ */

static void *http_server_thread(
    void *arg)
{
    (void)arg;

    struct sockaddr_in server_addr;

    server_fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (server_fd < 0) {

        perror(
            "[HTTP] socket"
        );

        return NULL;
    }

    int opt = 1;

    setsockopt(
        server_fd,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
    );

    memset(
        &server_addr,
        0,
        sizeof(server_addr)
    );

    server_addr.sin_family =
        AF_INET;

    server_addr.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);

    server_addr.sin_port =
        htons(HTTP_PORT);

    if (bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0) {

        perror(
            "[HTTP] bind"
        );

        close(server_fd);
        server_fd = -1;

        return NULL;
    }

    if (listen(server_fd, 10) < 0) {

        perror(
            "[HTTP] listen"
        );

        close(server_fd);
        server_fd = -1;

        return NULL;
    }

    printf(
        "[HTTP] Dashboard disponible en:\n"
        "       http://127.0.0.1:%d\n",
        HTTP_PORT
    );

    while (running) {

        struct sockaddr_in client_addr;

        socklen_t client_len =
            sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_len
        );

        if (client_fd < 0) {

            if (!running) {
                break;
            }

            if (errno == EINTR) {
                continue;
            }

            perror(
                "[HTTP] accept"
            );

            continue;
        }

        handle_http_client(
            client_fd
        );

        close(client_fd);
    }

    printf(
        "[HTTP] Servidor detenido.\n"
    );

    return NULL;
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(void)
{
    printf(
        "========================================\n"
        "       DAEMON PRACTICA 4 - SO2\n"
        "       Kernel 6.18.48\n"
        "========================================\n"
    );

    /*
     * SIGNAL
     */
    signal(
        SIGINT,
        handle_signal
    );

    signal(
        SIGTERM,
        handle_signal
    );

    /*
     * P2 - estado de procesos
     */
    print_processes();

    /*
     * P2 - proceso actual
     */
    {
        struct process_info_user current;

        memset(
            &current,
            0,
            sizeof(current)
        );

        pid_t pid = getpid();

        if (syscall_get_process_info(
                pid,
                &current
            ) == 0) {

            printf(
                "\n[P2] Proceso actual:\n"
            );

            printf(
                "  PID: %d\n",
                current.pid
            );

            printf(
                "  Nombre: %s\n",
                current.name
            );

            printf(
                "  Runtime: %llu ms\n",
                current.runtime_ms
            );

            printf(
                "  Memoria: %llu KB\n",
                current.memory_kb
            );
        }
        else {

            perror(
                "[P2] get_process_info"
            );
        }
    }

    /*
     * Memoria
     */
    print_memory_info();

    /*
     * Crear archivo de prueba si no existe.
     */
    {
        FILE *file =
            fopen(
                "/tmp/practica4.log",
                "a"
            );

        if (file) {
            fclose(file);
        }
        else {
            perror(
                "[DAEMON] No se pudo abrir /tmp/practica4.log"
            );
        }
    }

    /*
     * P3 - iniciar log watch
     */
    monitor_id =
        start_log_monitor();

    if (monitor_id < 0) {

        perror(
            "[DAEMON] Error iniciando log monitor"
        );

        return EXIT_FAILURE;
    }

    printf(
        "\n[DAEMON] Log monitor iniciado. ID=%d\n",
        monitor_id
    );

    printf(
        "\n[DAEMON] Sistema listo.\n"
        "[DAEMON] Presiona Ctrl+C para salir.\n"
    );

    /*
     * Hilo IPC
     */
    if (pthread_create(
            &ipc_thread,
            NULL,
            ipc_thread_function,
            NULL
        ) != 0) {

        perror(
            "[DAEMON] pthread_create IPC"
        );

        stop_log_monitor();

        return EXIT_FAILURE;
    }

    /*
     * Hilo HTTP
     */
    pthread_t http_thread;

    if (pthread_create(
            &http_thread,
            NULL,
            http_server_thread,
            NULL
        ) != 0) {

        perror(
            "[DAEMON] pthread_create HTTP"
        );

        running = 0;

        /*
         * Esperamos que el hilo IPC termine.
         */
        pthread_cancel(ipc_thread);
        pthread_join(ipc_thread, NULL);

        stop_log_monitor();

        return EXIT_FAILURE;
    }

    /*
     * Mantener daemon activo.
     */
    while (running) {
        sleep(1);
    }

    printf(
        "\n[DAEMON] Cerrando...\n"
    );

    /*
     * Detener monitor del kernel.
     */
    stop_log_monitor();

    /*
     * Cerrar servidor HTTP.
     */
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }

    /*
     * El hilo IPC utiliza un syscall bloqueante.
     * Cancelamos el hilo para poder finalizar el daemon.
     */
    pthread_cancel(ipc_thread);

    pthread_join(
        ipc_thread,
        NULL
    );

    pthread_join(
        http_thread,
        NULL
    );

    printf(
        "[DAEMON] Daemon finalizado correctamente.\n"
    );

    return EXIT_SUCCESS;
}