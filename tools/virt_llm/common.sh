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
QEMU_BUILD=${QEMU_BUILD:-$QEMU_SRC/build}
QEMU_BIN=${QEMU_BIN:-$QEMU_BUILD/qemu-system-riscv32}
LINUX_SRC=${LINUX_SRC:-/home/qemu/linux-6.12}
LINUX_BUILD=${LINUX_BUILD:-/home/qemu/linux-6.12-build-rv32}
LINUX_IMAGE=${LINUX_IMAGE:-$LINUX_BUILD/arch/riscv/boot/Image}
CROSS_COMPILE=${CROSS_COMPILE:-riscv64-linux-gnu-}
VIRT_LLM_MODEL_PATH=${VIRT_LLM_MODEL_PATH:-/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors}
VIRT_LLM_ARTIFACT_DIR=${VIRT_LLM_ARTIFACT_DIR:-$QEMU_BUILD/virt-llm-artifacts}
VIRT_LLM_LOG_DIR=${VIRT_LLM_LOG_DIR:-$VIRT_LLM_ARTIFACT_DIR/logs}

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

virt_llm_device_arg() {
  if [ -n "${VIRT_LLM_MODEL_PATH:-}" ]; then
    printf '%s' "virt-llm,model-path=$VIRT_LLM_MODEL_PATH"
  else
    printf '%s' "virt-llm"
  fi
}
