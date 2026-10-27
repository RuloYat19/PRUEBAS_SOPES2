#ifndef _LINUX_IPC_CHANNEL_H
#define _LINUX_IPC_CHANNEL_H

#define IPC_MSG_MAX_LEN		256

int ipc_channel_enqueue_kernel(const char *mensaje);

#endif
