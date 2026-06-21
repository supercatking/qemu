#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

ensure_dirs
need_exe QEMU_BIN "$QEMU_BIN"
need_file LINUX_IMAGE "$LINUX_IMAGE"
INITRD=${INITRD:-$VIRT_LLM_ARTIFACT_DIR/initramfs-test-rv$VIRT_LLM_GUEST_BITS.cpio}
"$SCRIPT_DIR/build_initramfs.sh" --mode test --out "$INITRD" >/dev/null
LOG=${LOG:-$VIRT_LLM_LOG_DIR/virt-llm-riscv$VIRT_LLM_GUEST_BITS-linux-6.12.log}
MARKER=$(virt_llm_initramfs_marker)
rm -f "$LOG"

virt_llm_step "run basic validation guest=$VIRT_LLM_GUEST_ARCH qemu=$QEMU_BIN linux=$LINUX_IMAGE"
rc=0
timeout --foreground "${VIRT_LLM_BOOT_TIMEOUT:-90s}" \
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
grep -E 'Linux version |pci|virt_llm|dma inference ok|INITRAMFS_OK|reboot: Restarting system|Kernel panic' "$LOG" | tail -180 || true
echo "LOG_PATH=$LOG"

test "$rc" -eq 0
grep -q 'Linux version ' "$LOG"
grep -q 'virt_llm_pci .*probe ok:' "$LOG"
grep -q 'virt_llm_pci .*dma inference ok:' "$LOG"
grep -q "$MARKER" "$LOG"
