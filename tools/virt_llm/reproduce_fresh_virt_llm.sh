#!/usr/bin/env bash
set -euo pipefail

# Fresh-clone reproducibility test for virt-llm.
# Rebuilds QEMU and Linux from GitHub under /tmp without using the caller's
# existing /home/qemu/qemu or /home/qemu/linux-6.12 trees.

QEMU_REPO=${QEMU_REPO:-https://github.com/supercatking/qemu.git}
QEMU_BRANCH=${QEMU_BRANCH:-llmdev}
LINUX_REPO=${LINUX_REPO:-https://github.com/supercatking/linux.git}
LINUX_BRANCH=${LINUX_BRANCH:-llmdev-linux-6.12}

QEMU_DIR=${QEMU_DIR:-/tmp/qemu}
LINUX_DIR=${LINUX_DIR:-/tmp/linux}
LINUX_BUILD=${LINUX_BUILD:-/tmp/linux-build-rv32}
REPORT_DIR=${REPORT_DIR:-/tmp/virt-llm-fresh-repro}

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
  printf '\n========== %s ==========\n' "$1"
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

need_cmd git
need_cmd make
need_cmd ninja
need_cmd python3
need_cmd cpio
need_cmd "${CROSS_COMPILE}gcc"

mkdir -p "$REPORT_DIR"
SUMMARY="$REPORT_DIR/summary.md"
QEMU_LOG="$REPORT_DIR/qemu-build.log"
LINUX_LOG="$REPORT_DIR/linux-build.log"
VALIDATION_LOG="$REPORT_DIR/validation.log"
QWEN_LOG="$REPORT_DIR/qwen-validation.log"

cat > "$SUMMARY" <<EOF
# virt-llm fresh clone reproduction

Start: $(date -Is)

EOF

log_step "Clean previous fresh-clone directories"
rm -rf "$QEMU_DIR" "$LINUX_DIR" "$LINUX_BUILD"

log_step "Clone QEMU"
git_clone_retry "$QEMU_BRANCH" "$QEMU_REPO" "$QEMU_DIR"
QEMU_COMMIT=$(git -C "$QEMU_DIR" rev-parse HEAD)

log_step "Configure QEMU"
(
  cd "$QEMU_DIR"
  ./configure --target-list=riscv32-softmmu --disable-werror
) 2>&1 | tee "$QEMU_LOG"

log_step "Build QEMU"
ninja -C "$QEMU_DIR/build" -j"$JOBS" qemu-system-riscv32 2>&1 | tee -a "$QEMU_LOG"
test -x "$QEMU_DIR/build/qemu-system-riscv32"

log_step "Check virt-llm model-path property"
"$QEMU_DIR/build/qemu-system-riscv32" -device virt-llm,help | tee "$REPORT_DIR/qemu-device-help.txt"
grep -q 'model-path=<str>' "$REPORT_DIR/qemu-device-help.txt"

log_step "Clone Linux"
git_clone_retry "$LINUX_BRANCH" "$LINUX_REPO" "$LINUX_DIR"
LINUX_COMMIT=$(git -C "$LINUX_DIR" rev-parse HEAD)
LINUX_VERSION=$(make -s -C "$LINUX_DIR" kernelversion)

log_step "Build Linux riscv32 Image"
(
  QEMU_SRC="$QEMU_DIR" \
  QEMU_BUILD="$QEMU_DIR/build" \
  LINUX_SRC="$LINUX_DIR" \
  LINUX_BUILD="$LINUX_BUILD" \
  CROSS_COMPILE="$CROSS_COMPILE" \
  "$QEMU_DIR/tools/virt_llm/build_linux_6_12_rv32.sh"
) 2>&1 | tee "$LINUX_LOG"

LINUX_IMAGE="$LINUX_BUILD/arch/riscv/boot/Image"
test -f "$LINUX_IMAGE"

log_step "Run fresh QEMU + fresh Linux validation"
(
  QEMU_SRC="$QEMU_DIR" \
  QEMU_BUILD="$QEMU_DIR/build" \
  QEMU_BIN="$QEMU_DIR/build/qemu-system-riscv32" \
  LINUX_SRC="$LINUX_DIR" \
  LINUX_BUILD="$LINUX_BUILD" \
  LINUX_IMAGE="$LINUX_IMAGE" \
  VIRT_LLM_LOG_DIR="$REPORT_DIR/logs" \
  CROSS_COMPILE="$CROSS_COMPILE" \
  "$QEMU_DIR/tools/virt_llm/run_virt_llm_validation.sh"
) 2>&1 | tee "$VALIDATION_LOG"

grep -q 'probe ok:' "$VALIDATION_LOG"
grep -q 'gemm ok:' "$VALIDATION_LOG"
grep -q 'attention q16 ok:' "$VALIDATION_LOG"
grep -q 'INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32' "$VALIDATION_LOG"

QWEN_STATUS="SKIP"
QWEN_INFER_LINE=""
if [ -n "$REQUESTED_QWEN_MODEL_PATH" ]; then
  log_step "Run optional Qwen exact-match validation"
  if [ ! -f "$REQUESTED_QWEN_MODEL_PATH" ]; then
    echo "ERROR: VIRT_LLM_MODEL_PATH is set but file does not exist: $REQUESTED_QWEN_MODEL_PATH" >&2
    exit 1
  fi
  (
    QEMU_SRC="$QEMU_DIR" \
    QEMU_BUILD="$QEMU_DIR/build" \
    QEMU_BIN="$QEMU_DIR/build/qemu-system-riscv32" \
    LINUX_SRC="$LINUX_DIR" \
    LINUX_BUILD="$LINUX_BUILD" \
    LINUX_IMAGE="$LINUX_IMAGE" \
    VIRT_LLM_MODEL_PATH="$REQUESTED_QWEN_MODEL_PATH" \
    VIRT_LLM_LOG_DIR="$REPORT_DIR/logs" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    "$QEMU_DIR/tools/virt_llm/run_virt_llm_qwen.sh"
  ) 2>&1 | tee "$QWEN_LOG"
  grep -q 'qwen model load ok' "$QWEN_LOG"
  grep -q 'qwen full layers ok' "$QWEN_LOG"
  grep -q 'QWEN_INFER_OK' "$QWEN_LOG"
  QWEN_INFER_LINE=$(grep 'QWEN_INFER_OK' "$QWEN_LOG" | tail -1)
  QWEN_STATUS="PASS"
else
  log_step "Skip optional Qwen exact-match validation"
  echo "SKIP: VIRT_LLM_MODEL_PATH is not set; Qwen exact-match validation was not run." > "$QWEN_LOG"
fi

cat >> "$SUMMARY" <<EOF
## Actual validation

- QEMU repo: \`$QEMU_REPO\`
- QEMU branch: \`$QEMU_BRANCH\`
- QEMU commit: \`$QEMU_COMMIT\`
- Linux repo: \`$LINUX_REPO\`
- Linux branch: \`$LINUX_BRANCH\`
- Linux commit: \`$LINUX_COMMIT\`
- Linux version: \`$LINUX_VERSION\`
- Linux Image: \`$LINUX_IMAGE\`

Required checks:

- \`model-path=<str>\`: PASS
- \`probe ok\`: PASS
- \`gemm ok\`: PASS
- \`attention q16 ok\`: PASS
- \`INITRAMFS_OK\`: PASS
- Qwen exact-match: \`$QWEN_STATUS\`

Logs:

- QEMU build log: \`$QEMU_LOG\`
- Linux build log: \`$LINUX_LOG\`
- Validation log: \`$VALIDATION_LOG\`
- Qwen validation log: \`$QWEN_LOG\`
- QEMU runtime logs: \`$REPORT_DIR/logs\`

Conclusion: PASS. The basic virt-llm boot/probe/GEMM/attention path was rebuilt
and validated from fresh clones under \`/tmp\`.
EOF

if [ "$QWEN_STATUS" = "PASS" ]; then
  cat >> "$SUMMARY" <<EOF

## Optional Qwen exact-match validation

- Model path: \`$REQUESTED_QWEN_MODEL_PATH\`
- Result: PASS
- Marker: \`$QWEN_INFER_LINE\`

EOF
else
  cat >> "$SUMMARY" <<'EOF'

## Optional Qwen exact-match validation

- Result: SKIP
- Reason: `VIRT_LLM_MODEL_PATH` was not set.
- To enable: rerun with `VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors`.

EOF
fi

cat >> "$SUMMARY" <<EOF
End: $(date -Is)
EOF

log_step "PASS"
cat "$SUMMARY"
