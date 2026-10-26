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

#define PROCESS_NAME_LEN 64
struct process_info_user {
    char name[PROCESS_NAME_LEN];
    pid_t pid;
    unsigned long long runtime_ms;
    unsigned long long memory_kb;
};
struct processes_state_user {
    unsigned long total, running, sleeping, zombie;
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

SYSCALL_DEFINE1(get_processes_state, struct processes_state_user __user *, out)
{
    struct task_struct *task;
    struct processes_state_user state = {0};
    rcu_read_lock();
    for_each_process(task) {
        state.total++;
        if (READ_ONCE(task->__state) == TASK_RUNNING) state.running++;
        else if (READ_ONCE(task->__state) & (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE | TASK_KILLABLE)) state.sleeping++;
        else if (READ_ONCE(task->exit_state) & EXIT_ZOMBIE) state.zombie++;
    }
    rcu_read_unlock();
    if (copy_to_user(out, &state, sizeof(state))) return -EFAULT;
    return 0;
}
