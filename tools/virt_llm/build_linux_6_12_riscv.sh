#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

JOBS=${JOBS:-$(nproc)}
need_file LINUX_SRC "$LINUX_SRC/scripts/kconfig/merge_config.sh"
need_cmd "${CROSS_COMPILE}gcc" CROSS_COMPILE
need_cmd make make
mkdir -p "$LINUX_BUILD" "$VIRT_LLM_ARTIFACT_DIR"

CFG=$VIRT_LLM_ARTIFACT_DIR/linux-rv$VIRT_LLM_GUEST_BITS-extra.config
cat > "$CFG" <<'CFGEOF'
CONFIG_BLK_DEV_INITRD=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_HVC_RISCV_SBI=y
CONFIG_EARLY_PRINTK=y
CONFIG_PRINTK_TIME=y
CONFIG_DEBUG_KERNEL=y
CONFIG_DEBUG_INFO_NONE=y
CONFIG_MODULES=n
CONFIG_VIRT_LLM_PCI=y
CFGEOF

configs=(arch/riscv/configs/defconfig)
if [ "$VIRT_LLM_GUEST_BITS" = 32 ]; then
  configs+=(arch/riscv/configs/32-bit.config)
fi

virt_llm_step "configure Linux 6.12 guest=$VIRT_LLM_GUEST_ARCH build=$LINUX_BUILD"
(
  cd "$LINUX_SRC"
  ARCH=riscv scripts/kconfig/merge_config.sh \
    -O "$LINUX_BUILD" \
    "${configs[@]}" \
    "$CFG"
)

virt_llm_step "olddefconfig Linux guest=$VIRT_LLM_GUEST_ARCH"
make -C "$LINUX_SRC" ARCH=riscv CROSS_COMPILE="$CROSS_COMPILE" O="$LINUX_BUILD" olddefconfig

virt_llm_step "build Linux Image guest=$VIRT_LLM_GUEST_ARCH jobs=$JOBS"
make -C "$LINUX_SRC" ARCH=riscv CROSS_COMPILE="$CROSS_COMPILE" O="$LINUX_BUILD" -j"$JOBS" Image

need_file LINUX_IMAGE "$LINUX_IMAGE"
file "$LINUX_IMAGE"
ls -lh "$LINUX_IMAGE"
