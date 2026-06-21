#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
"$SCRIPT_DIR/build_linux_6_12_rv32.sh"
"$SCRIPT_DIR/run_virt_llm_validation.sh"
