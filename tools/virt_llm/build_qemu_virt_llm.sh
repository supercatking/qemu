#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

JOBS=${JOBS:-$(nproc)}
QEMU_TARGET_LIST=${QEMU_TARGET_LIST:-riscv32-softmmu,riscv64-softmmu}
QEMU_CONFIGURE_FLAGS=${QEMU_CONFIGURE_FLAGS:---disable-werror}
QEMU_DEBUG_BUILD=${QEMU_DEBUG_BUILD:-0}

need_cmd ninja ninja
need_cmd python3 python3
need_cmd make make

if [ "$QEMU_DEBUG_BUILD" = 1 ]; then
  QEMU_CONFIGURE_FLAGS="$QEMU_CONFIGURE_FLAGS --enable-debug"
fi

need_configure=0
if [ ! -f "$QEMU_BUILD/build.ninja" ]; then
  need_configure=1
fi

IFS=, read -r -a targets <<< "$QEMU_TARGET_LIST"
for target in "${targets[@]}"; do
  binary=${target%-softmmu}
  if [ ! -x "$QEMU_BUILD/qemu-system-$binary" ]; then
    need_configure=1
  fi
done

if [ "${QEMU_FORCE_CONFIGURE:-0}" = 1 ]; then
  need_configure=1
fi

virt_llm_step "build QEMU virt-llm targets=$QEMU_TARGET_LIST build=$QEMU_BUILD jobs=$JOBS"
if [ "$need_configure" = 1 ]; then
  virt_llm_step "configure QEMU target-list=$QEMU_TARGET_LIST flags=$QEMU_CONFIGURE_FLAGS"
  (
    cd "$QEMU_SRC"
    ./configure --target-list="$QEMU_TARGET_LIST" $QEMU_CONFIGURE_FLAGS
  )
else
  virt_llm_info "reuse existing QEMU build directory: $QEMU_BUILD"
fi
ensure_dirs

for target in "${targets[@]}"; do
  binary=${target%-softmmu}
  virt_llm_step "ninja qemu-system-$binary"
  ninja -C "$QEMU_BUILD" -j"$JOBS" "qemu-system-$binary"
  "$QEMU_BUILD/qemu-system-$binary" --version | head -1
done
