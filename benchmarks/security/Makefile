APP = security

all: $(APP)

$(APP): $(APP).bpf.c $(APP).c
	# 1. Compiler le code eBPF (Noyau)
	clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -I. -c $(APP).bpf.c -o $(APP).bpf.o
	
	# 2. Générer le squelette (Header C pour faire le pont avec le userspace)
	/usr/sbin/bpftool gen skeleton $(APP).bpf.o > $(APP).skel.h
	
	# 3. Compiler le programme final (Userspace)
	gcc -g -O2 -Wall -I. -c $(APP).c -o $(APP).o
	gcc -Wall -O2 $(APP).o -lbpf -lelf -lz -o $(APP)

clean:
	rm -f *.o *.skel.h $(APP)
