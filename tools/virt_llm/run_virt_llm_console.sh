#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

ensure_dirs
need_exe QEMU_BIN "$QEMU_BIN"
need_file LINUX_IMAGE "$LINUX_IMAGE"
INITRD=${INITRD:-$VIRT_LLM_ARTIFACT_DIR/initramfs-console.cpio}
"$SCRIPT_DIR/build_initramfs.sh" --mode console --out "$INITRD" >/dev/null

echo "Built $INITRD"
echo "Commands: help, info, gemm, attention, reboot"
echo "QEMU escape: Ctrl-a x"

exec "$QEMU_BIN" \
  -machine virt \
  -nographic \
  -m 256M \
  -smp 1 \
  -no-reboot \
  -device "$(virt_llm_device_arg)" \
  -kernel "$LINUX_IMAGE" \
  -initrd "$INITRD" \
  -append "console=ttyS0 earlycon=sbi rdinit=/init loglevel=8"
