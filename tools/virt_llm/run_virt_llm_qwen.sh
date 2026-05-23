#!/usr/bin/env bash
set -euo pipefail

SRC=/home/qemu/linux-6.12/tools/testing/selftests/virt_llm/virt-llm-qwen.c
BIN=/tmp/virt-llm-qwen-rv32
INITDIR=/tmp/virt-llm-qwen-initramfs
INITRD=/home/qemu/initramfs-qwen.cpio
LOG=/home/qemu/virt-llm-qwen.log

riscv64-linux-gnu-gcc \
  -nostdlib -static -ffreestanding -fno-builtin -Os \
  -march=rv32imac_zicsr_zifencei -mabi=ilp32 \
  -o "$BIN" "$SRC"

rm -rf "$INITDIR"
mkdir -p "$INITDIR"
cp "$BIN" "$INITDIR/init"
chmod +x "$INITDIR/init"
(cd "$INITDIR" && find . | cpio -o -H newc > "$INITRD")

rm -f "$LOG"
timeout --foreground 900s \
  /home/qemu/qemu/build/qemu-system-riscv32 \
  -machine virt \
  -nographic \
  -m 512M \
  -smp 1 \
  -no-reboot \
  -device virt-llm \
  -kernel /home/qemu/linux-6.12-build-rv32/arch/riscv/boot/Image \
  -initrd "$INITRD" \
  -append "console=ttyS0 earlycon=sbi rdinit=/init loglevel=8" \
  > "$LOG" 2>&1

grep -E 'qwen |QWEN_|virt-llm:' "$LOG" | tail -120 || true
grep -q 'QWEN_SINGLE_TOKEN_OK' "$LOG"
