#!/usr/bin/env bash
set -euo pipefail

virt_llm_script_dir() {
  local src=${BASH_SOURCE[0]}
  while [ -L "$src" ]; do
    local dir
    dir=$(cd -P "$(dirname "$src")" >/dev/null 2>&1 && pwd)
    src=$(readlink "$src")
    [[ $src != /* ]] && src=$dir/$src
  done
  cd -P "$(dirname "$src")" >/dev/null 2>&1 && pwd
}

VIRT_LLM_SCRIPT_DIR=${VIRT_LLM_SCRIPT_DIR:-$(virt_llm_script_dir)}
QEMU_SRC=${QEMU_SRC:-$(cd "$VIRT_LLM_SCRIPT_DIR/../.." && pwd)}
VIRT_LLM_GUEST_BITS=${VIRT_LLM_GUEST_BITS:-32}
case "$VIRT_LLM_GUEST_BITS" in
  32|64) ;;
  *) echo "unsupported VIRT_LLM_GUEST_BITS=$VIRT_LLM_GUEST_BITS; expected 32 or 64" >&2; exit 2 ;;
esac

VIRT_LLM_GUEST_ARCH=riscv$VIRT_LLM_GUEST_BITS
QEMU_BUILD=${QEMU_BUILD:-$QEMU_SRC/build}
QEMU_SYSTEM=${QEMU_SYSTEM:-qemu-system-$VIRT_LLM_GUEST_ARCH}
QEMU_BIN=${QEMU_BIN:-$QEMU_BUILD/$QEMU_SYSTEM}
LINUX_SRC=${LINUX_SRC:-/home/qemu/linux-6.12}
LINUX_BUILD=${LINUX_BUILD:-/home/qemu/linux-6.12-build-rv$VIRT_LLM_GUEST_BITS}
LINUX_IMAGE=${LINUX_IMAGE:-$LINUX_BUILD/arch/riscv/boot/Image}
CROSS_COMPILE=${CROSS_COMPILE:-riscv64-linux-gnu-}
VIRT_LLM_MODEL_PATH=${VIRT_LLM_MODEL_PATH:-/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors}
VIRT_LLM_ARTIFACT_DIR=${VIRT_LLM_ARTIFACT_DIR:-$QEMU_BUILD/virt-llm-artifacts}
VIRT_LLM_LOG_DIR=${VIRT_LLM_LOG_DIR:-$VIRT_LLM_ARTIFACT_DIR/logs}

virt_llm_ts() {
  date '+%Y-%m-%d %H:%M:%S'
}

virt_llm_step() {
  printf '\n[%s] [virt-llm] >>> %s\n' "$(virt_llm_ts)" "$*"
}

virt_llm_info() {
  printf '[%s] [virt-llm] %s\n' "$(virt_llm_ts)" "$*"
}

need_file() {
  local var=$1 path=$2
  if [ ! -f "$path" ]; then
    echo "missing $var: $path" >&2
    return 1
  fi
}

need_exe() {
  local var=$1 path=$2
  if [ ! -x "$path" ]; then
    echo "missing executable $var: $path" >&2
    return 1
  fi
}

need_cmd() {
  local cmd=$1 hint=${2:-$1}
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "missing command: $cmd (set/install $hint)" >&2
    return 1
  fi
}

ensure_dirs() {
  mkdir -p "$VIRT_LLM_ARTIFACT_DIR" "$VIRT_LLM_LOG_DIR"
}

virt_llm_riscv_march() {
  if [ "$VIRT_LLM_GUEST_BITS" = 64 ]; then
    printf '%s' rv64imac_zicsr_zifencei
  else
    printf '%s' rv32imac_zicsr_zifencei
  fi
}

virt_llm_riscv_mabi() {
  if [ "$VIRT_LLM_GUEST_BITS" = 64 ]; then
    printf '%s' lp64
  else
    printf '%s' ilp32
  fi
}

virt_llm_initramfs_marker() {
  printf 'INITRAMFS_OK: Linux 6.12 booted on QEMU riscv%s' "$VIRT_LLM_GUEST_BITS"
}

virt_llm_device_arg() {
  if [ -n "${VIRT_LLM_MODEL_PATH:-}" ]; then
    printf '%s' "virt-llm,model-path=$VIRT_LLM_MODEL_PATH"
  else
    printf '%s' "virt-llm"
  fi
}
