#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ebpf/file_monitor.skel.h"
#include <grp.h>
#include <linux/limits.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <security/pam_appl.h>
#include <json-c/json.h>

/* Ajusta estos números si tus syscalls están registrados con otros IDs. */
#ifndef SYS_GET_PROCESS_INFO
#define SYS_GET_PROCESS_INFO 470
#define SYS_GET_PROCESSES_STATE 471
#define SYS_IPC_CHANNEL_SEND 472
#define SYS_IPC_CHANNEL_RECEIVE 473
#define SYS_START_LOG_WATCH 474
#define SYS_STOP_LOG_WATCH 475
#endif
#define HTTP_PORT 8080
#define MAX_EVENTS 256
#define MAX_FILES 512
#define BUF_SIZE 8192
#define LOG_PATH "/tmp/proyecto_unico.log"
#ifndef CONFIG_PATH
#define CONFIG_PATH "config.json"
#endif
#ifndef BLACKLIST_PATH
#define BLACKLIST_PATH "blacklist.json"
#endif
#ifndef DASHBOARD_DIR
#define DASHBOARD_DIR "../Dashboard"
#endif
#define PROCESS_NAME_LEN 64

struct process_info_user { char name[PROCESS_NAME_LEN]; pid_t pid; unsigned long long runtime_ms; unsigned long long memory_kb; };
struct processes_state_user { unsigned long total, running, sleeping, zombie; };
struct alert { char type[32], severity[16], description[512], timestamp[32]; };
struct file_record { char path[PATH_MAX]; char hash[65]; char status[16]; long long mtime; };
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t running = 1;
static int metrics_enabled = 1, file_scan_enabled = 1;
static char monitor_path[PATH_MAX] = "/tmp/proyecto_monitoreado";
static struct alert alerts[MAX_EVENTS]; static size_t alert_count;
static struct file_record files[MAX_FILES]; static size_t file_count;
static struct json_object *blacklist;
struct auth_session { char token[65]; char username[64]; char role[32]; int active; };
static struct auth_session sessions[128];
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static char logwatch_path[PATH_MAX] = "/tmp/practica4.log";
static char central_log[PATH_MAX] = "/tmp/practica4_central.log";
static char log_keyword[128] = "ERROR";
static int logwatch_id = -1;

static void log_msg(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a"); if (!f) return;
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv); char ts[32]; strftime(ts,sizeof ts,"%F %T",&tmv);
    fprintf(f,"[%s] ",ts); va_list ap; va_start(ap,fmt); vfprintf(f,fmt,ap); va_end(ap); fputc('\n',f); fclose(f);
}
static void on_signal(int sig) { (void)sig; running=0; }
static int write_pidfile(void) { int fd=open("/tmp/proyecto_unico.pid",O_CREAT|O_RDWR,0644); if(fd<0)return -1; if(flock(fd,LOCK_EX|LOCK_NB)<0){close(fd);return -1;} ftruncate(fd,0); dprintf(fd,"%ld\n",(long)getpid()); return fd; }
/* Double-fork daemonization; log file remains the diagnostic channel after terminal closes. */
static int daemonize_process(void) {
    pid_t p=fork(); if(p<0)return -1; if(p>0)_exit(0);
    if(setsid()<0)return -1;
    signal(SIGHUP,SIG_IGN);
    p=fork(); if(p<0)return -1; if(p>0)_exit(0);
    umask(027); if(chdir("/")<0)return -1;
    int fd=open("/dev/null",O_RDWR); if(fd<0)return -1;
    dup2(fd,STDIN_FILENO); dup2(fd,STDOUT_FILENO); dup2(fd,STDERR_FILENO); if(fd>2)close(fd);
    return 0;
}
static void add_alert(const char *type,const char *severity,const char *description) {
    pthread_mutex_lock(&data_lock);
    if(alert_count==MAX_EVENTS){memmove(alerts,alerts+1,(MAX_EVENTS-1)*sizeof alerts[0]);alert_count--;}
    struct alert *a=&alerts[alert_count++]; snprintf(a->type,sizeof a->type,"%s",type); snprintf(a->severity,sizeof a->severity,"%s",severity); snprintf(a->description,sizeof a->description,"%s",description);
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv); strftime(a->timestamp,sizeof a->timestamp,"%F %T",&tmv);
    pthread_mutex_unlock(&data_lock); log_msg("ALERTA %s %s: %s",severity,type,description);
}
static void load_config(void) {
    struct json_object *o=json_object_from_file(CONFIG_PATH); if(!o){log_msg("No se pudo leer %s; usando valores por defecto",CONFIG_PATH);return;}
    struct json_object *v;
    if(json_object_object_get_ex(o,"monitor_path",&v))snprintf(monitor_path,sizeof monitor_path,"%s",json_object_get_string(v));
    if(json_object_object_get_ex(o,"logwatch_path",&v))snprintf(logwatch_path,sizeof logwatch_path,"%s",json_object_get_string(v));
    if(json_object_object_get_ex(o,"central_log",&v))snprintf(central_log,sizeof central_log,"%s",json_object_get_string(v));
    if(json_object_object_get_ex(o,"log_keyword",&v))snprintf(log_keyword,sizeof log_keyword,"%s",json_object_get_string(v));
    json_object_put(o);
}
static void load_blacklist(void) { if(blacklist)json_object_put(blacklist); blacklist=json_object_from_file(BLACKLIST_PATH); if(!blacklist){blacklist=json_object_new_object();log_msg("Blacklist no disponible; se inicia vacía");} }
static int sha256_file(const char *path,char out[65]) {
    FILE *f=fopen(path,"rb"); if(!f)return -1; EVP_MD_CTX *ctx=EVP_MD_CTX_new(); if(!ctx){fclose(f);return -1;}
    const EVP_MD *md=EVP_sha256(); unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int n=0; unsigned char buf[8192]; size_t r;
    int ok=EVP_DigestInit_ex(ctx,md,NULL)==1;
    while(ok&&(r=fread(buf,1,sizeof buf,f))>0)ok=EVP_DigestUpdate(ctx,buf,r)==1;
    if(ferror(f))ok=0; if(ok)ok=EVP_DigestFinal_ex(ctx,digest,&n)==1;
    EVP_MD_CTX_free(ctx); fclose(f); if(!ok)return -1; for(unsigned int i=0;i<n;i++)sprintf(out+i*2,"%02x",digest[i]);out[64]='\0';return 0;
}
static bool hash_is_malicious(const char *hash,const char **name,const char **description) {
    struct json_object *v=NULL,*obj=NULL; if(!blacklist)return false;
    if(json_object_object_get_ex(blacklist,hash,&v)) { if(name)*name=hash;if(description)*description=json_object_get_string(v);return true; }
    /* También acepta formato {"signatures":[{"sha256":"...","name":"..."}]} */
    if(json_object_object_get_ex(blacklist,"signatures",&obj)&&json_object_is_type(obj,json_type_array)) {
        for(size_t i=0;i<json_object_array_length(obj);i++){struct json_object *it=json_object_array_get_idx(obj,i),*h=NULL,*nm=NULL,*ds=NULL;
            if(json_object_object_get_ex(it,"sha256",&h)&&strcasecmp(json_object_get_string(h),hash)==0){if(json_object_object_get_ex(it,"name",&nm)&&name)*name=json_object_get_string(nm);if(json_object_object_get_ex(it,"description",&ds)&&description)*description=json_object_get_string(ds);return true;}}
    } return false;
}
static void analyze_file(const char *path) {
    struct stat st; char hash[65]; if(stat(path,&st)<0||!S_ISREG(st.st_mode)||sha256_file(path,hash)<0)return;
    const char *threat="firma desconocida",*desc="Coincidencia en blacklist"; bool bad=hash_is_malicious(hash,&threat,&desc);
    if(bad && chmod(path,0000)==0){char msg[512];snprintf(msg,sizeof msg,"Hash malicioso '%s' detectado en %s; archivo puesto en cuarentena",threat,path);add_alert("archivo","HIGH",msg);}
    pthread_mutex_lock(&data_lock); size_t i; for(i=0;i<file_count;i++)if(strcmp(files[i].path,path)==0)break;
    if(i==MAX_FILES)i=MAX_FILES-1; if(i==file_count&&file_count<MAX_FILES)file_count++;
    snprintf(files[i].path,sizeof files[i].path,"%s",path);snprintf(files[i].hash,sizeof files[i].hash,"%s",hash);snprintf(files[i].status,sizeof files[i].status,"%s",bad?"infectado":"benigno");files[i].mtime=(long long)st.st_mtime;
    pthread_mutex_unlock(&data_lock);
    if(bad)log_msg("Hash malicioso %s: %s",threat,path);
}
static void *monitor_thread(void *unused) {
    (void)unused; unsigned long prev_fault=0,prev_maj=0; int first=1;
    while(running){
        if(metrics_enabled){FILE *f=fopen("/proc/vmstat","r");if(f){char key[64];unsigned long val;while(fscanf(f,"%63s %lu",key,&val)==2){if(!strcmp(key,"pgfault")){if(!first&&val>prev_fault){char d[128];snprintf(d,sizeof d,"Fallos de página menores: +%lu",val-prev_fault); /* métrica informativa */}prev_fault=val;}else if(!strcmp(key,"pgmajfault"))prev_maj=val;}fclose(f);first=0;}
            struct processes_state_user ps={0}; long rc=syscall(SYS_GET_PROCESSES_STATE,&ps); if(rc<0)log_msg("sys_get_processes_state falló: %s",strerror(errno));
        } sleep(2);
    } return NULL;
}
/* The kernel IPC receive syscall is blocking by design: this thread does not poll. */
static void *ipc_thread(void *unused) {
    (void)unused;
    while(running){char msg[512]={0}; long rc=syscall(SYS_IPC_CHANNEL_RECEIVE,msg);
        if(rc<0){if(errno==EINTR)continue;if(!running)break;log_msg("ipc receive: %s",strerror(errno));usleep(100000);continue;}
        msg[sizeof msg-1]='\0'; char d[600];snprintf(d,sizeof d,"Palabra clave detectada en log: %.500s",msg);add_alert("logs","MEDIUM",d);
    } return NULL;
}
struct file_event { __u32 pid; __u32 action; char path[256]; };
static int on_file_event(void *ctx, void *data, size_t size) {
    (void)ctx; if(size < sizeof(struct file_event))return 0;
    struct file_event *e=data; if(!file_scan_enabled)return 0;
    log_msg("eBPF evento pid=%u action=%u path=%s",e->pid,e->action,e->path);
    analyze_file(e->path); return 0;
}
static void *file_thread(void *unused) {
    (void)unused;
    struct file_monitor_bpf *skel=file_monitor_bpf__open();
    if(!skel){log_msg("No se pudo abrir el skeleton eBPF; compila con make ebpf");return NULL;}
    int err=file_monitor_bpf__load(skel); if(err){log_msg("No se pudo cargar eBPF: %d",err);file_monitor_bpf__destroy(skel);return NULL;}
    int map_path=bpf_map__fd(skel->maps.configured_dir), map_pid=bpf_map__fd(skel->maps.daemon_pid);
    __u32 zero=0,pid=(__u32)getpid(); char pathbuf[256]={0};snprintf(pathbuf,sizeof pathbuf,"%s",monitor_path);
    bpf_map_update_elem(map_path,&zero,pathbuf,BPF_ANY);bpf_map_update_elem(map_pid,&zero,&pid,BPF_ANY);
    err=file_monitor_bpf__attach(skel);if(err){log_msg("No se pudo adjuntar eBPF: %d",err);file_monitor_bpf__destroy(skel);return NULL;}
    struct ring_buffer *rb=ring_buffer__new(bpf_map__fd(skel->maps.events),on_file_event,NULL,NULL);
    if(!rb){log_msg("No se pudo crear Ring Buffer");file_monitor_bpf__destroy(skel);return NULL;}
    log_msg("Sensor eBPF activo en %s",monitor_path);add_alert("archivos","LOW","Monitoreo de archivos activado");
    while(running){if(!file_scan_enabled){usleep(200000);continue;}err=ring_buffer__poll(rb,250);if(err==-EINTR)continue;if(err<0){log_msg("ring_buffer__poll: %d",err);break;}}
    ring_buffer__free(rb);file_monitor_bpf__destroy(skel);return NULL;
}
static int pam_conv_func(int n,const struct pam_message **msg,struct pam_response **resp,void *data) {
    if(n<=0)return PAM_CONV_ERR; struct pam_response *r=calloc((size_t)n,sizeof *r);if(!r)return PAM_CONV_ERR;
    const char *password=(const char*)data; for(int i=0;i<n;i++){if(msg[i]->msg_style==PAM_PROMPT_ECHO_OFF||msg[i]->msg_style==PAM_PROMPT_ECHO_ON)r[i].resp=strdup(password);else r[i].resp=strdup("");}
    *resp=r;return PAM_SUCCESS;
}
static int authenticate_pam(const char *user,const char *password,char *role,size_t role_sz) {
    struct pam_conv conv={pam_conv_func,(void*)password}; pam_handle_t *pamh=NULL;
    int rc=pam_start("login",user,&conv,&pamh);if(rc==PAM_SUCCESS)rc=pam_authenticate(pamh,0);if(rc==PAM_SUCCESS)rc=pam_acct_mgmt(pamh,0);
    if(rc==PAM_SUCCESS){struct passwd *pw=getpwnam(user);if(pw){gid_t g[128];int ng=128;if(getgrouplist(user,pw->pw_gid,g,&ng)>=0){for(int i=0;i<ng;i++){struct group *gr=getgrgid(g[i]);if(gr&&strcmp(gr->gr_name,"admin_user")==0){snprintf(role,role_sz,"admin_user");break;}if(gr&&strcmp(gr->gr_name,"common_user")==0)snprintf(role,role_sz,"common_user");}}}}
    if(pamh)pam_end(pamh,rc);return rc==PAM_SUCCESS&&role[0]?0:-1;
}
static const char *get_header(const char *req,const char *key,char *out,size_t n);
static void make_token(char out[65]) {
    unsigned char bytes[32]; if(RAND_bytes(bytes,sizeof bytes)!=1){snprintf(out,65,"%ld-%ld",(long)getpid(),(long)time(NULL));return;}
    for(size_t i=0;i<sizeof bytes;i++)sprintf(out+i*2,"%02x",bytes[i]);out[64]='\0';
}
static int create_session(const char *username,const char *role,char out[65]) {
    pthread_mutex_lock(&session_lock);int slot=-1;for(int i=0;i<128;i++)if(!sessions[i].active){slot=i;break;}if(slot<0){pthread_mutex_unlock(&session_lock);return -1;}
    make_token(out);snprintf(sessions[slot].token,sizeof sessions[slot].token,"%s",out);snprintf(sessions[slot].username,sizeof sessions[slot].username,"%s",username);snprintf(sessions[slot].role,sizeof sessions[slot].role,"%s",role);sessions[slot].active=1;pthread_mutex_unlock(&session_lock);return 0;
}
static int role_for_request(const char *req,char role[32]) {
    char auth[256];if(!get_header(req,"Authorization:",auth,sizeof auth))return -1;
    if(strncmp(auth,"Bearer ",7)!=0)return -1;const char *token=auth+7;int ok=-1;
    pthread_mutex_lock(&session_lock);for(int i=0;i<128;i++)if(sessions[i].active&&strcmp(sessions[i].token,token)==0){snprintf(role,32,"%s",sessions[i].role);ok=0;break;}pthread_mutex_unlock(&session_lock);return ok;
}
static void send_response(int fd,int code,const char *status,const char *body,const char *type) {
    char head[512];int n=snprintf(head,sizeof head,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",code,status,type,strlen(body));send(fd,head,(size_t)n,0);send(fd,body,strlen(body),0);
}
static const char *get_header(const char *req,const char *key,char *out,size_t n) { const char *p=strcasestr(req,key);if(!p)return NULL;p+=strlen(key);while(*p==' '||*p=='\t')p++;const char *e=strstr(p,"\r\n");if(!e)return NULL;size_t l=(size_t)(e-p);if(l>=n)l=n-1;memcpy(out,p,l);out[l]=0;return out; }
static struct json_object *status_json(void) {
    struct json_object *root=json_object_new_object(),*mem=json_object_new_object(),*arr=json_object_new_array(),*fa=json_object_new_array();
    FILE *f=fopen("/proc/meminfo","r");char key[64];unsigned long long val;char unit[16];unsigned long long mt=0,mf=0,ma=0,st=0,sf=0,ca=0,bu=0,ac=0,in=0;
    if(f){while(fscanf(f,"%63s %llu %15s",key,&val,unit)>=2){if(!strcmp(key,"MemTotal:"))mt=val;else if(!strcmp(key,"MemFree:"))mf=val;else if(!strcmp(key,"MemAvailable:"))ma=val;else if(!strcmp(key,"SwapTotal:"))st=val;else if(!strcmp(key,"SwapFree:"))sf=val;else if(!strcmp(key,"Cached:"))ca=val;else if(!strcmp(key,"Buffers:"))bu=val;else if(!strcmp(key,"Active:"))ac=val;else if(!strcmp(key,"Inactive:"))in=val;}fclose(f);}
    json_object_object_add(mem,"total_kb",json_object_new_uint64(mt));json_object_object_add(mem,"free_kb",json_object_new_uint64(mf));json_object_object_add(mem,"available_kb",json_object_new_uint64(ma));json_object_object_add(mem,"used_kb",json_object_new_uint64(mt>ma?mt-ma:0));json_object_object_add(mem,"swap_total_kb",json_object_new_uint64(st));json_object_object_add(mem,"swap_free_kb",json_object_new_uint64(sf));json_object_object_add(mem,"cached_kb",json_object_new_uint64(ca));json_object_object_add(mem,"buffers_kb",json_object_new_uint64(bu));json_object_object_add(mem,"active_kb",json_object_new_uint64(ac));json_object_object_add(mem,"inactive_kb",json_object_new_uint64(in));
    pthread_mutex_lock(&data_lock);for(size_t i=0;i<alert_count;i++){struct json_object *o=json_object_new_object();json_object_object_add(o,"type",json_object_new_string(alerts[i].type));json_object_object_add(o,"severity",json_object_new_string(alerts[i].severity));json_object_object_add(o,"description",json_object_new_string(alerts[i].description));json_object_object_add(o,"timestamp",json_object_new_string(alerts[i].timestamp));json_object_array_add(arr,o);}for(size_t i=0;i<file_count;i++){struct json_object *o=json_object_new_object();json_object_object_add(o,"path",json_object_new_string(files[i].path));json_object_object_add(o,"sha256",json_object_new_string(files[i].hash));json_object_object_add(o,"status",json_object_new_string(files[i].status));json_object_object_add(o,"mtime",json_object_new_int64(files[i].mtime));json_object_array_add(fa,o);}pthread_mutex_unlock(&data_lock);
    struct json_object *ps=json_object_new_object();struct processes_state_user p={0};if(syscall(SYS_GET_PROCESSES_STATE,&p)<0){p.total=0;}
    json_object_object_add(ps,"total",json_object_new_uint64(p.total));json_object_object_add(ps,"running",json_object_new_uint64(p.running));json_object_object_add(ps,"sleeping",json_object_new_uint64(p.sleeping));json_object_object_add(ps,"zombie",json_object_new_uint64(p.zombie));
    json_object_object_add(root,"memory",mem);json_object_object_add(root,"processes_state",ps);json_object_object_add(root,"alerts",arr);json_object_object_add(root,"files",fa);json_object_object_add(root,"metrics_enabled",json_object_new_boolean(metrics_enabled));json_object_object_add(root,"file_scan_enabled",json_object_new_boolean(file_scan_enabled));json_object_object_add(root,"monitor_path",json_object_new_string(monitor_path));return root;
}
static void handle_client(int fd) {
    char req[BUF_SIZE]={0};ssize_t got=recv(fd,req,sizeof req-1,0);if(got<=0)return;
    char method[16]={0},path[256]={0};sscanf(req,"%15s %255s",method,path);
    if(!strcmp(method,"OPTIONS")){send_response(fd,204,"No Content","","application/json");return;}
    if(!strcmp(path,"/api/status")){char auth_role[32]={0};if(role_for_request(req,auth_role)<0){send_response(fd,401,"Unauthorized","{\"error\":\"autenticación requerida\"}","application/json");return;}struct json_object *o=status_json();const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PRETTY);send_response(fd,200,"OK",s,"application/json");json_object_put(o);return;}
    if(!strcmp(path,"/api/login")&&!strcmp(method,"POST")){char *body=strstr(req,"\r\n\r\n");if(!body){send_response(fd,400,"Bad Request","{\"error\":\"body requerido\"}","application/json");return;}body+=4;struct json_object *j=json_tokener_parse(body),*u=NULL,*p=NULL;const char *user=NULL,*pass=NULL;if(j&&json_object_object_get_ex(j,"username",&u)&&json_object_object_get_ex(j,"password",&p)){user=json_object_get_string(u);pass=json_object_get_string(p);}char role[32]={0};if(user&&pass&&authenticate_pam(user,pass,role,sizeof role)==0){char token[65],out[320];if(create_session(user,role,token)<0){send_response(fd,503,"Service Unavailable","{\"ok\":false,\"error\":\"sesiones llenas\"}","application/json");if(j)json_object_put(j);return;}snprintf(out,sizeof out,"{\"ok\":true,\"username\":\"%s\",\"role\":\"%s\",\"token\":\"%s\"}",user,role,token);add_alert("sesion","LOW","Inicio de sesión exitoso");send_response(fd,200,"OK",out,"application/json");}else{add_alert("sesion","MEDIUM","Inicio de sesión fallido");send_response(fd,401,"Unauthorized","{\"ok\":false,\"error\":\"Credenciales o grupo inválidos\"}","application/json");}if(j)json_object_put(j);return;}
    if(!strcmp(path,"/api/control")&&!strcmp(method,"POST")){char auth_role[32]={0};if(role_for_request(req,auth_role)<0||strcmp(auth_role,"admin_user")){send_response(fd,403,"Forbidden","{\"error\":\"solo admin_user autenticado\"}","application/json");return;}char *body=strstr(req,"\r\n\r\n");struct json_object *j=body?json_tokener_parse(body+4):NULL,*v=NULL;const char *action=NULL;if(j&&json_object_object_get_ex(j,"action",&v))action=json_object_get_string(v);if(action&&!strcmp(action,"metrics_on")){metrics_enabled=1;add_alert("sistema","LOW","Monitoreo del sistema activado");}else if(action&&!strcmp(action,"metrics_off")){metrics_enabled=0;add_alert("sistema","MEDIUM","Monitoreo del sistema desactivado");}else if(action&&!strcmp(action,"files_on")){file_scan_enabled=1;add_alert("archivos","LOW","Escaneo de archivos activado");}else if(action&&!strcmp(action,"files_off")){file_scan_enabled=0;add_alert("archivos","MEDIUM","Escaneo de archivos desactivado");}else if(action&&!strcmp(action,"reload_blacklist")){load_blacklist();add_alert("base_datos","MEDIUM","Blacklist recargada");}else{if(j)json_object_put(j);send_response(fd,400,"Bad Request","{\"error\":\"acción desconocida\"}","application/json");return;}if(j)json_object_put(j);send_response(fd,200,"OK","{\"ok\":true}","application/json");return;}
    if(!strcmp(path,"/")||!strcmp(path,"/index.html")||!strcmp(path,"/styles.css")||!strcmp(path,"/app.js")){
        char base[PATH_MAX]; snprintf(base,sizeof base,"%s/%s",DASHBOARD_DIR,strstr(path,".css")?"styles.css":(strstr(path,".js")?"app.js":"index.html"));
        FILE *f=fopen(base,"r");
        if(!f){send_response(fd,404,"Not Found","Dashboard no encontrado","text/plain");return;}
        char b[BUF_SIZE*4];size_t n=fread(b,1,sizeof b-1,f);b[n]=0;fclose(f);const char *ct=strstr(path,".css")?"text/css; charset=utf-8":(strstr(path,".js")?"application/javascript; charset=utf-8":"text/html; charset=utf-8");send_response(fd,200,"OK",b,ct);return;}
    send_response(fd,404,"Not Found","{\"error\":\"ruta no encontrada\"}","application/json");
}
static void *http_thread(void *unused) {
    (void)unused;int s=socket(AF_INET,SOCK_STREAM,0);if(s<0){log_msg("socket: %s",strerror(errno));return NULL;}int one=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(HTTP_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(bind(s,(struct sockaddr*)&a,sizeof a)<0||listen(s,32)<0){log_msg("bind/listen: %s",strerror(errno));close(s);return NULL;}fcntl(s,F_SETFL,fcntl(s,F_GETFL)|O_NONBLOCK);log_msg("HTTP escuchando en http://127.0.0.1:%d",HTTP_PORT);
    while(running){int c=accept(s,NULL,NULL);if(c<0){if(errno==EAGAIN||errno==EWOULDBLOCK){usleep(50000);continue;}if(errno==EINTR)continue;break;}handle_client(c);close(c);}close(s);return NULL;
}
int main(int argc,char **argv) {
    bool foreground=(argc>1&&!strcmp(argv[1],"--foreground"));
    /* Leer rutas relativas antes de chdir("/") durante la daemonización. */
    load_config();load_blacklist();
    if(!foreground&&daemonize_process()<0)return EXIT_FAILURE;
    signal(SIGTERM,on_signal);signal(SIGINT,on_signal);signal(SIGHUP,on_signal);
    int pidfd=write_pidfile();if(pidfd<0){log_msg("Ya existe una instancia o no se pudo crear pidfile");return EXIT_FAILURE;}
    mkdir(monitor_path,0750);log_msg("Daemon iniciado PID=%ld",(long)getpid());add_alert("sistema","LOW","Monitoreo del sistema activado");
    long lw=syscall(SYS_START_LOG_WATCH,logwatch_path,central_log,log_keyword);if(lw>=0){logwatch_id=(int)lw;log_msg("log_watch iniciado ID=%d",logwatch_id);}else{log_msg("No se pudo iniciar log_watch: %s",strerror(errno));}
    pthread_t tmon,tipc,tfile,thttp;pthread_create(&tmon,NULL,monitor_thread,NULL);pthread_create(&tipc,NULL,ipc_thread,NULL);pthread_create(&tfile,NULL,file_thread,NULL);pthread_create(&thttp,NULL,http_thread,NULL);
    while(running)sleep(1);
    /* The blocking IPC syscall must be interrupted by a signal or stopped by the existing syscall API. */
    pthread_kill(tipc,SIGTERM);pthread_join(tmon,NULL);pthread_join(tfile,NULL);pthread_join(tipc,NULL);pthread_join(thttp,NULL);
    if(logwatch_id>=0)syscall(SYS_STOP_LOG_WATCH,logwatch_id);log_msg("Daemon detenido limpiamente");if(blacklist)json_object_put(blacklist);close(pidfd);unlink("/tmp/proyecto_unico.pid");return 0;
}
