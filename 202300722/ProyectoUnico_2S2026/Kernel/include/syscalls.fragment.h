/* Fragmento orientativo para include/linux/syscalls.h.
 * Si las estructuras son necesarias en las firmas, muévelas a un header compartido
 * (por ejemplo include/uapi/linux/process_monitor.h) y úsalo desde kernel y usuario.
 * No pegues prototipos que contradigan los que ya existen en tus prácticas.
 */
asmlinkage long sys_get_process_info(pid_t pid, struct process_info_user __user *out);
asmlinkage long sys_get_processes_state(struct processes_state_user __user *out);
asmlinkage long sys_ipc_channel_send(const char __user *user_message);
asmlinkage long sys_ipc_channel_receive(char __user *user_message);
asmlinkage long sys_start_log_watch(const char __user *path, const char __user *central, const char __user *keyword);
asmlinkage long sys_stop_log_watch(int id);
