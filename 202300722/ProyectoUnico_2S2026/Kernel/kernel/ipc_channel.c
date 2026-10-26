// Canal IPC simple para la práctica: cola de mensajes entre kernel y daemon.
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <linux/string.h>

#define IPC_MAX_MESSAGE 512
struct ipc_message { struct list_head list; char message[IPC_MAX_MESSAGE]; };
static LIST_HEAD(ipc_queue);
static DEFINE_MUTEX(ipc_mutex);
static DECLARE_WAIT_QUEUE_HEAD(ipc_wait_queue);

int ipc_channel_enqueue_event(const char *message)
{
    struct ipc_message *msg;
    if (!message) return -EINVAL;
    msg = kmalloc(sizeof(*msg), GFP_KERNEL);
    if (!msg) return -ENOMEM;
    strscpy(msg->message, message, sizeof(msg->message));
    mutex_lock(&ipc_mutex); list_add_tail(&msg->list, &ipc_queue); mutex_unlock(&ipc_mutex);
    wake_up_interruptible(&ipc_wait_queue);
    return 0;
}
EXPORT_SYMBOL_GPL(ipc_channel_enqueue_event);

SYSCALL_DEFINE1(ipc_channel_send, const char __user *, user_message)
{
    struct ipc_message *msg;
    msg = kzalloc(sizeof(*msg), GFP_KERNEL); if (!msg) return -ENOMEM;
    if (strncpy_from_user(msg->message, user_message, IPC_MAX_MESSAGE) < 0) { kfree(msg); return -EFAULT; }
    msg->message[IPC_MAX_MESSAGE - 1] = '\0';
    mutex_lock(&ipc_mutex); list_add_tail(&msg->list, &ipc_queue); mutex_unlock(&ipc_mutex);
    wake_up_interruptible(&ipc_wait_queue);
    return 0;
}

SYSCALL_DEFINE1(ipc_channel_receive, char __user *, user_message)
{
    struct ipc_message *msg;
    int ret;
    ret = wait_event_interruptible(ipc_wait_queue, !list_empty(&ipc_queue));
    if (ret) return ret;
    mutex_lock(&ipc_mutex);
    if (list_empty(&ipc_queue)) { mutex_unlock(&ipc_mutex); return -EAGAIN; }
    msg = list_first_entry(&ipc_queue, struct ipc_message, list);
    list_del(&msg->list); mutex_unlock(&ipc_mutex);
    if (copy_to_user(user_message, msg->message, IPC_MAX_MESSAGE)) ret = -EFAULT;
    else ret = 0;
    kfree(msg); return ret;
}
