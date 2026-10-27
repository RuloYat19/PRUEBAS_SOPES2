#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "sensor.h"
#include "filtro_ruta.h"

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct config_sensor);
} config_sensor SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} pid_daemon SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct evento_archivo);
} auxiliar SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, __u64);
	__type(value, struct evento_archivo);
} pendientes SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 8192);
	__type(key, __u64);
	__type(value, struct evento_archivo);
} abiertos SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} eventos SEC(".maps");

static inline void registrar_apertura(const char *ruta_usuario, unsigned int flags)
{
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__u32 tgid = (__u32)(pid_tgid >> 32);
	__u32 cero = 0;

	__u32 *daemon_pid = bpf_map_lookup_elem(&pid_daemon, &cero);
	if (daemon_pid && *daemon_pid != 0 && tgid == *daemon_pid)
		return;

	struct config_sensor *cfg = bpf_map_lookup_elem(&config_sensor, &cero);
	if (!cfg || cfg->largo == 0 || cfg->largo >= SENSOR_RUTA_MAX)
		return;

	struct evento_archivo *aux = bpf_map_lookup_elem(&auxiliar, &cero);
	if (!aux)
		return;

	long n = bpf_probe_read_user_str(aux->ruta, SENSOR_RUTA_MAX, ruta_usuario);
	if (n <= 0 || n >= SENSOR_RUTA_MAX)
		return;

	if (!ruta_en_directorio(aux->ruta, cfg->ruta, cfg->largo))
		return;

	aux->pid = tgid;
	aux->flags = flags;
	bpf_get_current_comm(aux->proceso, sizeof(aux->proceso));

	bpf_map_update_elem(&pendientes, &pid_tgid, aux, BPF_ANY);
}

SEC("tracepoint/syscalls/sys_enter_openat")
int ent_openat(struct trace_event_raw_sys_enter *ctx)
{
	registrar_apertura((const char *)ctx->args[1], (unsigned int)ctx->args[2]);
	return 0;
}

SEC("tracepoint/syscalls/sys_enter_open")
int ent_open(struct trace_event_raw_sys_enter *ctx)
{
	registrar_apertura((const char *)ctx->args[0], (unsigned int)ctx->args[1]);
	return 0;
}

SEC("tracepoint/syscalls/sys_enter_creat")
int ent_creat(struct trace_event_raw_sys_enter *ctx)
{
	registrar_apertura((const char *)ctx->args[0], SENSOR_FLAGS_CREAT);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_openat")
int sal_openat(struct trace_event_raw_sys_exit *ctx)
{
	(void)ctx;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	bpf_map_delete_elem(&pendientes, &pid_tgid);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_open")
int sal_open(struct trace_event_raw_sys_exit *ctx)
{
	(void)ctx;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	bpf_map_delete_elem(&pendientes, &pid_tgid);
	return 0;
}

SEC("tracepoint/syscalls/sys_exit_creat")
int sal_creat(struct trace_event_raw_sys_exit *ctx)
{
	(void)ctx;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	bpf_map_delete_elem(&pendientes, &pid_tgid);
	return 0;
}

SEC("kprobe/fd_install")
int BPF_KPROBE(instalar_fd, unsigned int fd, struct file *archivo)
{
	(void)fd;
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	struct evento_archivo *ev = bpf_map_lookup_elem(&pendientes, &pid_tgid);
	if (!ev)
		return 0;

	__u64 clave_file = (__u64)archivo;
	bpf_map_update_elem(&abiertos, &clave_file, ev, BPF_ANY);
	bpf_map_delete_elem(&pendientes, &pid_tgid);
	return 0;
}

SEC("kprobe/security_file_release")
int BPF_KPROBE(liberar_archivo, struct file *archivo)
{
	__u64 clave_file = (__u64)archivo;
	struct evento_archivo *ev = bpf_map_lookup_elem(&abiertos, &clave_file);
	if (!ev)
		return 0;

	struct evento_archivo *salida = bpf_ringbuf_reserve(&eventos, sizeof(*salida), 0);
	if (salida) {
		__builtin_memcpy(salida, ev, sizeof(*salida));
		bpf_ringbuf_submit(salida, 0);
	}

	bpf_map_delete_elem(&abiertos, &clave_file);
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
