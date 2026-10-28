// Copiar al árbol kernel/kernel/ y registrar las syscalls en syscall_64.tbl.
// Las estructuras deben mantenerse idénticas a las definidas en daemon.c.
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/timekeeping.h>
#include <linux/sched/cputime.h>
#include <linux/slab.h>

#define PROCESS_NAME_LEN 64
#define PROCESS_LIST_MAX 4096
struct process_info_user {
    char name[PROCESS_NAME_LEN];
    pid_t pid;
    unsigned long long runtime_ms;
    unsigned long long memory_kb;
};
struct processes_state_user {
    unsigned long total, running, sleeping, zombie;
};
/* Una entrada por proceso: PID, nombre, estado (R/S/D/Z/...) y memoria RSS. */
struct process_entry_user {
    pid_t pid;
    char state;
    char name[PROCESS_NAME_LEN];
    unsigned long long rss_kb;
};

SYSCALL_DEFINE2(get_process_info, pid_t, pid, struct process_info_user __user *, out)
{
    struct task_struct *task;
    struct process_info_user info = {0};
    struct mm_struct *mm;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) { rcu_read_unlock(); return -ESRCH; }
    get_task_struct(task);
    rcu_read_unlock();
    strscpy(info.name, task->comm, sizeof(info.name));
    info.pid = task_pid_nr(task);
    info.runtime_ms = div_u64(READ_ONCE(task->se.sum_exec_runtime), NSEC_PER_MSEC);
    mm = get_task_mm(task);
    if (mm) { info.memory_kb = (unsigned long long)get_mm_rss(mm) * (PAGE_SIZE / 1024); mmput(mm); }
    put_task_struct(task);
    if (copy_to_user(out, &info, sizeof(info))) return -EFAULT;
    return 0;
}

/*
 * out:     contadores globales (obligatorio).
 * entries: arreglo opcional donde se copia cada proceso; NULL = solo contadores.
 * max:     capacidad del arreglo (se limita a PROCESS_LIST_MAX).
 * Devuelve el numero de entradas escritas en entries.
 */
SYSCALL_DEFINE3(get_processes_state, struct processes_state_user __user *, out,
                struct process_entry_user __user *, entries, unsigned int, max)
{
    struct task_struct *task;
    struct processes_state_user state = {0};
    struct process_entry_user *buf = NULL;
    unsigned int n = 0;
    long ret;

    if (entries && max) {
        if (max > PROCESS_LIST_MAX) max = PROCESS_LIST_MAX;
        buf = kvcalloc(max, sizeof(*buf), GFP_KERNEL);
        if (!buf) return -ENOMEM;
    }
    rcu_read_lock();
    for_each_process(task) {
        unsigned int st = READ_ONCE(task->__state);
        state.total++;
        if (READ_ONCE(task->exit_state) & EXIT_ZOMBIE) state.zombie++;
        else if (st == TASK_RUNNING) state.running++;
        else if (st & (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)) state.sleeping++;
        if (buf && n < max) {
            struct process_entry_user *e = &buf[n++];
            struct mm_struct *mm;
            e->pid = task_pid_nr(task);
            e->state = task_state_to_char(task);
            strscpy(e->name, task->comm, sizeof(e->name));
            /* task_lock evita que exit_mm libere el mm mientras se lee (no duerme). */
            task_lock(task);
            mm = task->mm;
            if (mm) e->rss_kb = (unsigned long long)get_mm_rss(mm) * (PAGE_SIZE / 1024);
            task_unlock(task);
        }
    }
    rcu_read_unlock();
    if (copy_to_user(out, &state, sizeof(state))) ret = -EFAULT;
    else if (buf && n && copy_to_user(entries, buf, n * sizeof(*buf))) ret = -EFAULT;
    else ret = n;
    kvfree(buf);
    return ret;
}
