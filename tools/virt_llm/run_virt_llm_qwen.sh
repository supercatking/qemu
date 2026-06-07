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
EXPECTED_TOKENS=${VIRT_LLM_QWEN_EXPECTED_TOKENS:-785,6722,315,9625,374,12095,13,151645}
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
echo "QEMU_EXIT_CODE=$rc"
if [ "$rc" -ne 0 ]; then
  echo "ERROR: QEMU exited with non-zero status while running Qwen validation" >&2
  exit "$rc"
fi

if ! grep -q 'qwen model load ok' "$LOG"; then
  echo "ERROR: Qwen validation did not report model load success" >&2
  exit 1
fi

if ! grep -q 'qwen full layers ok' "$LOG"; then
  echo "ERROR: Qwen validation did not report full-layer graph success" >&2
  exit 1
fi

infer_line=$(grep 'QWEN_INFER_OK' "$LOG" | tail -1 || true)
if [ -n "$infer_line" ]; then
  echo "$infer_line"
  if ! printf '%s\n' "$infer_line" | grep -q "output_tokens=$EXPECTED_TOKENS"; then
    echo "ERROR: Qwen exact-match validation produced unexpected tokens" >&2
    echo "EXPECTED_TOKENS=$EXPECTED_TOKENS" >&2
    echo "ACTUAL_LINE=$infer_line" >&2
    exit 1
  fi
  exit 0
fi

echo "ERROR: Qwen exact-match validation failed: missing QWEN_INFER_OK" >&2
echo "The guest runtime may still be using the older smoke-test markers." >&2
grep -E 'QWEN_SINGLE_TOKEN_OK|QWEN_DECODE_OK|QWEN_INFER_FAIL|qwen decode step' "$LOG" | tail -80 >&2 || true
exit 1
