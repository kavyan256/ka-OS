#!/bin/bash
set -xue

QEMU=qemu-system-riscv32

# Path to clang and compiler flags
CC=clang
CFLAGS="-std=c11 -O2 -g3 -Wall -Wextra --target=riscv32-unknown-elf -fuse-ld=lld -fno-stack-protector -ffreestanding -nostdlib"

if [ -x "/opt/homebrew/opt/llvm/bin/llvm-objcopy" ]; then
    OBJCOPY="/opt/homebrew/opt/llvm/bin/llvm-objcopy"
elif command -v llvm-objcopy >/dev/null 2>&1; then
    OBJCOPY="$(command -v llvm-objcopy)"
elif command -v objcopy >/dev/null 2>&1; then
    OBJCOPY="$(command -v objcopy)"
else
    echo "ERROR: llvm-objcopy or objcopy not found; install llvm or binutils" >&2
    exit 1
fi

# Build the shell (application)
$CC $CFLAGS -Wl,-Tuser.ld -Wl,-Map=shell.map -o shell.elf shell.c user.c common.c
$OBJCOPY --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
$OBJCOPY -Ibinary -Oelf32-littleriscv shell.bin shell.bin.o

# Build the preemption test program (run it with: DEMO=1 ./run.sh)
$CC $CFLAGS -Wl,-Tuser.ld -Wl,-Map=spin.map -o spin.elf spin.c user.c common.c
$OBJCOPY --set-section-flags .bss=alloc,contents -O binary spin.elf spin.bin
$OBJCOPY -Ibinary -Oelf32-littleriscv spin.bin spin.bin.o

KERNEL_DEFS=""
if [ "${DEMO:-0}" = "1" ]; then
    KERNEL_DEFS="-DPREEMPT_DEMO"
fi

# Build the kernel
$CC $CFLAGS $KERNEL_DEFS -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf \
    kernel.c common.c shell.bin.o spin.bin.o

# Build the disk image; an empty (or missing) disk/ directory gives an empty tar.
mkdir -p disk
(cd disk && find . -maxdepth 1 -name '*.txt' -printf '%f\n' | tar cf ../disk.tar --format=ustar -T /dev/stdin)
truncate -s 16K disk.tar                                                      # room for the kernel's whole disk buffer

# Start QEMU
$QEMU -machine virt -bios default -nographic -serial mon:stdio --no-reboot \
    -d unimp,guest_errors,int,cpu_reset -D qemu.log \
    -drive id=drive0,file=disk.tar,format=raw,if=none \
    -device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
    -kernel kernel.elf