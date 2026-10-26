CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread
CPPFLAGS += $(shell pkg-config --cflags libbpf json-c openssl) -DCONFIG_PATH=\"$(CURDIR)/config.json\" -DBLACKLIST_PATH=\"$(CURDIR)/blacklist.json\" -DDASHBOARD_DIR=\"$(CURDIR)/../Dashboard\"
LDLIBS += $(shell pkg-config --libs libbpf json-c openssl) -lpam -pthread

all: daemon

ebpf:
	$(MAKE) -C ebpf

daemon: daemon.c ebpf/file_monitor.skel.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ daemon.c $(LDLIBS)

ebpf/file_monitor.skel.h:
	$(MAKE) -C ebpf

clean:
	rm -f daemon ebpf/file_monitor.bpf.o ebpf/file_monitor.skel.h
.PHONY: all clean ebpf
