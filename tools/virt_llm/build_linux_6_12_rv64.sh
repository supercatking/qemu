#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export VIRT_LLM_GUEST_BITS=64
exec "$SCRIPT_DIR/build_linux_6_12_riscv.sh" "$@"
