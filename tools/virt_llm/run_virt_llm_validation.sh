#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

ensure_dirs
need_exe QEMU_BIN "$QEMU_BIN"
need_file LINUX_IMAGE "$LINUX_IMAGE"
INITRD=${INITRD:-$VIRT_LLM_ARTIFACT_DIR/initramfs-test.cpio}
"$SCRIPT_DIR/build_initramfs.sh" --mode test --out "$INITRD" >/dev/null
LOG=${LOG:-$VIRT_LLM_LOG_DIR/virt-llm-riscv32-linux-6.12.log}
rm -f "$LOG"

rc=0
timeout --foreground 90s \
  "$QEMU_BIN" \
  -machine virt \
  -nographic \
  -m 256M \
  -smp 1 \
  -no-reboot \
  -device "$(virt_llm_device_arg)" \
  -kernel "$LINUX_IMAGE" \
  -initrd "$INITRD" \
  -append "console=ttyS0 earlycon=sbi rdinit=/init loglevel=8" \
  > "$LOG" 2>&1 || rc=$?

echo "QEMU_EXIT_CODE=$rc"
grep -E 'Linux version 6.12.0|pci|virt_llm|dma inference ok|INITRAMFS_OK|reboot: Restarting system|Kernel panic' "$LOG" | tail -140 || true
echo "LOG_PATH=$LOG"

test "$rc" -eq 0
grep -q 'Linux version 6.12.0' "$LOG"
grep -q 'virt_llm_pci .*probe ok:' "$LOG"
grep -q 'virt_llm_pci .*dma inference ok:' "$LOG"
grep -q 'INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32' "$LOG"
