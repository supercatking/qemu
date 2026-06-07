#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

MODE=test
OUT=
while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE=${2:?}; shift 2 ;;
    --out) OUT=${2:?}; shift 2 ;;
    *) echo "usage: $0 [--mode test|console|qwen] [--out initramfs.cpio]" >&2; exit 2 ;;
  esac
done

ensure_dirs
need_cmd "${CROSS_COMPILE}gcc" CROSS_COMPILE
need_cmd cpio cpio

case "$MODE" in
  test) OUT=${OUT:-$VIRT_LLM_ARTIFACT_DIR/initramfs-test.cpio} ;;
  console) OUT=${OUT:-$VIRT_LLM_ARTIFACT_DIR/initramfs-console.cpio} ;;
  qwen) OUT=${OUT:-$VIRT_LLM_ARTIFACT_DIR/initramfs-qwen.cpio} ;;
  *) echo "unsupported initramfs mode: $MODE" >&2; exit 2 ;;
esac

MARCH=$(virt_llm_riscv_march)
MABI=$(virt_llm_riscv_mabi)
virt_llm_step "build initramfs mode=$MODE guest=$VIRT_LLM_GUEST_ARCH out=$OUT"
TMPBASE=${TMPDIR:-/tmp}
WORK=$(mktemp -d "$TMPBASE/virt-llm-$MODE-initramfs.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/root"

if [ "$MODE" = test ]; then
  MARKER=$(virt_llm_initramfs_marker)
  MARKER_LEN=$((${#MARKER} + 1))
  cat > "$WORK/init.S" <<ASM
.section .text
.global _start
_start:
    li a0, 1
    la a1, msg
    li a2, $MARKER_LEN
    li a7, 64
    ecall

    li a0, 0xfee1dead
    li a1, 672274793
    li a2, 0x1234567
    li a3, 0
    li a7, 142
    ecall

1:  j 1b

.section .rodata
msg:
    .ascii "$MARKER\n"
ASM
  "${CROSS_COMPILE}gcc" -nostdlib -static \
    -march="$MARCH" -mabi="$MABI" \
    -Wl,-e,_start -o "$WORK/root/init" "$WORK/init.S"
elif [ "$MODE" = console ]; then
  SRC=$LINUX_SRC/tools/testing/selftests/virt_llm/virt-llm-console.c
  need_file LINUX_SRC "$SRC"
  "${CROSS_COMPILE}gcc" -nostdlib -static -ffreestanding -fno-builtin -Os \
    -march="$MARCH" -mabi="$MABI" \
    -o "$WORK/root/init" "$SRC"
else
  SRC=$LINUX_SRC/tools/testing/selftests/virt_llm/virt-llm-qwen.c
  need_file LINUX_SRC "$SRC"
  "${CROSS_COMPILE}gcc" -nostdlib -static -ffreestanding -fno-builtin -Os \
    -march="$MARCH" -mabi="$MABI" \
    -o "$WORK/root/init" "$SRC"
fi

chmod +x "$WORK/root/init"
mkdir -p "$(dirname "$OUT")"
(cd "$WORK/root" && find . | cpio -o -H newc > "$OUT")
echo "INITRAMFS=$OUT"
