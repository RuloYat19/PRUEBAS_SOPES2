// Versión educativa de log_watch. Verifica la API de archivos del kernel 6.18.48
// y compara este archivo con el log_watch.c que ya compilaste en Práctica 3.
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/timekeeping.h>
#include "ipc_channel.h"

#define LW_MAX 8
#define LW_PATH 256
#define LW_KEY 128
#define LW_LINE 512
struct lw_monitor { int id; char path[LW_PATH], central[LW_PATH], keyword[LW_KEY]; loff_t offset; struct task_struct *thread; bool active; };
static struct lw_monitor *monitors[LW_MAX];
static DEFINE_MUTEX(lw_lock);
static int next_id = 1;

static void lw_write_central(const char *path, const char *line)
{
    struct file *f = filp_open(path, O_WRONLY|O_CREAT|O_APPEND, 0640);
    if (!IS_ERR(f)) { loff_t pos = 0; kernel_write(f, line, strlen(line), &pos); kernel_write(f, "\n", 1, &pos); filp_close(f, NULL); }
}
static int lw_thread(void *arg)
{
    struct lw_monitor *m = arg; char *buf = kmalloc(PAGE_SIZE, GFP_KERNEL); if (!buf) return -ENOMEM;
    while (!kthread_should_stop() && READ_ONCE(m->active)) {
        struct file *f = filp_open(m->path, O_RDONLY, 0);
        if (!IS_ERR(f)) {
            loff_t pos = m->offset; ssize_t n = kernel_read(f, buf, PAGE_SIZE-1, &pos);
            if (n > 0) {
                buf[n] = '\0'; m->offset = pos;
                char *line = buf, *cur = buf;
                while (cur < buf+n) {
                    char *nl = memchr(cur, '\n', (buf+n)-cur); if (nl) *nl='\0';
                    if (strnstr(cur, m->keyword, strlen(cur))) {
                        char out[LW_LINE+LW_PATH+64];
                        snprintf(out, sizeof(out), "[archivo=%s] [palabra_clave=%s] %.*s", m->path, m->keyword, LW_LINE, cur);
                        lw_write_central(m->central, out);
                        { char ipc[LW_LINE+LW_PATH+128]; snprintf(ipc, sizeof(ipc), "archivo_origen=%s|palabra_detectada=%s|contenido=%.*s", m->path, m->keyword, LW_LINE, cur); ipc_channel_enqueue_event(ipc); }
                    }
                    if (!nl) break; cur=nl+1; line=cur; (void)line;
                }
            }
            filp_close(f, NULL);
        }
        msleep(3000);
    }
    kfree(buf); return 0;
}
SYSCALL_DEFINE3(start_log_watch, const char __user *, path, const char __user *, central, const char __user *, keyword)
{
    struct lw_monitor *m; int i, id;
    m = kzalloc(sizeof(*m), GFP_KERNEL); if (!m) return -ENOMEM;
    if (strncpy_from_user(m->path,path,LW_PATH)<0 || strncpy_from_user(m->central,central,LW_PATH)<0 || strncpy_from_user(m->keyword,keyword,LW_KEY)<0) { kfree(m); return -EFAULT; }
    if (!m->path[0] || !m->central[0] || !m->keyword[0]) { kfree(m); return -EINVAL; }
    mutex_lock(&lw_lock); for(i=0;i<LW_MAX && monitors[i];i++); if(i==LW_MAX){mutex_unlock(&lw_lock);kfree(m);return -ENOSPC;}
    id=next_id++; m->id=id;m->active=true;monitors[i]=m;mutex_unlock(&lw_lock);
    m->thread=kthread_run(lw_thread,m,"log_watch/%d",id);if(IS_ERR(m->thread)){mutex_lock(&lw_lock);monitors[i]=NULL;mutex_unlock(&lw_lock);i=PTR_ERR(m->thread);kfree(m);return i;}
    return id;
}
SYSCALL_DEFINE1(stop_log_watch, int, id)
{
    int i; struct lw_monitor *m=NULL;
    mutex_lock(&lw_lock);for(i=0;i<LW_MAX;i++)if(monitors[i]&&monitors[i]->id==id){m=monitors[i];monitors[i]=NULL;break;}mutex_unlock(&lw_lock);
    if(!m)return -ENOENT;WRITE_ONCE(m->active,false);if(m->thread)kthread_stop(m->thread);kfree(m);return 0;
}
