# virt-llm 排障手册

更新时间：2026-06-08

## 1. 快速定位原则

先确定失败属于哪一层：

1. QEMU 构建失败。
2. Linux 构建失败。
3. guest boot 失败。
4. PCI probe/driver 失败。
5. DMA/queue/CQ/IRQ 失败。
6. GEMM/attention 基础算子失败。
7. Qwen model load 或 token exact-match 失败。
8. fresh clone 复现失败。

## 2. 必查日志

```text
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv32-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv64-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-qwen.log
/tmp/virt-llm-platform-repro/summary.md
/tmp/virt-llm-platform-repro/validation-rv32.log
/tmp/virt-llm-platform-repro/validation-rv64.log
/tmp/virt-llm-platform-repro/qwen-validation.log
```

## 3. QEMU 构建失败

优先检查：

```bash
cd /home/qemu/qemu
tools/virt_llm/build_qemu_virt_llm.sh
```

常见原因：

| 现象 | 处理 |
|---|---|
| 缺 `ninja`/`python3`/`make` | 安装基础构建工具 |
| 缺 glib/pixman | 安装 QEMU 标准构建依赖 |
| `build dir already exists` | 设置 `QEMU_FORCE_CONFIGURE=1` 或清理 build 目录 |
| `qemu-system-riscv64` 不存在 | 确认 `QEMU_TARGET_LIST` 包含 `riscv64-softmmu` |

## 4. Linux 构建失败

命令：

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/build_linux_6_12_riscv.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/build_linux_6_12_riscv.sh
```

常见原因：

| 现象 | 处理 |
|---|---|
| 缺 `riscv64-linux-gnu-gcc` | 安装 toolchain 或设置 `CROSS_COMPILE` |
| 缺 `bc`/`bison`/`flex`/openssl headers | 安装 Linux 构建依赖 |
| driver 没有 probe | 检查 `.config` 中 `CONFIG_VIRT_LLM_PCI=y` |

## 5. guest boot 或 probe 失败

必须看到：

```text
Linux version 6.12.0
virt_llm_pci ... probe ok:
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32/riscv64
```

如果缺失：

| 缺失项 | 优先检查 |
|---|---|
| `Linux version` | `LINUX_IMAGE` 是否正确 |
| `probe ok` | QEMU 是否带 `-device virt-llm`，kernel 是否启用 driver |
| `INITRAMFS_OK` | initramfs 是否按正确 guest bits 构建 |
| QEMU timeout | 增大 `VIRT_LLM_BOOT_TIMEOUT`，检查 kernel panic |

## 6. DMA/queue/CQ/IRQ 失败

基础 gate 应输出：

```text
dma inference ok
dma copy ok
gemm ok
attention q16 ok
```

排查方向：

- submit queue base、head、tail；
- completion queue tail；
- descriptor status；
- queue error；
- DMA buffer 地址和长度；
- INTx/MSI/MSI-X 选择；
- riscv32/riscv64 结构体布局。

## 7. Qwen 失败

运行：

```bash
VIRT_LLM_MODEL_PATH=/path/to/model.safetensors tools/virt_llm/run_virt_llm_qwen.sh
```

常见原因：

| 现象 | 处理 |
|---|---|
| `missing VIRT_LLM_MODEL_PATH` | 设置模型 safetensors 路径 |
| `qwen model load ok` 缺失 | 检查模型路径、QEMU device property `model-path` |
| token mismatch | 检查 RoPE、RMSNorm eps、attention mask、LM head logits 位置 |
| 运行很慢 | 当前是 FP32 host CPU backend，属于预期 |

## 8. fresh clone 失败

命令：

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

常见原因：

| 现象 | 处理 |
|---|---|
| GitHub clone timeout | 脚本会 retry；内网机器建议设置 `QEMU_REPO`/`LINUX_REPO` |
| Qwen SKIP | 未设置 `VIRT_LLM_MODEL_PATH`，基础 gate 不受影响 |
| fresh clone 依赖 `/home/qemu` | 这是 bug，应修脚本，不能作为通过 |
