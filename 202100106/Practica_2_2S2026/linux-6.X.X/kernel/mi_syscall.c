#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/rcupdate.h>
#include <linux/pid.h>

/*
 * ==============================================================================
 * Universidad de San Carlos de Guatemala - Facultad de Ingenieria
 * Sistemas Operativos 2 - 2S2026
 * Practica 2: Syscalls Personalizadas de Metricas del Sistema
 * Estudiante Carne: 202100106
 * ==============================================================================
 */

#define TASK_COMM_LEN 16

/* Estructura para metricas de un proceso individual */
struct process_info {
	char name[TASK_COMM_LEN];
	pid_t pid;
	unsigned long long runtime_ns;
	unsigned long memory_kb;
};

/* Estructura para el estado global de procesos del sistema */
struct processes_state {
	unsigned int total;
	unsigned int running;
	unsigned int sleeping;
	unsigned int zombie;
};

/*
 * Syscall 1: get_process_info(pid, &struct)
 * Recolecta nombre, PID, tiempo de CPU y memoria de un proceso dado.
 */
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
		return -ESRCH; /* Proceso no encontrado */
	}

	get_task_struct(task);
	rcu_read_unlock();

	/* Extraer nombre del ejecutable y PID */
	get_task_comm(kinfo.name, task);
	kinfo.pid = task->pid;

	/* Extraer tiempo total de CPU acumulado en nanosegundos */
	kinfo.runtime_ns = task->se.sum_exec_runtime;

	/*
	 * Extraer uso aproximado de memoria:
	 * IMPORTANTE: Los hilos de kernel (kthreads) no poseen espacio de
	 * memoria de usuario (task->mm == NULL). Se debe validar para evitar Kernel Panic.
	 */
	if (task->mm) {
		/* total_vm es el conteo de paginas virtuales mapeadas */
		kinfo.memory_kb = (task->mm->total_vm << (PAGE_SHIFT - 10));
	} else {
		kinfo.memory_kb = 0;
	}

	put_task_struct(task);

	/* Transferencia segura de memoria hacia espacio de usuario */
	if (copy_to_user(info, &kinfo, sizeof(kinfo)))
		return -EFAULT;

	pr_info("get_process_info: Metricas enviadas para PID %d (%s)\n", kinfo.pid, kinfo.name);
	return 0;
}

/*
 * Syscall 2: get_processes_state(&struct)
 * Itera sobre todos los procesos activos y clasifica sus estados de ejecucion.
 */
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

		/* Clasificacion del estado del proceso */
		if (task->exit_state == EXIT_ZOMBIE || task->exit_state == EXIT_DEAD) {
			kstate.zombie++;
		} else if (task_is_running(task) || task->__state == TASK_RUNNING) {
			kstate.running++;
		} else {
			/* TASK_INTERRUPTIBLE, TASK_UNINTERRUPTIBLE, TASK_IDLE */
			kstate.sleeping++;
		}
	}
	rcu_read_unlock();

	/* Transferencia segura de memoria hacia espacio de usuario */
	if (copy_to_user(state, &kstate, sizeof(kstate)))
		return -EFAULT;

	pr_info("get_processes_state: Total=%u, Running=%u, Sleeping=%u, Zombie=%u\n",
	        kstate.total, kstate.running, kstate.sleeping, kstate.zombie);
	return 0;
}
