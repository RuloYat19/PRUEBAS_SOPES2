// eBPF tracepoint: observa openat con flags de escritura/creación y filtra en kernel
// únicamente archivos directamente dentro del directorio configurado.
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "file_monitor.h"
char LICENSE[] SEC("license") = "GPL";

struct { __uint(type, BPF_MAP_TYPE_ARRAY); __uint(max_entries, 1); __type(key, __u32); __type(value, char[FILE_EVENT_PATH_LEN]); } configured_dir SEC(".maps");
struct { __uint(type, BPF_MAP_TYPE_ARRAY); __uint(max_entries, 1); __type(key, __u32); __type(value, __u32); } daemon_pid SEC(".maps");
struct { __uint(type, BPF_MAP_TYPE_RINGBUF); __uint(max_entries, 1 << 20); } events SEC(".maps");

SEC("tracepoint/syscalls/sys_enter_openat")
int handle_openat(struct trace_event_raw_sys_enter *ctx) {
    __u32 zero=0; __u32 pid=(__u32)(bpf_get_current_pid_tgid() >> 32);
    __u32 *own=bpf_map_lookup_elem(&daemon_pid,&zero); if(own && *own==pid)return 0;
    char *dir=bpf_map_lookup_elem(&configured_dir,&zero); if(!dir || dir[0]=='\0')return 0;
    const char *user_path=(const char *)ctx->args[1]; long flags=(long)ctx->args[2];
    if(!(flags & 1 /* O_WRONLY */) && !(flags & 2 /* O_RDWR */) && !(flags & 0100 /* O_CREAT */) && !(flags & 01000 /* O_TRUNC */))return 0;
    char path[FILE_EVENT_PATH_LEN]={0}; if(bpf_probe_read_user_str(path,sizeof(path),user_path)<0)return 0;
    // sys_enter_openat expone normalmente el pathname como relativo al proceso. Solo se
    // acepta ruta absoluta, para evitar una coincidencia falsa con dirfd/relativas.
    if(path[0]!='/')return 0;
    int dlen=0; while(dlen<FILE_EVENT_PATH_LEN && dir[dlen])dlen++; if(dlen==0 || dlen>=FILE_EVENT_PATH_LEN)return 0;
    for(int i=0;i<FILE_EVENT_PATH_LEN;i++){if(i>=dlen)break;if(path[i]!=dir[i])return 0;}
    if(path[dlen]!='/')return 0;
    // No incluir subdirectorios: tras el prefijo debe haber un único nombre.
    for(int i=dlen+1;i<FILE_EVENT_PATH_LEN;i++){if(path[i]=='/')return 0;if(path[i]=='\0')break;}
    struct file_event *e=bpf_ringbuf_reserve(&events,sizeof(*e),0); if(!e)return 0;
    e->pid=pid;e->action=(flags & 0100)?FILE_ACTION_OPEN_CREATE:FILE_ACTION_OPEN_WRITE;
    __builtin_memcpy(e->path,path,sizeof(e->path)); bpf_ringbuf_submit(e,0);return 0;
}
