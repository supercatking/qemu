#!/usr/bin/env bash
set -euo pipefail

# Fresh-clone platform reproduction for virt-llm.
# It validates that the project can be rebuilt outside the historical
# /home/qemu source layout and that both riscv32 and riscv64 system targets
# can boot Linux 6.12 with the virt-llm PCIe device.

QEMU_REPO=${QEMU_REPO:-https://github.com/supercatking/qemu.git}
QEMU_BRANCH=${QEMU_BRANCH:-llmdev}
LINUX_REPO=${LINUX_REPO:-https://github.com/supercatking/linux.git}
LINUX_BRANCH=${LINUX_BRANCH:-llmdev-linux-6.12}

QEMU_DIR=${QEMU_DIR:-/tmp/qemu}
LINUX_DIR=${LINUX_DIR:-/tmp/linux}
REPORT_DIR=${REPORT_DIR:-/tmp/virt-llm-platform-repro}
BITS_LIST=${VIRT_LLM_GUEST_BITS_LIST:-32 64}
JOBS=${JOBS:-$(nproc)}
CROSS_COMPILE=${CROSS_COMPILE:-riscv64-linux-gnu-}
REQUESTED_QWEN_MODEL_PATH=${VIRT_LLM_MODEL_PATH:-}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "ERROR: missing command: $1" >&2
    exit 2
  fi
}

log_step() {
  printf '\n[%s] [virt-llm-platform] >>> %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1"
}

append_summary() {
  printf '%s\n' "$*" >> "$SUMMARY"
}

git_clone_retry() {
  local branch=$1
  local repo=$2
  local dest=$3
  local attempt

  for attempt in 1 2 3 4 5; do
    rm -rf "$dest"
    echo "clone attempt $attempt: $repo branch=$branch -> $dest"
    if git -c http.version=HTTP/1.1 clone --depth 1 --branch "$branch" \
      --single-branch "$repo" "$dest"; then
      return 0
    fi
    sleep $((attempt * 5))
  done
  echo "ERROR: clone failed after retries: $repo branch=$branch" >&2
  return 1
}

target_list_from_bits() {
  local out=
  local bits
  for bits in $BITS_LIST; do
    case "$bits" in
      32|64) ;;
      *) echo "ERROR: unsupported guest bits in VIRT_LLM_GUEST_BITS_LIST: $bits" >&2; exit 2 ;;
    esac
    if [ -n "$out" ]; then
      out=$out,
    fi
    out=${out}riscv${bits}-softmmu
  done
  printf '%s' "$out"
}

need_cmd git
need_cmd make
need_cmd ninja
need_cmd python3
need_cmd cpio
need_cmd "${CROSS_COMPILE}gcc"

mkdir -p "$REPORT_DIR"
SUMMARY="$REPORT_DIR/summary.md"
QEMU_LOG="$REPORT_DIR/qemu-build.log"
QWEN_LOG="$REPORT_DIR/qwen-validation.log"
TARGET_LIST=$(target_list_from_bits)

cat > "$SUMMARY" <<EOF
# virt-llm platform fresh clone reproduction

Start: $(date -Is)

Configuration:

- QEMU repo: \`$QEMU_REPO\`
- QEMU branch: \`$QEMU_BRANCH\`
- Linux repo: \`$LINUX_REPO\`
- Linux branch: \`$LINUX_BRANCH\`
- Guest bits list: \`$BITS_LIST\`
- QEMU target list: \`$TARGET_LIST\`
- Report dir: \`$REPORT_DIR\`

EOF

log_step "clean previous fresh-clone directories"
rm -rf "$QEMU_DIR" "$LINUX_DIR"

log_step "clone QEMU"
git_clone_retry "$QEMU_BRANCH" "$QEMU_REPO" "$QEMU_DIR"
QEMU_COMMIT=$(git -C "$QEMU_DIR" rev-parse HEAD)

log_step "clone Linux"
git_clone_retry "$LINUX_BRANCH" "$LINUX_REPO" "$LINUX_DIR"
LINUX_COMMIT=$(git -C "$LINUX_DIR" rev-parse HEAD)
LINUX_VERSION=$(make -s -C "$LINUX_DIR" kernelversion)

log_step "build QEMU targets: $TARGET_LIST"
(
  QEMU_SRC="$QEMU_DIR" \
  QEMU_BUILD="$QEMU_DIR/build" \
  QEMU_TARGET_LIST="$TARGET_LIST" \
  JOBS="$JOBS" \
  "$QEMU_DIR/tools/virt_llm/build_qemu_virt_llm.sh"
) 2>&1 | tee "$QEMU_LOG"

append_summary "## Commits"
append_summary ""
append_summary "- QEMU commit: \`$QEMU_COMMIT\`"
append_summary "- Linux commit: \`$LINUX_COMMIT\`"
append_summary "- Linux version: \`$LINUX_VERSION\`"
append_summary ""
append_summary "## Per-guest validation"
append_summary ""

for bits in $BITS_LIST; do
  log_step "build Linux riscv$bits Image"
  LINUX_BUILD="$REPORT_DIR/linux-build-rv$bits"
  LINUX_LOG="$REPORT_DIR/linux-build-rv$bits.log"
  (
    QEMU_SRC="$QEMU_DIR" \
    QEMU_BUILD="$QEMU_DIR/build" \
    LINUX_SRC="$LINUX_DIR" \
    LINUX_BUILD="$LINUX_BUILD" \
    VIRT_LLM_GUEST_BITS="$bits" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    JOBS="$JOBS" \
    "$QEMU_DIR/tools/virt_llm/build_linux_6_12_riscv.sh"
  ) 2>&1 | tee "$LINUX_LOG"

  LINUX_IMAGE="$LINUX_BUILD/arch/riscv/boot/Image"
  VALIDATION_LOG="$REPORT_DIR/validation-rv$bits.log"
  log_step "run basic validation riscv$bits"
  (
    QEMU_SRC="$QEMU_DIR" \
    QEMU_BUILD="$QEMU_DIR/build" \
    LINUX_SRC="$LINUX_DIR" \
    LINUX_BUILD="$LINUX_BUILD" \
    LINUX_IMAGE="$LINUX_IMAGE" \
    VIRT_LLM_GUEST_BITS="$bits" \
    VIRT_LLM_LOG_DIR="$REPORT_DIR/logs" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    "$QEMU_DIR/tools/virt_llm/run_virt_llm_validation.sh"
  ) 2>&1 | tee "$VALIDATION_LOG"

  grep -q 'probe ok:' "$VALIDATION_LOG"
  grep -q 'gemm ok:' "$VALIDATION_LOG"
  grep -q 'attention q16 ok:' "$VALIDATION_LOG"
  grep -q "INITRAMFS_OK: Linux 6.12 booted on QEMU riscv$bits" "$VALIDATION_LOG"

  append_summary "- riscv$bits: PASS"
  append_summary "  - Linux build log: \`$LINUX_LOG\`"
  append_summary "  - Validation log: \`$VALIDATION_LOG\`"
done

QWEN_STATUS=SKIP
QWEN_LINE=
if [ -n "$REQUESTED_QWEN_MODEL_PATH" ]; then
  log_step "run optional Qwen exact-match validation on riscv${VIRT_LLM_QWEN_BITS:-32}"
  if [ ! -f "$REQUESTED_QWEN_MODEL_PATH" ]; then
    echo "ERROR: VIRT_LLM_MODEL_PATH is set but file does not exist: $REQUESTED_QWEN_MODEL_PATH" >&2
    exit 1
  fi
  QWEN_BITS=${VIRT_LLM_QWEN_BITS:-32}
  QWEN_LINUX_BUILD="$REPORT_DIR/linux-build-rv$QWEN_BITS"
  (
    QEMU_SRC="$QEMU_DIR" \
    QEMU_BUILD="$QEMU_DIR/build" \
    LINUX_SRC="$LINUX_DIR" \
    LINUX_BUILD="$QWEN_LINUX_BUILD" \
    LINUX_IMAGE="$QWEN_LINUX_BUILD/arch/riscv/boot/Image" \
    VIRT_LLM_GUEST_BITS="$QWEN_BITS" \
    VIRT_LLM_MODEL_PATH="$REQUESTED_QWEN_MODEL_PATH" \
    VIRT_LLM_LOG_DIR="$REPORT_DIR/logs" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    "$QEMU_DIR/tools/virt_llm/run_virt_llm_qwen.sh"
  ) 2>&1 | tee "$QWEN_LOG"
  grep -q 'QWEN_INFER_OK' "$QWEN_LOG"
  QWEN_LINE=$(grep 'QWEN_INFER_OK' "$QWEN_LOG" | tail -1)
  QWEN_STATUS=PASS
else
  echo "SKIP: VIRT_LLM_MODEL_PATH is not set; Qwen exact-match validation was not run." > "$QWEN_LOG"
fi

append_summary ""
append_summary "## Optional Qwen validation"
append_summary ""
append_summary "- Status: \`$QWEN_STATUS\`"
append_summary "- Log: \`$QWEN_LOG\`"
if [ "$QWEN_STATUS" = PASS ]; then
  append_summary "- Marker: \`$QWEN_LINE\`"
fi
append_summary ""
append_summary "End: $(date -Is)"
append_summary ""
append_summary "Conclusion: PASS"

log_step "PASS"
cat "$SUMMARY"
