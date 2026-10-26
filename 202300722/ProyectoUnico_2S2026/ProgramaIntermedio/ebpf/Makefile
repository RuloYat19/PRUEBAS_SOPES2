CLANG ?= clang
BPFTOOL ?= bpftool
ARCH ?= x86
CFLAGS ?= -O2 -g

all: file_monitor.skel.h

vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@

file_monitor.bpf.o: file_monitor.bpf.c file_monitor.h vmlinux.h
	$(CLANG) -target bpf -D__TARGET_ARCH_$(ARCH) $(CFLAGS) -I. -c file_monitor.bpf.c -o $@

file_monitor.skel.h: file_monitor.bpf.o
	$(BPFTOOL) gen skeleton file_monitor.bpf.o > $@

clean:
	rm -f vmlinux.h file_monitor.bpf.o file_monitor.skel.h
.PHONY: all clean
