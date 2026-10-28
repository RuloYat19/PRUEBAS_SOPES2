#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ebpf/file_monitor.h"
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
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
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
#define MAX_THREATS 200
#define MAX_LOGWATCH 32
#define MAX_PROCS 4096
#define TOP_PROCS 20
#define MAX_REQUEST (2 * 1024 * 1024)
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
#define PF_KTHREAD_FLAG 0x00200000

/* Estructuras compartidas con Kernel/kernel/process_monitor.c: deben ser idénticas. */
struct process_info_user { char name[PROCESS_NAME_LEN]; pid_t pid; unsigned long long runtime_ms; unsigned long long memory_kb; };
struct processes_state_user { unsigned long total, running, sleeping, zombie; };
struct process_entry_user { pid_t pid; char state; char name[PROCESS_NAME_LEN]; unsigned long long rss_kb; };

struct alert { char type[32], severity[16], reason[96], description[512], timestamp[32]; };
struct file_record { char path[PATH_MAX]; char hash[65]; char status[16]; char threat[96]; char description[256]; long long mtime; time_t analyzed; };
struct threat_record { char name[96]; char path[PATH_MAX]; char description[256]; char hash[65]; char timestamp[32]; };
struct logwatch_entry { int id; char path[PATH_MAX]; char central[PATH_MAX]; char keyword[128]; char started[32]; };
struct metrics {
    unsigned long long mem_total, mem_free, mem_available, buffers, cached, active, inactive, swap_total, swap_free;
    unsigned long long pgfault, pgmajfault, d_pgfault, d_pgmajfault;
    char timestamp[32]; int valid;
};

static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bpf_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t running = 1;
static volatile int metrics_enabled = 1, procscan_enabled = 1, file_scan_enabled = 0;
static int file_scan_on_start = 1, sample_interval = 2;
static char monitor_path[PATH_MAX] = "/tmp/proyecto_monitoreado";
static struct alert alerts[MAX_EVENTS]; static size_t alert_count;
static struct file_record files[MAX_FILES]; static size_t file_count;
static struct threat_record threats[MAX_THREATS]; static size_t threat_count;
static struct logwatch_entry logwatches[MAX_LOGWATCH]; static size_t logwatch_count;
static struct metrics metrics;
static struct processes_state_user proc_state;
static struct process_entry_user top_procs[TOP_PROCS]; static size_t top_count;
static char proc_source[16] = "syscall";
static unsigned long long mem_total_kb;
static struct json_object *blacklist;
struct auth_session { char token[65]; char username[64]; char role[32]; int active; };
static struct auth_session sessions[128];
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static char logwatch_path[PATH_MAX] = "/tmp/practica4.log";
static char central_log[PATH_MAX] = "/tmp/practica4_central.log";
static char log_keyword[128] = "ERROR";
static struct file_monitor_bpf *skel;
static struct ring_buffer *rb;

static void log_msg(const char *fmt, ...) {
    FILE *f = fopen(LOG_PATH, "a"); if (!f) return;
    time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv); char ts[32]; strftime(ts,sizeof ts,"%F %T",&tmv);
    fprintf(f,"[%s] ",ts); va_list ap; va_start(ap,fmt); vfprintf(f,fmt,ap); va_end(ap); fputc('\n',f); fclose(f);
}
static void now_str(char *out, size_t n) { time_t now=time(NULL); struct tm tmv; localtime_r(&now,&tmv); strftime(out,n,"%F %T",&tmv); }
static void on_signal(int sig) { (void)sig; running=0; }
static int write_pidfile(void) { int fd=open("/tmp/proyecto_unico.pid",O_CREAT|O_RDWR,0644); if(fd<0)return -1; if(flock(fd,LOCK_EX|LOCK_NB)<0){close(fd);return -1;} if(ftruncate(fd,0)<0){close(fd);return -1;} dprintf(fd,"%ld\n",(long)getpid()); return fd; }
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
/* type: archivos | sistema | sesion | logs. reason = motivo corto; el resto es la descripción. */
static void add_alert(const char *type,const char *severity,const char *reason,const char *fmt,...) {
    char description[512]; va_list ap; va_start(ap,fmt); vsnprintf(description,sizeof description,fmt,ap); va_end(ap);
    pthread_mutex_lock(&data_lock);
    if(alert_count==MAX_EVENTS){memmove(alerts,alerts+1,(MAX_EVENTS-1)*sizeof alerts[0]);alert_count--;}
    struct alert *a=&alerts[alert_count++];
    snprintf(a->type,sizeof a->type,"%s",type); snprintf(a->severity,sizeof a->severity,"%s",severity);
    snprintf(a->reason,sizeof a->reason,"%s",reason); snprintf(a->description,sizeof a->description,"%s",description);
    now_str(a->timestamp,sizeof a->timestamp);
    pthread_mutex_unlock(&data_lock); log_msg("ALERTA %s %s: %s - %s",severity,type,reason,description);
}
static void load_config(void) {
    struct json_object *o=json_object_from_file(CONFIG_PATH); if(!o){log_msg("No se pudo leer %s; usando valores por defecto",CONFIG_PATH);return;}
    struct json_object *v;
    if(json_object_object_get_ex(o,"monitor_path",&v))snprintf(monitor_path,sizeof monitor_path,"%s",json_object_get_string(v));
    if(json_object_object_get_ex(o,"logwatch_path",&v))snprintf(logwatch_path,sizeof logwatch_path,"%s",json_object_get_string(v));
    if(json_object_object_get_ex(o,"central_log",&v))snprintf(central_log,sizeof central_log,"%s",json_object_get_string(v));
    if(json_object_object_get_ex(o,"log_keyword",&v))snprintf(log_keyword,sizeof log_keyword,"%s",json_object_get_string(v));
    if(json_object_object_get_ex(o,"sample_interval_seconds",&v)&&json_object_get_int(v)>0)sample_interval=json_object_get_int(v);
    if(json_object_object_get_ex(o,"file_scan_on_start",&v))file_scan_on_start=json_object_get_boolean(v);
    json_object_put(o);
}

/* ------------------------------------------------------------ blacklist */

static bool is_hex64(const char *s) { if(!s||strlen(s)!=64)return false; for(int i=0;i<64;i++)if(!isxdigit((unsigned char)s[i]))return false; return true; }
/* Formato del proyecto: { "Nombre.Amenaza": { "hash": "<sha256>", "descripcion": "..." }, ... } */
static int validate_blacklist(struct json_object *o,char *err,size_t n) {
    if(!o||!json_object_is_type(o,json_type_object)){snprintf(err,n,"El archivo debe ser un objeto JSON");return -1;}
    int count=0;
    json_object_object_foreach(o,key,value){
        struct json_object *h,*d;
        if(!json_object_is_type(value,json_type_object)){snprintf(err,n,"La entrada '%s' debe ser un objeto",key);return -1;}
        if(!json_object_object_get_ex(value,"hash",&h)||!json_object_is_type(h,json_type_string)||!is_hex64(json_object_get_string(h))){snprintf(err,n,"La entrada '%s' no tiene un 'hash' SHA-256 válido (64 hexadecimales)",key);return -1;}
        if(!json_object_object_get_ex(value,"descripcion",&d)||!json_object_is_type(d,json_type_string)){snprintf(err,n,"La entrada '%s' no tiene 'descripcion' (texto)",key);return -1;}
        count++;
    }
    if(count==0){snprintf(err,n,"La blacklist no contiene firmas");return -1;}
    return 0;
}
static void load_blacklist(void) {
    struct json_object *o=json_object_from_file(BLACKLIST_PATH); char err[256];
    if(o&&validate_blacklist(o,err,sizeof err)<0){log_msg("Blacklist %s inválida: %s",BLACKLIST_PATH,err);json_object_put(o);o=NULL;}
    if(!o){o=json_object_new_object();log_msg("Blacklist no disponible; se inicia vacía");}
    pthread_mutex_lock(&bl_lock); if(blacklist)json_object_put(blacklist); blacklist=o; pthread_mutex_unlock(&bl_lock);
}
static int blacklist_size(void) { pthread_mutex_lock(&bl_lock); int n=blacklist?json_object_object_length(blacklist):0; pthread_mutex_unlock(&bl_lock); return n; }
/* Copia nombre y descripción: el objeto JSON puede reemplazarse al subir otra blacklist. */
static bool blacklist_lookup(const char *hash,char *name,size_t nn,char *desc,size_t dn) {
    bool found=false; struct json_object *f;
    pthread_mutex_lock(&bl_lock);
    if(blacklist){json_object_object_foreach(blacklist,key,value){
        if(json_object_object_get_ex(value,"hash",&f)&&strcasecmp(json_object_get_string(f),hash)==0){
            snprintf(name,nn,"%s",key);
            snprintf(desc,dn,"%s",json_object_object_get_ex(value,"descripcion",&f)?json_object_get_string(f):"");
            found=true;break;}}}
    pthread_mutex_unlock(&bl_lock);
    return found;
}

/* ------------------------------------------------------------ archivos */

static int sha256_file(const char *path,char out[65]) {
    FILE *f=fopen(path,"rb"); if(!f)return -1; EVP_MD_CTX *ctx=EVP_MD_CTX_new(); if(!ctx){fclose(f);return -1;}
    const EVP_MD *md=EVP_sha256(); unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int n=0; unsigned char buf[8192]; size_t r;
    int ok=EVP_DigestInit_ex(ctx,md,NULL)==1;
    while(ok&&(r=fread(buf,1,sizeof buf,f))>0)ok=EVP_DigestUpdate(ctx,buf,r)==1;
    if(ferror(f))ok=0; if(ok)ok=EVP_DigestFinal_ex(ctx,digest,&n)==1;
    EVP_MD_CTX_free(ctx); fclose(f); if(!ok)return -1; for(unsigned int i=0;i<n;i++)sprintf(out+i*2,"%02x",digest[i]);out[64]='\0';return 0;
}
static int find_file_locked(const char *path) { for(size_t i=0;i<file_count;i++)if(strcmp(files[i].path,path)==0)return (int)i; return -1; }
static void add_threat_locked(const char *name,const char *path,const char *desc,const char *hash) {
    if(threat_count==MAX_THREATS){memmove(threats,threats+1,(MAX_THREATS-1)*sizeof threats[0]);threat_count--;}
    struct threat_record *t=&threats[threat_count++];
    snprintf(t->name,sizeof t->name,"%s",name); snprintf(t->path,sizeof t->path,"%s",path);
    snprintf(t->description,sizeof t->description,"%s",desc); snprintf(t->hash,sizeof t->hash,"%s",hash);
    now_str(t->timestamp,sizeof t->timestamp);
}
/*
 * Calcula SHA-256, compara con la blacklist y con el hash/timestamp anterior.
 * initial = escaneo de los archivos que ya estaban en la carpeta (no genera alertas LOW).
 */
static void analyze_file(const char *path,bool initial,unsigned int pid) {
    struct stat st; char hash[65],name[96]="",desc[256]="";
    if(lstat(path,&st)<0||!S_ISREG(st.st_mode))return;
    if(sha256_file(path,hash)<0){log_msg("No se pudo calcular SHA-256 de %s: %s",path,strerror(errno));return;}
    bool bad=blacklist_lookup(hash,name,sizeof name,desc,sizeof desc);
    bool quarantined=bad&&chmod(path,0000)==0;
    if(bad&&!quarantined)log_msg("chmod 0000 falló en %s: %s",path,strerror(errno));

    pthread_mutex_lock(&data_lock);
    int i=find_file_locked(path); bool existed=i>=0,was_q=false,changed;
    if(existed){was_q=strcmp(files[i].status,"cuarentena")==0;changed=strcmp(files[i].hash,hash)!=0||files[i].mtime!=(long long)st.st_mtime;}
    else{changed=true;if(file_count==MAX_FILES){memmove(files,files+1,(MAX_FILES-1)*sizeof files[0]);file_count--;}i=(int)file_count++;}
    struct file_record *f=&files[i]; memset(f,0,sizeof *f);
    snprintf(f->path,sizeof f->path,"%s",path); snprintf(f->hash,sizeof f->hash,"%s",hash);
    f->mtime=(long long)st.st_mtime; f->analyzed=time(NULL);
    if(bad){snprintf(f->threat,sizeof f->threat,"%s",name);snprintf(f->description,sizeof f->description,"%s",desc);}
    snprintf(f->status,sizeof f->status,"%s",(quarantined||was_q)?"cuarentena":(bad?"infectado":"benigno"));
    bool report=bad&&(!initial||changed);
    if(report)add_threat_locked(name,path,desc,hash);
    pthread_mutex_unlock(&data_lock);

    if(report){
        add_alert("archivos","HIGH","Hash malicioso detectado","Amenaza %s en %s (SHA-256 %s, PID %u)",name,path,hash,pid);
        if(quarantined)add_alert("archivos","HIGH","Archivo puesto en cuarentena","Cuarentena automática de %s (permisos 0000)",path);
    }else if(!bad&&changed&&!initial){
        add_alert("archivos","LOW","Archivo modificado","%s %s (PID %u)",path,existed?"cambió de hash o timestamp":"fue creado",pid);
    }
}
static void scan_existing_files(void) {
    char dir[PATH_MAX]; pthread_mutex_lock(&data_lock); snprintf(dir,sizeof dir,"%s",monitor_path); pthread_mutex_unlock(&data_lock);
    DIR *d=opendir(dir); if(!d){log_msg("No se pudo recorrer %s: %s",dir,strerror(errno));return;}
    struct dirent *e; int n=0;
    while((e=readdir(d))){
        if(e->d_name[0]=='.'&&(!e->d_name[1]||(e->d_name[1]=='.'&&!e->d_name[2])))continue;
        char p[PATH_MAX]; if(snprintf(p,sizeof p,"%s/%s",dir,e->d_name)>=(int)sizeof p)continue;
        analyze_file(p,true,(unsigned int)getpid()); n++;
    }
    closedir(d); log_msg("Escaneo inicial de %s: %d entradas",dir,n);
}
/* Tras subir una blacklist nueva se reevalúan los archivos benignos con su hash guardado. */
static void reevaluate_files(void) {
    struct cand { char path[PATH_MAX]; char hash[65]; } *c=calloc(MAX_FILES,sizeof *c); size_t n=0;
    if(!c)return;
    pthread_mutex_lock(&data_lock);
    for(size_t i=0;i<file_count;i++)if(!strcmp(files[i].status,"benigno")){snprintf(c[n].path,sizeof c[n].path,"%s",files[i].path);snprintf(c[n].hash,sizeof c[n].hash,"%s",files[i].hash);n++;}
    pthread_mutex_unlock(&data_lock);
    for(size_t k=0;k<n;k++){
        char name[96],desc[256]; if(!blacklist_lookup(c[k].hash,name,sizeof name,desc,sizeof desc))continue;
        bool q=chmod(c[k].path,0000)==0;
        pthread_mutex_lock(&data_lock);
        int i=find_file_locked(c[k].path);
        if(i>=0){snprintf(files[i].status,sizeof files[i].status,"%s",q?"cuarentena":"infectado");snprintf(files[i].threat,sizeof files[i].threat,"%s",name);snprintf(files[i].description,sizeof files[i].description,"%s",desc);add_threat_locked(name,c[k].path,desc,c[k].hash);}
        pthread_mutex_unlock(&data_lock);
        if(i<0)continue;
        add_alert("archivos","HIGH","Hash malicioso detectado","Amenaza %s en %s (detectada al actualizar la base de datos)",name,c[k].path);
        if(q)add_alert("archivos","HIGH","Archivo puesto en cuarentena","Cuarentena automática de %s (permisos 0000)",c[k].path);
    }
    free(c);
}
static int quarantine_file(const char *path,const char *user,char *err,size_t n) {
    pthread_mutex_lock(&data_lock); int i=find_file_locked(path);
    if(i<0){pthread_mutex_unlock(&data_lock);snprintf(err,n,"El archivo no está en la lista de archivos analizados");return -1;}
    if(!strcmp(files[i].status,"cuarentena")){pthread_mutex_unlock(&data_lock);snprintf(err,n,"El archivo ya está en cuarentena");return -1;}
    pthread_mutex_unlock(&data_lock);
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0){snprintf(err,n,"No se pudo abrir el archivo: %s",strerror(errno));return -1;}
    if(fchmod(fd,0)<0){snprintf(err,n,"chmod 0000 falló: %s",strerror(errno));close(fd);return -1;}
    close(fd);
    pthread_mutex_lock(&data_lock); i=find_file_locked(path); if(i>=0)snprintf(files[i].status,sizeof files[i].status,"cuarentena"); pthread_mutex_unlock(&data_lock);
    add_alert("archivos","HIGH","Archivo puesto en cuarentena","Cuarentena manual de %s por %s (permisos 0000)",path,user);
    return 0;
}

/* ------------------------------------------------------------ sensor eBPF */

static int on_file_event(void *ctx, void *data, size_t size) {
    (void)ctx;
    if (size < sizeof(struct file_event)) return 0;
    struct file_event *e = data;
    if (!file_scan_enabled) return 0;
    char path[sizeof(e->path)];
    snprintf(path, sizeof(path), "%s", e->path);
    log_msg("eBPF evento pid=%u action=%u path=%s", e->pid, e->action, path);

    /* sys_enter_openat llega antes de escribir: esperar hasta 3 s a que tamaño y fechas se estabilicen. */
    struct stat previous, current; int stable = 0;
    for (int attempt = 0; attempt < 30 && running; attempt++) {
        if (stat(path, &current) == 0 && S_ISREG(current.st_mode)) {
            if (stable && current.st_size == previous.st_size && current.st_mtime == previous.st_mtime &&
                current.st_ctim.tv_sec == previous.st_ctim.tv_sec && current.st_ctim.tv_nsec == previous.st_ctim.tv_nsec) {
                analyze_file(path, false, e->pid);
                return 0;
            }
            previous = current; stable = 1;
        } else stable = 0;
        usleep(100000);
    }
    log_msg("No se pudo analizar %s: archivo inexistente o inestable", path);
    return 0;
}
static int sensor_init(void) {
    skel=file_monitor_bpf__open();
    if(!skel){log_msg("No se pudo abrir el skeleton eBPF; compila con make ebpf");return -1;}
    int err=file_monitor_bpf__load(skel); if(err){log_msg("No se pudo cargar eBPF: %d",err);file_monitor_bpf__destroy(skel);skel=NULL;return -1;}
    __u32 zero=0,pid=(__u32)getpid(); char pathbuf[FILE_EVENT_PATH_LEN]={0}; snprintf(pathbuf,sizeof pathbuf,"%s",monitor_path);
    bpf_map_update_elem(bpf_map__fd(skel->maps.configured_dir),&zero,pathbuf,BPF_ANY);
    bpf_map_update_elem(bpf_map__fd(skel->maps.daemon_pid),&zero,&pid,BPF_ANY);
    rb=ring_buffer__new(bpf_map__fd(skel->maps.events),on_file_event,NULL,NULL);
    if(!rb){log_msg("No se pudo crear Ring Buffer");file_monitor_bpf__destroy(skel);skel=NULL;return -1;}
    log_msg("Sensor eBPF cargado (PID del daemon %u)",pid);
    return 0;
}
/* Activar = attach de los programas eBPF; desactivar = detach (libbpf). */
static int set_file_scan(bool on,char *err,size_t n) {
    pthread_mutex_lock(&bpf_lock);
    if(!skel){pthread_mutex_unlock(&bpf_lock);snprintf(err,n,"El sensor eBPF no está disponible");return -1;}
    if(on==(bool)file_scan_enabled){pthread_mutex_unlock(&bpf_lock);return 0;}
    if(on){int e=file_monitor_bpf__attach(skel);if(e){pthread_mutex_unlock(&bpf_lock);snprintf(err,n,"No se pudo adjuntar eBPF: %d",e);return -1;}file_scan_enabled=1;}
    else{file_monitor_bpf__detach(skel);file_scan_enabled=0;}
    pthread_mutex_unlock(&bpf_lock);
    if(on){add_alert("archivos","LOW","Escaneo de archivos activado","Sensor eBPF adjuntado en %s",monitor_path);scan_existing_files();}
    else add_alert("archivos","LOW","Escaneo de archivos desactivado","Sensor eBPF desadjuntado");
    return 0;
}
static int set_monitor_path(const char *dir,char *err,size_t n) {
    char resolved[PATH_MAX]; struct stat st;
    if(!dir||dir[0]!='/'){snprintf(err,n,"La ruta debe ser absoluta");return -1;}
    if(!realpath(dir,resolved)){snprintf(err,n,"No se pudo resolver '%s': %s",dir,strerror(errno));return -1;}
    if(stat(resolved,&st)<0||!S_ISDIR(st.st_mode)){snprintf(err,n,"'%s' no es un directorio",resolved);return -1;}
    if(!strcmp(resolved,"/")){snprintf(err,n,"No se permite monitorear la raíz");return -1;}
    if(strlen(resolved)>=FILE_EVENT_PATH_LEN-2){snprintf(err,n,"Ruta demasiado larga (máximo %d caracteres)",FILE_EVENT_PATH_LEN-3);return -1;}
    pthread_mutex_lock(&bpf_lock);
    if(!skel){pthread_mutex_unlock(&bpf_lock);snprintf(err,n,"El sensor eBPF no está disponible");return -1;}
    __u32 zero=0; char pathbuf[FILE_EVENT_PATH_LEN]={0}; snprintf(pathbuf,sizeof pathbuf,"%s",resolved);
    if(bpf_map_update_elem(bpf_map__fd(skel->maps.configured_dir),&zero,pathbuf,BPF_ANY)<0){pthread_mutex_unlock(&bpf_lock);snprintf(err,n,"No se pudo actualizar el mapa eBPF: %s",strerror(errno));return -1;}
    pthread_mutex_lock(&data_lock); snprintf(monitor_path,sizeof monitor_path,"%s",resolved); pthread_mutex_unlock(&data_lock);
    bool attached=file_scan_enabled;
    pthread_mutex_unlock(&bpf_lock);
    log_msg("Ruta a monitorear: %s",resolved);
    if(attached)scan_existing_files();
    return 0;
}
static void *file_thread(void *unused) {
    (void)unused;
    while(running){
        if(!rb){sleep(1);continue;}
        int err=ring_buffer__poll(rb,250);
        if(err<0&&err!=-EINTR){log_msg("ring_buffer__poll: %d",err);usleep(500000);}
    }
    return NULL;
}

/* ------------------------------------------------------------ métricas y procesos */

static void read_meminfo(struct metrics *m) {
    FILE *f=fopen("/proc/meminfo","r"); if(!f)return; char key[64]; unsigned long long val;
    while(fscanf(f,"%63s %llu %*[^\n]",key,&val)==2){
        if(!strcmp(key,"MemTotal:"))m->mem_total=val; else if(!strcmp(key,"MemFree:"))m->mem_free=val;
        else if(!strcmp(key,"MemAvailable:"))m->mem_available=val; else if(!strcmp(key,"Buffers:"))m->buffers=val;
        else if(!strcmp(key,"Cached:"))m->cached=val; else if(!strcmp(key,"Active:"))m->active=val;
        else if(!strcmp(key,"Inactive:"))m->inactive=val; else if(!strcmp(key,"SwapTotal:"))m->swap_total=val;
        else if(!strcmp(key,"SwapFree:"))m->swap_free=val;
    }
    fclose(f);
}
static void read_vmstat(unsigned long long *fault,unsigned long long *maj) {
    FILE *f=fopen("/proc/vmstat","r"); if(!f)return; char key[64]; unsigned long long val;
    while(fscanf(f,"%63s %llu",key,&val)==2){if(!strcmp(key,"pgfault"))*fault=val;else if(!strcmp(key,"pgmajfault"))*maj=val;}
    fclose(f);
}
static int cmp_rss(const void *a,const void *b) {
    const struct process_entry_user *x=a,*y=b; return x->rss_kb<y->rss_kb?1:(x->rss_kb>y->rss_kb?-1:0);
}
/* Respaldo si el kernel arrancado todavía no devuelve la lista (syscall de un solo argumento). */
static size_t procs_from_proc(struct process_entry_user *buf,size_t max) {
    DIR *d=opendir("/proc"); if(!d)return 0; struct dirent *e; size_t n=0; long page_kb=sysconf(_SC_PAGESIZE)/1024;
    while(n<max&&(e=readdir(d))){
        if(!isdigit((unsigned char)e->d_name[0]))continue;
        char p[300],line[512]; FILE *f; snprintf(p,sizeof p,"/proc/%s/stat",e->d_name);
        if(!(f=fopen(p,"r")))continue;
        if(!fgets(line,sizeof line,f)){fclose(f);continue;}
        fclose(f);
        char *l=strchr(line,'('),*r=strrchr(line,')'); if(!l||!r||r<l)continue;
        struct process_entry_user *pe=&buf[n]; memset(pe,0,sizeof *pe); pe->pid=atoi(line);
        size_t len=(size_t)(r-l-1); if(len>=sizeof pe->name)len=sizeof pe->name-1; memcpy(pe->name,l+1,len);
        pe->state=r[2];
        snprintf(p,sizeof p,"/proc/%s/statm",e->d_name); unsigned long long size=0,res=0;
        if((f=fopen(p,"r"))){if(fscanf(f,"%llu %llu",&size,&res)!=2)res=0;fclose(f);}
        pe->rss_kb=res*(unsigned long long)page_kb; n++;
    }
    closedir(d); return n;
}
static void *monitor_thread(void *unused) {
    (void)unused; unsigned long long prev_fault=0,prev_maj=0; int first=1;
    struct process_entry_user *buf=calloc(MAX_PROCS,sizeof *buf);
    if(!buf)log_msg("Sin memoria para la lista de procesos");
    while(running){
        if(metrics_enabled){
            struct metrics m={0}; read_meminfo(&m); read_vmstat(&m.pgfault,&m.pgmajfault);
            if(!first){m.d_pgfault=m.pgfault>=prev_fault?m.pgfault-prev_fault:0;m.d_pgmajfault=m.pgmajfault>=prev_maj?m.pgmajfault-prev_maj:0;}
            prev_fault=m.pgfault; prev_maj=m.pgmajfault; first=0;
            now_str(m.timestamp,sizeof m.timestamp); m.valid=1;
            pthread_mutex_lock(&data_lock); metrics=m; pthread_mutex_unlock(&data_lock);
        }else first=1;
        if(procscan_enabled&&buf){
            struct processes_state_user ps={0};
            long rc=syscall(SYS_GET_PROCESSES_STATE,&ps,buf,(unsigned int)MAX_PROCS);
            if(rc<0)log_msg("sys_get_processes_state falló: %s",strerror(errno));
            else{
                size_t n=(size_t)rc; const char *src="syscall";
                if(n==0){n=procs_from_proc(buf,MAX_PROCS);src="proc";}
                qsort(buf,n,sizeof *buf,cmp_rss); if(n>TOP_PROCS)n=TOP_PROCS;
                pthread_mutex_lock(&data_lock); proc_state=ps; memcpy(top_procs,buf,n*sizeof *buf); top_count=n; snprintf(proc_source,sizeof proc_source,"%s",src); pthread_mutex_unlock(&data_lock);
            }
        }
        for(int i=0;i<sample_interval*10&&running;i++)usleep(100000);
    }
    free(buf); return NULL;
}
/* No se permite PID 1, el propio daemon ni hilos del kernel (PF_KTHREAD en /proc/<pid>/stat). */
static int kill_process(int pid,const char *user,char *err,size_t n) {
    if(pid<=1){snprintf(err,n,"No se permite terminar el PID %d",pid);return -1;}
    if(pid==getpid()){snprintf(err,n,"No se permite terminar el propio daemon");return -1;}
    struct process_info_user info={0};
    if(syscall(SYS_GET_PROCESS_INFO,pid,&info)<0){snprintf(err,n,"Proceso %d no encontrado: %s",pid,strerror(errno));return -1;}
    char p[64],line[1024]; snprintf(p,sizeof p,"/proc/%d/stat",pid); FILE *f=fopen(p,"r");
    if(f){if(fgets(line,sizeof line,f)){char *r=strrchr(line,')');unsigned int flags=0;int ppid=0;
        if(r&&sscanf(r+1," %*c %d %*d %*d %*d %*d %u",&ppid,&flags)==2&&((flags&PF_KTHREAD_FLAG)||ppid==2||pid==2)){fclose(f);snprintf(err,n,"No se permite terminar hilos del kernel (%s)",info.name);return -1;}}
        fclose(f);}
    if(syscall(SYS_kill,pid,SIGKILL)<0){snprintf(err,n,"kill falló: %s",strerror(errno));return -1;}
    add_alert("sistema","MEDIUM","Proceso matado","%s (PID %d) terminado por %s",info.name,pid,user);
    return 0;
}

/* ------------------------------------------------------------ log_watch + IPC */

static int start_logwatch(const char *path,const char *central,const char *keyword,char *err,size_t n) {
    pthread_mutex_lock(&data_lock); bool full=logwatch_count>=MAX_LOGWATCH; pthread_mutex_unlock(&data_lock);
    if(full){snprintf(err,n,"Máximo de %d logwatch alcanzado",MAX_LOGWATCH);return -1;}
    long id=syscall(SYS_START_LOG_WATCH,path,central,keyword);
    if(id<0){snprintf(err,n,"sys_start_log_watch(%s) falló: %s",path,strerror(errno));return -1;}
    pthread_mutex_lock(&data_lock);
    struct logwatch_entry *l=&logwatches[logwatch_count++]; memset(l,0,sizeof *l); l->id=(int)id;
    snprintf(l->path,sizeof l->path,"%s",path); snprintf(l->central,sizeof l->central,"%s",central);
    snprintf(l->keyword,sizeof l->keyword,"%s",keyword); now_str(l->started,sizeof l->started);
    pthread_mutex_unlock(&data_lock);
    log_msg("log_watch iniciado ID=%ld ruta=%s central=%s palabra=%s",id,path,central,keyword);
    return (int)id;
}
static int stop_logwatch(int id,char *err,size_t n) {
    pthread_mutex_lock(&data_lock); size_t i; for(i=0;i<logwatch_count;i++)if(logwatches[i].id==id)break;
    if(i==logwatch_count){pthread_mutex_unlock(&data_lock);snprintf(err,n,"No existe el logwatch %d",id);return -1;}
    memmove(&logwatches[i],&logwatches[i+1],(logwatch_count-i-1)*sizeof logwatches[0]); logwatch_count--;
    pthread_mutex_unlock(&data_lock);
    if(syscall(SYS_STOP_LOG_WATCH,id)<0){snprintf(err,n,"sys_stop_log_watch(%d) falló: %s",id,strerror(errno));return -1;}
    log_msg("log_watch %d detenido",id); return 0;
}
/* Mensaje del kernel: "archivo_origen=X|palabra_detectada=Y|contenido=Z" */
static void ipc_field(const char *msg,const char *key,char *out,size_t n) {
    out[0]='\0'; const char *p=strstr(msg,key); if(!p)return; p+=strlen(key);
    const char *e=strchr(p,'|'); if(!strcmp(key,"contenido="))e=NULL;
    size_t l=e?(size_t)(e-p):strlen(p); if(l>=n)l=n-1; memcpy(out,p,l); out[l]='\0';
}
/* The kernel IPC receive syscall is blocking by design: this thread does not poll. */
static void *ipc_thread(void *unused) {
    (void)unused;
    while(running){char msg[512]={0}; long rc=syscall(SYS_IPC_CHANNEL_RECEIVE,msg);
        if(rc<0){if(errno==EINTR)continue;if(!running)break;log_msg("ipc receive: %s",strerror(errno));sleep(1);continue;}
        msg[sizeof msg-1]='\0'; char file[256],word[128],line[256];
        ipc_field(msg,"archivo_origen=",file,sizeof file); ipc_field(msg,"palabra_detectada=",word,sizeof word); ipc_field(msg,"contenido=",line,sizeof line);
        if(file[0])add_alert("logs","MEDIUM","Palabra clave detectada en log","'%s' en %s: %s",word,file,line);
        else add_alert("logs","MEDIUM","Palabra clave detectada en log","%.500s",msg);
    } return NULL;
}

/* ------------------------------------------------------------ PAM y sesiones */

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
static void make_token(char out[65]) {
    unsigned char bytes[32]; if(RAND_bytes(bytes,sizeof bytes)!=1){snprintf(out,65,"%ld-%ld",(long)getpid(),(long)time(NULL));return;}
    for(size_t i=0;i<sizeof bytes;i++)sprintf(out+i*2,"%02x",bytes[i]);out[64]='\0';
}
static int create_session(const char *username,const char *role,char out[65]) {
    pthread_mutex_lock(&session_lock);int slot=-1;for(int i=0;i<128;i++)if(!sessions[i].active){slot=i;break;}if(slot<0){pthread_mutex_unlock(&session_lock);return -1;}
    make_token(out);snprintf(sessions[slot].token,sizeof sessions[slot].token,"%s",out);snprintf(sessions[slot].username,sizeof sessions[slot].username,"%s",username);snprintf(sessions[slot].role,sizeof sessions[slot].role,"%s",role);sessions[slot].active=1;pthread_mutex_unlock(&session_lock);return 0;
}

/* ------------------------------------------------------------ HTTP */

struct request { char method[16]; char path[256]; char query[256]; char *headers; char *body; size_t body_len; char *raw; };
struct who { char username[64]; char role[32]; };

static const char *get_header(const char *req,const char *key,char *out,size_t n) { const char *p=strcasestr(req,key);if(!p)return NULL;p+=strlen(key);while(*p==' '||*p=='\t')p++;const char *e=strstr(p,"\r\n");if(!e)return NULL;size_t l=(size_t)(e-p);if(l>=n)l=n-1;memcpy(out,p,l);out[l]=0;return out; }
static int session_for_request(const struct request *r,struct who *w) {
    char auth[256];if(!get_header(r->headers,"Authorization:",auth,sizeof auth))return -1;
    if(strncmp(auth,"Bearer ",7)!=0)return -1;const char *token=auth+7;int ok=-1;
    pthread_mutex_lock(&session_lock);for(int i=0;i<128;i++)if(sessions[i].active&&strcmp(sessions[i].token,token)==0){if(w){snprintf(w->role,sizeof w->role,"%s",sessions[i].role);snprintf(w->username,sizeof w->username,"%s",sessions[i].username);}ok=0;break;}pthread_mutex_unlock(&session_lock);return ok;
}
static void drop_session(const struct request *r) {
    char auth[256];if(!get_header(r->headers,"Authorization:",auth,sizeof auth)||strncmp(auth,"Bearer ",7))return;
    pthread_mutex_lock(&session_lock);for(int i=0;i<128;i++)if(sessions[i].active&&!strcmp(sessions[i].token,auth+7))memset(&sessions[i],0,sizeof sessions[i]);pthread_mutex_unlock(&session_lock);
}
/* Lee cabeceras y el cuerpo completo según Content-Length (la blacklist puede llegar en varios paquetes). */
static int read_request(int fd,struct request *r) {
    /* hlen es un desplazamiento (no un puntero): realloc puede mover el buffer. */
    size_t cap=16384,len=0,hlen=0,need=0; char *buf=malloc(cap+1); if(!buf)return -1;
    for(;;){
        if(len==cap){if(cap>=MAX_REQUEST){free(buf);return -2;}cap*=2;char *nb=realloc(buf,cap+1);if(!nb){free(buf);return -1;}buf=nb;}
        ssize_t got=recv(fd,buf+len,cap-len,0); if(got<=0){free(buf);return -1;} len+=(size_t)got; buf[len]='\0';
        char *hend;
        if(!hlen&&(hend=strstr(buf,"\r\n\r\n"))){
            char cl[32]; hlen=(size_t)(hend-buf)+4; hend[2]='\0';
            need=hlen+(get_header(buf,"Content-Length:",cl,sizeof cl)?strtoul(cl,NULL,10):0);
            hend[2]='\r'; if(need>MAX_REQUEST){free(buf);return -2;}
        }
        if(hlen&&len>=need)break;
    }
    buf[hlen-2]='\0'; r->raw=buf; r->headers=buf; r->body=buf+hlen; r->body_len=need-hlen; r->body[r->body_len]='\0';
    char target[512]={0}; sscanf(buf,"%15s %511s",r->method,target);
    char *q=strchr(target,'?'); if(q){*q++='\0';snprintf(r->query,sizeof r->query,"%s",q);} snprintf(r->path,sizeof r->path,"%s",target);
    return 0;
}
static void send_all(int fd,const char *p,size_t n) { while(n){ssize_t s=send(fd,p,n,MSG_NOSIGNAL);if(s<=0)return;p+=s;n-=(size_t)s;} }
static void send_response(int fd,int code,const char *body,size_t len,const char *type) {
    const char *status=code==200?"OK":code==204?"No Content":code==400?"Bad Request":code==401?"Unauthorized":code==403?"Forbidden":code==404?"Not Found":code==413?"Payload Too Large":code==503?"Service Unavailable":"Error";
    char head[512];int n=snprintf(head,sizeof head,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: Content-Type, Authorization\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",code,status,type,len);
    send_all(fd,head,(size_t)n);send_all(fd,body,len);
}
static void send_json(int fd,int code,struct json_object *o) { const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN); send_response(fd,code,s,strlen(s),"application/json; charset=utf-8"); json_object_put(o); }
static void send_error(int fd,int code,const char *msg) { struct json_object *o=json_object_new_object(); json_object_object_add(o,"ok",json_object_new_boolean(0)); json_object_object_add(o,"error",json_object_new_string(msg)); send_json(fd,code,o); }
static void send_ok(int fd) { struct json_object *o=json_object_new_object(); json_object_object_add(o,"ok",json_object_new_boolean(1)); send_json(fd,200,o); }
static void add_str(struct json_object *o,const char *k,const char *v) { json_object_object_add(o,k,json_object_new_string(v)); }
static void add_u64(struct json_object *o,const char *k,unsigned long long v) { json_object_object_add(o,k,json_object_new_uint64(v)); }
static const char *body_str(struct json_object *j,const char *k) { struct json_object *v; return j&&json_object_object_get_ex(j,k,&v)&&json_object_is_type(v,json_type_string)?json_object_get_string(v):NULL; }

static struct json_object *file_json(const struct file_record *f,bool admin) {
    struct json_object *o=json_object_new_object();
    /* "Visualizar archivos en cuarentena" es solo de admin: el usuario común lo ve como infectado. */
    const char *status=(!admin&&!strcmp(f->status,"cuarentena"))?"infectado":f->status;
    char ts[32]; time_t mt=(time_t)f->mtime; struct tm tmv; localtime_r(&mt,&tmv); strftime(ts,sizeof ts,"%F %T",&tmv);
    add_str(o,"path",f->path); add_str(o,"sha256",f->hash); add_str(o,"status",status); add_str(o,"threat",f->threat);
    add_str(o,"description",f->description); json_object_object_add(o,"mtime",json_object_new_int64(f->mtime)); add_str(o,"mtime_str",ts);
    return o;
}
static struct json_object *status_json(const struct who *w) {
    bool admin=!strcmp(w->role,"admin_user");
    struct json_object *root=json_object_new_object(),*mem=json_object_new_object(),*arr=json_object_new_array(),*fa=json_object_new_array(),*ta=json_object_new_array(),*pa=json_object_new_array(),*la=json_object_new_array(),*ps=json_object_new_object();
    pthread_mutex_lock(&data_lock);
    struct metrics m=metrics;
    add_u64(mem,"total_kb",m.mem_total); add_u64(mem,"free_kb",m.mem_free); add_u64(mem,"available_kb",m.mem_available);
    add_u64(mem,"used_kb",m.mem_total>m.mem_available?m.mem_total-m.mem_available:0); add_u64(mem,"buffers_kb",m.buffers); add_u64(mem,"cached_kb",m.cached);
    add_u64(mem,"active_kb",m.active); add_u64(mem,"inactive_kb",m.inactive); add_u64(mem,"swap_total_kb",m.swap_total); add_u64(mem,"swap_free_kb",m.swap_free);
    add_u64(mem,"pgfault",m.pgfault); add_u64(mem,"pgmajfault",m.pgmajfault); add_u64(mem,"pgfault_delta",m.d_pgfault); add_u64(mem,"pgmajfault_delta",m.d_pgmajfault);
    add_str(mem,"timestamp",m.timestamp); json_object_object_add(mem,"valid",json_object_new_boolean(m.valid));
    for(size_t i=0;i<alert_count;i++){struct json_object *o=json_object_new_object();add_str(o,"type",alerts[i].type);add_str(o,"severity",alerts[i].severity);add_str(o,"reason",alerts[i].reason);add_str(o,"description",alerts[i].description);add_str(o,"timestamp",alerts[i].timestamp);json_object_array_add(arr,o);}
    for(size_t i=0;i<file_count;i++)json_object_array_add(fa,file_json(&files[i],admin));
    for(size_t i=threat_count;i-->0;){struct json_object *o=json_object_new_object();add_str(o,"name",threats[i].name);add_str(o,"path",threats[i].path);add_str(o,"description",threats[i].description);add_str(o,"sha256",threats[i].hash);add_str(o,"timestamp",threats[i].timestamp);json_object_array_add(ta,o);}
    for(size_t i=0;i<top_count;i++){struct json_object *o=json_object_new_object();char st[2]={top_procs[i].state,0};
        json_object_object_add(o,"pid",json_object_new_int(top_procs[i].pid));add_str(o,"name",top_procs[i].name);add_str(o,"state",st);add_u64(o,"rss_kb",top_procs[i].rss_kb);
        json_object_object_add(o,"mem_percent",json_object_new_double(mem_total_kb?100.0*(double)top_procs[i].rss_kb/(double)mem_total_kb:0));json_object_array_add(pa,o);}
    for(size_t i=0;i<logwatch_count;i++){struct json_object *o=json_object_new_object();json_object_object_add(o,"id",json_object_new_int(logwatches[i].id));add_str(o,"path",logwatches[i].path);add_str(o,"central",logwatches[i].central);add_str(o,"keyword",logwatches[i].keyword);add_str(o,"started",logwatches[i].started);json_object_array_add(la,o);}
    add_u64(ps,"total",proc_state.total); add_u64(ps,"running",proc_state.running); add_u64(ps,"sleeping",proc_state.sleeping); add_u64(ps,"zombie",proc_state.zombie);
    add_str(root,"proc_source",proc_source); add_str(root,"monitor_path",monitor_path);
    pthread_mutex_unlock(&data_lock);
    json_object_object_add(root,"memory",mem); json_object_object_add(root,"processes_state",ps); json_object_object_add(root,"top_processes",pa);
    json_object_object_add(root,"alerts",arr); json_object_object_add(root,"files",fa); json_object_object_add(root,"threats",ta); json_object_object_add(root,"logwatch",la);
    json_object_object_add(root,"metrics_enabled",json_object_new_boolean(metrics_enabled));
    json_object_object_add(root,"procscan_enabled",json_object_new_boolean(procscan_enabled));
    json_object_object_add(root,"file_scan_enabled",json_object_new_boolean(file_scan_enabled));
    json_object_object_add(root,"sensor_available",json_object_new_boolean(skel!=NULL));
    json_object_object_add(root,"blacklist_count",json_object_new_int(blacklist_size()));
    add_str(root,"username",w->username); add_str(root,"role",w->role);
    return root;
}
static void serve_static(int fd,const char *path) {
    const char *name=!strcmp(path,"/styles.css")?"styles.css":!strcmp(path,"/app.js")?"app.js":"index.html";
    const char *ct=strstr(name,".css")?"text/css; charset=utf-8":strstr(name,".js")?"application/javascript; charset=utf-8":"text/html; charset=utf-8";
    char base[PATH_MAX]; snprintf(base,sizeof base,"%s/%s",DASHBOARD_DIR,name);
    FILE *f=fopen(base,"rb"); if(!f){send_response(fd,404,"Dashboard no encontrado",23,"text/plain");return;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); char *b=malloc(sz>0?(size_t)sz:1);
    if(!b||sz<0||fread(b,1,(size_t)sz,f)!=(size_t)sz){fclose(f);free(b);send_response(fd,500,"Error al leer el dashboard",26,"text/plain");return;}
    fclose(f); send_response(fd,200,b,(size_t)sz,ct); free(b);
}
static void handle_admin_post(int fd,const struct request *r,const struct who *w) {
    char err[512]=""; const char *p=r->path;
    if(!strcmp(p,"/api/blacklist")){
        struct json_object *o=json_tokener_parse(r->body);
        if(validate_blacklist(o,err,sizeof err)<0){if(o)json_object_put(o);send_error(fd,400,err[0]?err:"JSON inválido");return;}
        char tmp[PATH_MAX]; snprintf(tmp,sizeof tmp,"%s.tmp",BLACKLIST_PATH);
        if(json_object_to_file_ext(tmp,o,JSON_C_TO_STRING_PRETTY)<0||rename(tmp,BLACKLIST_PATH)<0){json_object_put(o);send_error(fd,500,"No se pudo guardar blacklist.json");return;}
        int n=json_object_object_length(o);
        pthread_mutex_lock(&bl_lock); if(blacklist)json_object_put(blacklist); blacklist=o; pthread_mutex_unlock(&bl_lock);
        add_alert("sistema","MEDIUM","Actualización base de datos local","%s cargó %d firmas en la blacklist",w->username,n);
        reevaluate_files(); send_ok(fd); return;
    }
    struct json_object *j=json_tokener_parse(r->body),*v;
    if(!strcmp(p,"/api/control")){
        const char *action=body_str(j,"action"); int rc=0;
        if(action&&!strcmp(action,"metrics_on")){if(!metrics_enabled){metrics_enabled=1;add_alert("sistema","LOW","Monitoreo del sistema activado","Activado por %s",w->username);}}
        else if(action&&!strcmp(action,"metrics_off")){if(metrics_enabled){metrics_enabled=0;add_alert("sistema","MEDIUM","Monitoreo del sistema desactivado","Desactivado por %s",w->username);}}
        else if(action&&!strcmp(action,"procs_on"))procscan_enabled=1;
        else if(action&&!strcmp(action,"procs_off"))procscan_enabled=0;
        else if(action&&!strcmp(action,"files_on"))rc=set_file_scan(true,err,sizeof err);
        else if(action&&!strcmp(action,"files_off"))rc=set_file_scan(false,err,sizeof err);
        else if(action&&!strcmp(action,"reload_blacklist")){load_blacklist();add_alert("sistema","MEDIUM","Actualización base de datos local","Blacklist recargada desde disco por %s",w->username);reevaluate_files();}
        else{rc=-1;snprintf(err,sizeof err,"Acción desconocida");}
        if(rc<0)send_error(fd,400,err);else send_ok(fd);
    }else if(!strcmp(p,"/api/path")){
        if(set_monitor_path(body_str(j,"ruta"),err,sizeof err)<0)send_error(fd,400,err);else send_ok(fd);
    }else if(!strcmp(p,"/api/kill")){
        if(!j||!json_object_object_get_ex(j,"pid",&v))send_error(fd,400,"Se requiere 'pid'");
        else if(kill_process(json_object_get_int(v),w->username,err,sizeof err)<0)send_error(fd,400,err);else send_ok(fd);
    }else if(!strcmp(p,"/api/quarantine")){
        const char *ruta=body_str(j,"ruta");
        if(!ruta)send_error(fd,400,"Se requiere 'ruta'");else if(quarantine_file(ruta,w->username,err,sizeof err)<0)send_error(fd,400,err);else send_ok(fd);
    }else if(!strcmp(p,"/api/logwatch")){
        const char *central=body_str(j,"central"),*palabra=body_str(j,"palabra"); struct json_object *rutas=NULL;
        if(!central||central[0]!='/'||!palabra||!palabra[0]||strlen(palabra)>=sizeof logwatches[0].keyword||!json_object_object_get_ex(j,"rutas",&rutas)||!json_object_is_type(rutas,json_type_array)||json_object_array_length(rutas)==0){send_error(fd,400,"Se requieren 'central' (ruta absoluta), 'palabra' y 'rutas' (lista)");if(j)json_object_put(j);return;}
        struct json_object *ids=json_object_new_array(); size_t ok=0,nr=json_object_array_length(rutas);
        for(size_t i=0;i<nr;i++){const char *ruta=json_object_get_string(json_object_array_get_idx(rutas,i));
            if(!ruta||ruta[0]!='/'){snprintf(err,sizeof err,"Ruta inválida: %s",ruta?ruta:"(vacía)");continue;}
            int id=start_logwatch(ruta,central,palabra,err,sizeof err); if(id>=0){json_object_array_add(ids,json_object_new_int(id));ok++;}}
        if(!ok){json_object_put(ids);send_error(fd,400,err);}
        else{struct json_object *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(1));json_object_object_add(o,"ids",ids);if(ok<nr)add_str(o,"aviso",err);send_json(fd,200,o);}
    }else if(!strcmp(p,"/api/logwatch/stop")){
        if(!j||!json_object_object_get_ex(j,"id",&v))send_error(fd,400,"Se requiere 'id'");
        else if(stop_logwatch(json_object_get_int(v),err,sizeof err)<0)send_error(fd,400,err);else send_ok(fd);
    }else send_error(fd,404,"Ruta no encontrada");
    if(j)json_object_put(j);
}
static void handle_client(int fd) {
    struct request r={0}; int rc=read_request(fd,&r);
    if(rc==-2){send_error(fd,413,"Petición demasiado grande");return;}
    if(rc<0)return;
    struct who w={0}; bool is_get=!strcmp(r.method,"GET"),is_post=!strcmp(r.method,"POST");
    if(!strcmp(r.method,"OPTIONS")){send_response(fd,204,"",0,"application/json");goto out;}
    if(is_post&&!strcmp(r.path,"/api/login")){
        struct json_object *j=json_tokener_parse(r.body); const char *user=body_str(j,"username"),*pass=body_str(j,"password"); char role[32]={0};
        if(user&&pass&&strlen(user)<64&&authenticate_pam(user,pass,role,sizeof role)==0){
            char token[65];
            if(create_session(user,role,token)<0)send_error(fd,503,"Sesiones llenas");
            else{struct json_object *o=json_object_new_object();json_object_object_add(o,"ok",json_object_new_boolean(1));add_str(o,"username",user);add_str(o,"role",role);add_str(o,"token",token);
                add_alert("sesion","LOW","Inicio de sesión exitoso","Usuario %s (%s)",user,role);send_json(fd,200,o);}
        }else{add_alert("sesion","MEDIUM","Inicio de sesión fallido","Usuario %s: credenciales inválidas o sin grupo admin_user/common_user",user?user:"(vacío)");send_error(fd,401,"Credenciales o grupo inválidos");}
        if(j)json_object_put(j); goto out;
    }
    if(strncmp(r.path,"/api/",5)){if(is_get)serve_static(fd,r.path);else send_error(fd,404,"Ruta no encontrada");goto out;}
    if(session_for_request(&r,&w)<0){send_error(fd,401,"Autenticación requerida");goto out;}
    bool admin=!strcmp(w.role,"admin_user");
    if(is_post&&!strcmp(r.path,"/api/logout")){drop_session(&r);send_ok(fd);}
    else if(is_get&&!strcmp(r.path,"/api/status"))send_json(fd,200,status_json(&w));
    else if(!admin)send_error(fd,403,"Acción permitida solo para admin_user");
    else if(is_get&&!strcmp(r.path,"/api/quarantine")){
        struct json_object *a=json_object_new_array(); pthread_mutex_lock(&data_lock);
        for(size_t i=0;i<file_count;i++)if(!strcmp(files[i].status,"cuarentena"))json_object_array_add(a,file_json(&files[i],true));
        pthread_mutex_unlock(&data_lock); send_json(fd,200,a);
    }else if(is_get&&!strcmp(r.path,"/api/process")){
        int pid=0; const char *q=strstr(r.query,"pid="); if(q)pid=atoi(q+4);
        struct process_info_user info={0};
        if(pid<=0)send_error(fd,400,"Se requiere ?pid=<número>");
        else if(syscall(SYS_GET_PROCESS_INFO,pid,&info)<0){char e[128];snprintf(e,sizeof e,"sys_get_process_info(%d): %s",pid,strerror(errno));send_error(fd,404,e);}
        else{struct json_object *o=json_object_new_object();json_object_object_add(o,"pid",json_object_new_int(info.pid));add_str(o,"name",info.name);add_u64(o,"runtime_ms",info.runtime_ms);add_u64(o,"memory_kb",info.memory_kb);
            json_object_object_add(o,"mem_percent",json_object_new_double(mem_total_kb?100.0*(double)info.memory_kb/(double)mem_total_kb:0));send_json(fd,200,o);}
    }else if(is_post)handle_admin_post(fd,&r,&w);
    else send_error(fd,404,"Ruta no encontrada");
out:
    free(r.raw);
}
static void *conn_thread(void *arg) {
    int c=(int)(intptr_t)arg; struct timeval tv={10,0}; setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    handle_client(c); close(c); return NULL;
}
static void *http_thread(void *unused) {
    (void)unused;int s=socket(AF_INET,SOCK_STREAM,0);if(s<0){log_msg("socket: %s",strerror(errno));return NULL;}int one=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(HTTP_PORT),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if(bind(s,(struct sockaddr*)&a,sizeof a)<0||listen(s,32)<0){log_msg("bind/listen: %s",strerror(errno));close(s);return NULL;}fcntl(s,F_SETFL,fcntl(s,F_GETFL)|O_NONBLOCK);log_msg("HTTP escuchando en http://127.0.0.1:%d",HTTP_PORT);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
    /* Un hilo por conexión: un login PAM lento no bloquea al resto del dashboard. */
    while(running){int c=accept(s,NULL,NULL);if(c<0){if(errno==EAGAIN||errno==EWOULDBLOCK){usleep(50000);continue;}if(errno==EINTR)continue;break;}
        pthread_t t; if(pthread_create(&t,&at,conn_thread,(void*)(intptr_t)c)!=0){handle_client(c);close(c);}}
    pthread_attr_destroy(&at); close(s); return NULL;
}
int main(int argc,char **argv) {
    bool foreground=(argc>1&&!strcmp(argv[1],"--foreground"));
    /* Leer rutas relativas antes de chdir("/") durante la daemonización. */
    load_config();load_blacklist();
    if(!foreground&&daemonize_process()<0)return EXIT_FAILURE;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGINT,  &sa, NULL) == -1 ||
        sigaction(SIGHUP,  &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE,SIG_IGN);
    int pidfd=write_pidfile();if(pidfd<0){log_msg("Ya existe una instancia o no se pudo crear pidfile");return EXIT_FAILURE;}
    mkdir(monitor_path,0750);log_msg("Daemon iniciado PID=%ld",(long)getpid());
    { struct metrics m={0}; read_meminfo(&m); mem_total_kb=m.mem_total; }
    add_alert("sistema","LOW","Monitoreo del sistema activado","Daemon iniciado (PID %ld)",(long)getpid());
    char err[512];
    if(start_logwatch(logwatch_path,central_log,log_keyword,err,sizeof err)<0)log_msg("No se pudo iniciar log_watch: %s",err);
    if(sensor_init()==0){char rp[PATH_MAX];if(realpath(monitor_path,rp))snprintf(monitor_path,sizeof monitor_path,"%s",rp);
        if(set_monitor_path(monitor_path,err,sizeof err)<0)log_msg("Ruta inicial rechazada: %s",err);
        if(file_scan_on_start&&set_file_scan(true,err,sizeof err)<0)log_msg("%s",err);}
    pthread_t tmon,tipc,tfile,thttp;pthread_create(&tmon,NULL,monitor_thread,NULL);pthread_create(&tipc,NULL,ipc_thread,NULL);pthread_create(&tfile,NULL,file_thread,NULL);pthread_create(&thttp,NULL,http_thread,NULL);
    while(running)sleep(1);
    /* The blocking IPC syscall must be interrupted by a signal or stopped by the existing syscall API. */
    pthread_kill(tipc,SIGTERM);pthread_join(tmon,NULL);pthread_join(tfile,NULL);pthread_join(tipc,NULL);pthread_join(thttp,NULL);
    pthread_mutex_lock(&data_lock); int ids[MAX_LOGWATCH]; size_t nl=logwatch_count; for(size_t i=0;i<nl;i++)ids[i]=logwatches[i].id; pthread_mutex_unlock(&data_lock);
    for(size_t i=0;i<nl;i++)stop_logwatch(ids[i],err,sizeof err);
    if(rb)ring_buffer__free(rb); if(skel)file_monitor_bpf__destroy(skel);
    log_msg("Daemon detenido limpiamente");if(blacklist)json_object_put(blacklist);close(pidfd);unlink("/tmp/proyecto_unico.pid");return 0;
}
