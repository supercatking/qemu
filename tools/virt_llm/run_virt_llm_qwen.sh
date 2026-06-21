#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

ensure_dirs
need_exe QEMU_BIN "$QEMU_BIN"
need_file LINUX_IMAGE "$LINUX_IMAGE"
need_file VIRT_LLM_MODEL_PATH "$VIRT_LLM_MODEL_PATH"
INITRD=${INITRD:-$VIRT_LLM_ARTIFACT_DIR/initramfs-qwen.cpio}
"$SCRIPT_DIR/build_initramfs.sh" --mode qwen --out "$INITRD" >/dev/null
LOG=${LOG:-$VIRT_LLM_LOG_DIR/virt-llm-qwen.log}
rm -f "$LOG"

rc=0
timeout --foreground 900s \
  "$QEMU_BIN" \
  -machine virt \
  -nographic \
  -m 512M \
  -smp 1 \
  -no-reboot \
  -device "$(virt_llm_device_arg)" \
  -kernel "$LINUX_IMAGE" \
  -initrd "$INITRD" \
  -append "console=ttyS0 earlycon=sbi rdinit=/init loglevel=8" \
  > "$LOG" 2>&1 || rc=$?

grep -E 'qwen |QWEN_|virt-llm:' "$LOG" | tail -120 || true
echo "LOG_PATH=$LOG"
test "$rc" -eq 0
grep -q 'QWEN_SINGLE_TOKEN_OK' "$LOG"
