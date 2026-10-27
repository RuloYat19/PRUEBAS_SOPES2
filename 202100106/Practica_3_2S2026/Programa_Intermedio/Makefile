CC = gcc
CFLAGS = -Wall -Wextra -O2

PROGRAMAS = test_ipc_channel_send test_ipc_channel_receive \
            test_start_log_watch test_stop_log_watch

all: $(PROGRAMAS)

test_ipc_channel_send: test_ipc_channel_send.c
	$(CC) $(CFLAGS) -o $@ $<

test_ipc_channel_receive: test_ipc_channel_receive.c
	$(CC) $(CFLAGS) -o $@ $<

test_start_log_watch: test_start_log_watch.c
	$(CC) $(CFLAGS) -o $@ $<

test_stop_log_watch: test_stop_log_watch.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGRAMAS)

.PHONY: all clean
