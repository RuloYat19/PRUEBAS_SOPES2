CLANG ?= clang
BPFTOOL ?= bpftool
LLVM_STRIP ?= llvm-strip

all: sensor.skel.h

KERNEL_BUILD ?= $(HOME)/Escritorio/SistemasOperativos2/PRUEBAS_SOPES2/202100106/Practica_3_2S2026/so2_build_workspace/linux-6.12.69

vmlinux.h:
	@if [ -r /sys/kernel/btf/vmlinux ]; then \
		$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@.tmp; \
	elif [ -r "$(KERNEL_BUILD)/vmlinux" ]; then \
		$(BPFTOOL) btf dump file "$(KERNEL_BUILD)/vmlinux" format c > $@.tmp; \
	else \
		echo "ERROR: no se encontró BTF en el kernel activo ni en $(KERNEL_BUILD)/vmlinux"; \
		exit 1; \
	fi
	@mv $@.tmp $@
	
sensor.bpf.o: sensor.bpf.c sensor.h filtro_ruta.h vmlinux.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_x86 -I. -c $< -o $@
	$(LLVM_STRIP) -g $@

sensor.skel.h: sensor.bpf.o
	$(BPFTOOL) gen skeleton $< name sensor_bpf > $@.tmp && mv $@.tmp $@

clean:
	rm -f vmlinux.h sensor.bpf.o sensor.skel.h *.tmp

.PHONY: all clean
