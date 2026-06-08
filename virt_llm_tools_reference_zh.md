# virt-llm 工具脚本参考

更新时间：2026-06-08

## 1. 目标

本文档逐个说明 `tools/virt_llm/` 下的脚本、关键环境变量、输出产物和成功标记。平台搭建手册给出主流程；本文档给出脚本级细节。

## 2. 公共环境变量

这些变量由 `tools/virt_llm/common.sh` 解析：

| 变量 | 默认值 | 说明 |
|---|---|---|
| `QEMU_SRC` | 当前 QEMU repo | QEMU 源码目录 |
| `QEMU_BUILD` | `$QEMU_SRC/build` | QEMU build 目录 |
| `QEMU_TARGET_LIST` | `riscv32-softmmu,riscv64-softmmu` | QEMU 构建 target |
| `QEMU_SYSTEM` | `qemu-system-riscv$VIRT_LLM_GUEST_BITS` | 当前运行的 QEMU binary 名称 |
| `QEMU_BIN` | `$QEMU_BUILD/$QEMU_SYSTEM` | 当前运行的 QEMU binary |
| `VIRT_LLM_GUEST_BITS` | `32` | 当前 guest 位宽，`32` 或 `64` |
| `VIRT_LLM_GUEST_BITS_LIST` | `32 64` | 一键脚本覆盖的 guest 位宽 |
| `LINUX_SRC` | `/home/qemu/linux-6.12` | Linux 源码目录 |
| `LINUX_BUILD` | `/home/qemu/linux-6.12-build-rv$VIRT_LLM_GUEST_BITS` | Linux build 目录 |
| `LINUX_IMAGE` | `$LINUX_BUILD/arch/riscv/boot/Image` | Linux kernel Image |
| `CROSS_COMPILE` | `riscv64-linux-gnu-` | cross compiler prefix |
| `VIRT_LLM_MODEL_PATH` | 本地 Qwen safetensors | 可选模型路径 |
| `VIRT_LLM_ARTIFACT_DIR` | `$QEMU_BUILD/virt-llm-artifacts` | initramfs/config 等产物目录 |
| `VIRT_LLM_LOG_DIR` | `$VIRT_LLM_ARTIFACT_DIR/logs` | 运行日志目录 |
| `JOBS` | `nproc` | 并行编译线程 |

## 3. `build_qemu_virt_llm.sh`

用途：构建 QEMU system targets。

默认：

```bash
tools/virt_llm/build_qemu_virt_llm.sh
```

会构建：

```text
qemu-system-riscv32
qemu-system-riscv64
```

常用变量：

| 变量 | 说明 |
|---|---|
| `QEMU_TARGET_LIST` | 覆盖 target 列表 |
| `QEMU_CONFIGURE_FLAGS` | 额外 configure 参数，默认 `--disable-werror` |
| `QEMU_DEBUG_BUILD=1` | 增加 `--enable-debug` |
| `QEMU_FORCE_CONFIGURE=1` | 强制重新 configure |

## 4. `build_linux_6_12_riscv.sh`

用途：通用 Linux 6.12 riscv32/riscv64 构建入口。

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/build_linux_6_12_riscv.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/build_linux_6_12_riscv.sh
```

wrapper：

```bash
tools/virt_llm/build_linux_6_12_rv32.sh
tools/virt_llm/build_linux_6_12_rv64.sh
```

输出：

```text
$LINUX_BUILD/arch/riscv/boot/Image
```

## 5. `build_initramfs.sh`

用途：构建 freestanding initramfs。

模式：

| mode | init 程序 | 用途 |
|---|---|---|
| `test` | 内联 assembly init | boot marker 和 driver probe selftest |
| `console` | `virt-llm-console.c` | 手动 console |
| `qwen` | `virt-llm-qwen.c` | Qwen exact-match runtime |

示例：

```bash
tools/virt_llm/build_initramfs.sh --mode test --out /tmp/initramfs-test.cpio
tools/virt_llm/build_initramfs.sh --mode console --out /tmp/initramfs-console.cpio
tools/virt_llm/build_initramfs.sh --mode qwen --out /tmp/initramfs-qwen.cpio
```

`VIRT_LLM_GUEST_BITS=64` 时，initramfs 会使用 rv64/lp64 编译参数。

## 6. `run_virt_llm_validation.sh`

用途：启动 QEMU + Linux，运行基础 gate。

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/run_virt_llm_validation.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/run_virt_llm_validation.sh
```

成功标记：

```text
probe ok:
gemm ok:
attention q16 ok:
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv64
```

可选变量：

| 变量 | 说明 |
|---|---|
| `INITRD` | 指定 initramfs |
| `LOG` | 指定日志路径 |
| `VIRT_LLM_BOOT_TIMEOUT` | boot timeout，默认 `90s` |

## 7. `run_virt_llm_qwen.sh`

用途：运行 Qwen2.5-0.5B Instruct 8-token exact-match。

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

成功标记：

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ... output_tokens=785,6722,315,9625,374,12095,13,151645
```

可选变量：

| 变量 | 说明 |
|---|---|
| `VIRT_LLM_QWEN_EXPECTED_TOKENS` | 覆盖 expected token list |
| `LOG` | 指定日志路径 |
| `INITRD` | 指定 qwen initramfs |

## 8. `run_virt_llm_console.sh`

用途：启动手动 console runtime。

典型命令：

```bash
tools/virt_llm/run_virt_llm_console.sh
```

进入 QEMU 串口后可手动运行 console 中实现的命令。该路径用于人工验证，不替代自动 gate。

## 9. `rebuild_and_validate_virt_llm.sh`

用途：一键构建 QEMU、构建 Linux rv32/rv64，并运行基础 validation。

```bash
tools/virt_llm/rebuild_and_validate_virt_llm.sh
```

常用变量：

| 变量 | 说明 |
|---|---|
| `VIRT_LLM_GUEST_BITS_LIST` | 默认 `32 64` |
| `VIRT_LLM_RUN_VALIDATION` | 是否运行基础 validation，默认 `1` |
| `VIRT_LLM_RUN_QWEN` | 是否追加 Qwen validation，默认 `0` |
| `VIRT_LLM_QWEN_BITS` | Qwen validation 使用的 guest bits，默认 `32` |

## 10. `reproduce_fresh_virt_llm_platform.sh`

用途：从 GitHub 或内网镜像 fresh clone QEMU/Linux 到 `/tmp`，重新构建并验证。

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

可替换仓库：

```bash
QEMU_REPO=https://your-intranet/qemu.git \
LINUX_REPO=https://your-intranet/linux.git \
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

输出：

```text
$REPORT_DIR/summary.md
$REPORT_DIR/qemu-build.log
$REPORT_DIR/linux-build-rv32.log
$REPORT_DIR/linux-build-rv64.log
$REPORT_DIR/validation-rv32.log
$REPORT_DIR/validation-rv64.log
$REPORT_DIR/qwen-validation.log
```
