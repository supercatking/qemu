#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

need_file LINUX_SRC "$LINUX_SRC/scripts/kconfig/merge_config.sh"
need_cmd "${CROSS_COMPILE}gcc" CROSS_COMPILE
mkdir -p "$LINUX_BUILD" "$VIRT_LLM_ARTIFACT_DIR"
CFG=$VIRT_LLM_ARTIFACT_DIR/linux-rv32-extra.config
cat > "$CFG" <<'CFGEOF'
CONFIG_BLK_DEV_INITRD=y
CONFIG_DEVTMPFS=y
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

cd "$LINUX_SRC"
ARCH=riscv scripts/kconfig/merge_config.sh \
  -O "$LINUX_BUILD" \
  arch/riscv/configs/defconfig \
  arch/riscv/configs/32-bit.config \
  "$CFG"
make ARCH=riscv CROSS_COMPILE="$CROSS_COMPILE" O="$LINUX_BUILD" olddefconfig
make ARCH=riscv CROSS_COMPILE="$CROSS_COMPILE" O="$LINUX_BUILD" -j"$(nproc)" Image
file "$LINUX_IMAGE"
ls -lh "$LINUX_IMAGE"
