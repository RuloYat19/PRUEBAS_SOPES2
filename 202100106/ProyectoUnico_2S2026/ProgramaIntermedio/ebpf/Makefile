CLANG ?= clang
BPFTOOL ?= bpftool
LLVM_STRIP ?= llvm-strip

all: sensor.skel.h

vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@.tmp && mv $@.tmp $@

sensor.bpf.o: sensor.bpf.c sensor.h filtro_ruta.h vmlinux.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_x86 -I. -c $< -o $@
	$(LLVM_STRIP) -g $@

sensor.skel.h: sensor.bpf.o
	$(BPFTOOL) gen skeleton $< name sensor_bpf > $@.tmp && mv $@.tmp $@

clean:
	rm -f vmlinux.h sensor.bpf.o sensor.skel.h *.tmp

.PHONY: all clean
