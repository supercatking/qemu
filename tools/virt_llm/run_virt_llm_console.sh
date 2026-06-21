#!/usr/bin/env bash
set -euo pipefail

SRC=/home/qemu/linux-6.12/tools/testing/selftests/virt_llm/virt-llm-console.c
BIN=/tmp/virt-llm-console-rv32
INITDIR=/tmp/virt-llm-console-initramfs
INITRD=/home/qemu/initramfs-console.cpio

riscv64-linux-gnu-gcc \
  -nostdlib -static -ffreestanding -fno-builtin -Os \
  -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
  -o "$BIN" "$SRC"

rm -rf "$INITDIR"
mkdir -p "$INITDIR"
cp "$BIN" "$INITDIR/init"
chmod +x "$INITDIR/init"
(cd "$INITDIR" && find . | cpio -o -H newc > "$INITRD")

echo "Built $INITRD"
echo "Commands: help, info, gemm, attention, reboot"
echo "QEMU escape: Ctrl-a x"

exec /home/qemu/qemu/build/qemu-system-riscv32 \
  -machine virt \
  -nographic \
  -m 256M \
  -smp 1 \
  -no-reboot \
  -device virt-llm \
  -kernel /home/qemu/linux-6.12-build-rv32/arch/riscv/boot/Image \
  -initrd "$INITRD" \
  -append "console=ttyS0 earlycon=sbi rdinit=/init loglevel=8"
