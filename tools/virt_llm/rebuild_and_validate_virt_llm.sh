#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

BITS_LIST=${VIRT_LLM_GUEST_BITS_LIST:-32 64}
RUN_VALIDATION=${VIRT_LLM_RUN_VALIDATION:-1}
RUN_QWEN=${VIRT_LLM_RUN_QWEN:-0}

virt_llm_step "one-click virt-llm platform rebuild start bits=[$BITS_LIST]"
"$SCRIPT_DIR/build_qemu_virt_llm.sh"

for bits in $BITS_LIST; do
  virt_llm_step "build Linux 6.12 for riscv$bits"
  VIRT_LLM_GUEST_BITS=$bits "$SCRIPT_DIR/build_linux_6_12_riscv.sh"
  if [ "$RUN_VALIDATION" = 1 ]; then
    virt_llm_step "validate riscv$bits Linux on virt-llm QEMU"
    VIRT_LLM_GUEST_BITS=$bits "$SCRIPT_DIR/run_virt_llm_validation.sh"
  fi
done

if [ "$RUN_QWEN" = 1 ]; then
  virt_llm_step "optional Qwen validation on riscv${VIRT_LLM_QWEN_BITS:-32}"
  VIRT_LLM_GUEST_BITS=${VIRT_LLM_QWEN_BITS:-32} "$SCRIPT_DIR/run_virt_llm_qwen.sh"
fi

virt_llm_step "one-click virt-llm platform rebuild complete"
