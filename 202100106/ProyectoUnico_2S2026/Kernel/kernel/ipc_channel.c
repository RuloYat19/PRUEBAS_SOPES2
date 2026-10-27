#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/ipc_channel.h>

#define IPC_QUEUE_MAX_MSGS	128

struct ipc_message {
	struct list_head list;
	char data[IPC_MSG_MAX_LEN];
};

static LIST_HEAD(ipc_queue);
static DEFINE_MUTEX(ipc_queue_lock);
static DECLARE_WAIT_QUEUE_HEAD(ipc_queue_wq);
static unsigned int ipc_queue_count;

SYSCALL_DEFINE1(ipc_channel_send, const char __user *, mensaje)
{
	struct ipc_message *nodo;
	long copiados;
	unsigned int encolados;

	if (!mensaje)
		return -EINVAL;

	nodo = kmalloc(sizeof(*nodo), GFP_KERNEL);
	if (!nodo)
		return -ENOMEM;

	copiados = strncpy_from_user(nodo->data, mensaje, IPC_MSG_MAX_LEN);
	if (copiados < 0) {
		kfree(nodo);
		return -EFAULT;
	}
	if (copiados >= IPC_MSG_MAX_LEN) {
		kfree(nodo);
		return -EMSGSIZE;
	}

	mutex_lock(&ipc_queue_lock);
	if (ipc_queue_count >= IPC_QUEUE_MAX_MSGS) {
		mutex_unlock(&ipc_queue_lock);
		kfree(nodo);
		return -EAGAIN;
	}
	list_add_tail(&nodo->list, &ipc_queue);
	ipc_queue_count++;
	encolados = ipc_queue_count;
	mutex_unlock(&ipc_queue_lock);

	wake_up_interruptible(&ipc_queue_wq);

	pr_info("ipc_channel_send: mensaje encolado por PID %d, mensajes en cola %u\n",
		current->pid, encolados);
	return 0;
}

int ipc_channel_enqueue_kernel(const char *mensaje)
{
	struct ipc_message *nodo;
	size_t largo;
	unsigned int encolados;

	if (!mensaje)
		return -EINVAL;

	largo = strnlen(mensaje, IPC_MSG_MAX_LEN);
	if (largo >= IPC_MSG_MAX_LEN)
		return -EMSGSIZE;

	nodo = kmalloc(sizeof(*nodo), GFP_KERNEL);
	if (!nodo)
		return -ENOMEM;

	strscpy(nodo->data, mensaje, IPC_MSG_MAX_LEN);

	mutex_lock(&ipc_queue_lock);
	if (ipc_queue_count >= IPC_QUEUE_MAX_MSGS) {
		mutex_unlock(&ipc_queue_lock);
		kfree(nodo);
		return -EAGAIN;
	}
	list_add_tail(&nodo->list, &ipc_queue);
	ipc_queue_count++;
	encolados = ipc_queue_count;
	mutex_unlock(&ipc_queue_lock);

	wake_up_interruptible(&ipc_queue_wq);

	pr_info("ipc_channel_enqueue_kernel: hilo de kernel encolo una notificacion, mensajes en cola %u\n",
		encolados);
	return 0;
}

SYSCALL_DEFINE1(ipc_channel_receive, char __user *, buffer)
{
	struct ipc_message *nodo = NULL;
	char local[IPC_MSG_MAX_LEN];
	size_t largo;
	int espera;

	if (!buffer)
		return -EINVAL;

	pr_info("ipc_channel_receive: PID %d bloqueado en espera de mensaje\n",
		current->pid);

	for (;;) {
		espera = wait_event_interruptible(ipc_queue_wq, ipc_queue_count > 0);
		if (espera)
			return -ERESTARTSYS;

		mutex_lock(&ipc_queue_lock);
		if (!list_empty(&ipc_queue)) {
			nodo = list_first_entry(&ipc_queue, struct ipc_message, list);
			list_del(&nodo->list);
			ipc_queue_count--;
			mutex_unlock(&ipc_queue_lock);
			break;
		}
		mutex_unlock(&ipc_queue_lock);
	}

	largo = strnlen(nodo->data, IPC_MSG_MAX_LEN - 1) + 1;
	memcpy(local, nodo->data, largo);
	kfree(nodo);

	if (copy_to_user(buffer, local, largo))
		return -EFAULT;

	pr_info("ipc_channel_receive: PID %d recibio un mensaje de %zu bytes\n",
		current->pid, largo);
	return 0;
}
