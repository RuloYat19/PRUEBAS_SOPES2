#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/rcupdate.h>
#include <linux/pid.h>

#define TASK_COMM_LEN 16

struct process_info {
	char name[TASK_COMM_LEN];
	pid_t pid;
	unsigned long long runtime_ns;
	unsigned long memory_kb;
};

struct processes_state {
	unsigned int total;
	unsigned int running;
	unsigned int sleeping;
	unsigned int zombie;
};

SYSCALL_DEFINE2(get_process_info, pid_t, pid, struct process_info __user *, info)
{
	struct task_struct *task;
	struct process_info kinfo;

	if (!info)
		return -EINVAL;

	memset(&kinfo, 0, sizeof(kinfo));

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}

	get_task_struct(task);
	rcu_read_unlock();

	get_task_comm(kinfo.name, task);
	kinfo.pid = task->pid;

	kinfo.runtime_ns = task->se.sum_exec_runtime;

	if (task->mm)
		kinfo.memory_kb = (task->mm->total_vm << (PAGE_SHIFT - 10));
	else
		kinfo.memory_kb = 0;

	put_task_struct(task);

	if (copy_to_user(info, &kinfo, sizeof(kinfo)))
		return -EFAULT;

	pr_info("get_process_info: Metricas enviadas para PID %d (%s)\n",
		kinfo.pid, kinfo.name);
	return 0;
}

SYSCALL_DEFINE1(get_processes_state, struct processes_state __user *, state)
{
	struct task_struct *task;
	struct processes_state kstate;

	if (!state)
		return -EINVAL;

	memset(&kstate, 0, sizeof(kstate));

	rcu_read_lock();
	for_each_process(task) {
		kstate.total++;

		if (task->exit_state == EXIT_ZOMBIE ||
		    task->exit_state == EXIT_DEAD)
			kstate.zombie++;
		else if (task_is_running(task) || task->__state == TASK_RUNNING)
			kstate.running++;
		else
			kstate.sleeping++;
	}
	rcu_read_unlock();

	if (copy_to_user(state, &kstate, sizeof(kstate)))
		return -EFAULT;

	pr_info("get_processes_state: Total=%u, Running=%u, Sleeping=%u, Zombie=%u\n",
		kstate.total, kstate.running, kstate.sleeping, kstate.zombie);
	return 0;
}
