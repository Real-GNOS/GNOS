# hudos-server userland — freestanding aarch64 PIE ELF executables.
# Linux toolchain variant (clang + ld.lld from the system package).
CC       := clang
LD       := ld.lld
CFLAGS   := -target aarch64-none-elf -ffreestanding -fPIE -fno-stack-protector \
            -fno-builtin -nostdlib -I. -c

PROGS := hello.elf echo.elf calc.elf

.PHONY: all clean

all: $(PROGS)

%.elf: %.c crt0.o syscall.o
	$(CC) $(CFLAGS) $< -o $*.o
	$(LD) -pie -o $@ $*.o crt0.o syscall.o

crt0.o: crt0.S
	$(CC) -target aarch64-none-elf -c crt0.S -o crt0.o

syscall.o: syscall.c
	$(CC) $(CFLAGS) syscall.c -o syscall.o

clean:
	rm -f $(PROGS) *.o
