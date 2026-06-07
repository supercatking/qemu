# virt-llm 平台搭建与复现手册

生成时间：2026-06-07

## 1. 目标

本文档面向在新主机上复现 `virt-llm` 平台。当前目标是：

1. 构建带 `virt-llm` PCIe 虚拟设备的 QEMU。
2. 同时支持 `qemu-system-riscv32` 和 `qemu-system-riscv64`。
3. 构建 Linux 6.12 riscv32/riscv64 guest Image。
4. 自动生成 initramfs，启动 guest，并验证 PCI probe、GEMM、attention 和 boot marker。
5. 可选运行 Qwen2.5-0.5B Instruct 的 8-token exact-match 验证。
6. 尽量通过环境变量消除固定路径依赖，方便迁移到公司内网机器。

## 2. 仓库与分支

推荐使用以下分支：

| 项目 | GitHub | 分支 |
|---|---|---|
| QEMU | `https://github.com/supercatking/qemu.git` | `llmdev` |
| Linux | `https://github.com/supercatking/linux.git` | `llmdev-linux-6.12` |

在公司内网环境中，可以把上述 URL 替换成内网镜像。脚本通过 `QEMU_REPO`、`LINUX_REPO`、`QEMU_BRANCH`、`LINUX_BRANCH` 覆盖。

## 3. 外部依赖

基础依赖：

- `git`
- `make`
- `ninja`
- `python3`
- `cpio`
- RISC-V 交叉编译器：默认 `riscv64-linux-gnu-gcc`
- QEMU 标准构建依赖：glib、pixman、meson/python venv 相关依赖等
- Linux 标准构建依赖：bc、bison、flex、openssl headers、elfutils headers 等

可选依赖：

- Qwen 模型文件：`qwen2.5-0.5b-instruct/model.safetensors`
- Python Transformers 环境：只用于 host golden/reference，不是基础 boot/probe/GEMM/attention gate 的必需项

## 4. 关键环境变量

| 变量 | 默认值 | 作用 |
|---|---|---|
| `QEMU_SRC` | 当前 QEMU 仓库根目录 | QEMU 源码路径 |
| `QEMU_BUILD` | `$QEMU_SRC/build` | QEMU build 目录 |
| `QEMU_TARGET_LIST` | `riscv32-softmmu,riscv64-softmmu` | QEMU system target 列表 |
| `VIRT_LLM_GUEST_BITS` | `32` | 当前 guest 位宽，取值 `32` 或 `64` |
| `VIRT_LLM_GUEST_BITS_LIST` | `32 64` | 一键构建/复现时要覆盖的 guest 位宽 |
| `LINUX_SRC` | `/home/qemu/linux-6.12` | Linux 源码路径 |
| `LINUX_BUILD` | `/home/qemu/linux-6.12-build-rv$VIRT_LLM_GUEST_BITS` | Linux build 目录 |
| `LINUX_IMAGE` | `$LINUX_BUILD/arch/riscv/boot/Image` | guest kernel Image |
| `CROSS_COMPILE` | `riscv64-linux-gnu-` | RISC-V 交叉编译器前缀 |
| `VIRT_LLM_MODEL_PATH` | 本机 Qwen 模型路径 | 可选 Qwen 验证模型文件 |
| `VIRT_LLM_LOG_DIR` | `$QEMU_BUILD/virt-llm-artifacts/logs` | QEMU 运行日志目录 |
| `JOBS` | `nproc` | 并行编译线程数 |

可以参考：

```bash
source tools/virt_llm/env.example
```

## 5. 一键构建和验证

在 QEMU 仓库内执行：

```bash
cd /home/qemu/qemu
tools/virt_llm/rebuild_and_validate_virt_llm.sh
```

默认行为：

1. 构建 QEMU `riscv32-softmmu,riscv64-softmmu`。
2. 构建 Linux 6.12 riscv32 Image。
3. 启动 `qemu-system-riscv32` 验证 `virt-llm`。
4. 构建 Linux 6.12 riscv64 Image。
5. 启动 `qemu-system-riscv64` 验证 `virt-llm`。

输出中会打印带时间戳的 progress：

```text
[2026-06-07 14:00:00] [virt-llm] >>> build QEMU virt-llm targets=...
[2026-06-07 14:02:00] [virt-llm] >>> build Linux Image guest=riscv64 jobs=32
[2026-06-07 14:04:00] [virt-llm] >>> run basic validation guest=riscv64 ...
```

## 6. 单步命令

只构建 QEMU：

```bash
tools/virt_llm/build_qemu_virt_llm.sh
```

只构建 riscv32 Linux：

```bash
tools/virt_llm/build_linux_6_12_rv32.sh
```

只构建 riscv64 Linux：

```bash
tools/virt_llm/build_linux_6_12_rv64.sh
```

验证 riscv32：

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/run_virt_llm_validation.sh
```

验证 riscv64：

```bash
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/run_virt_llm_validation.sh
```

## 7. fresh clone 复现

使用新脚本从 GitHub 或内网镜像重新 clone、构建和验证：

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

可替换仓库地址：

```bash
QEMU_REPO=https://your-intranet/qemu.git \
LINUX_REPO=https://your-intranet/linux.git \
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

如果设置 `VIRT_LLM_MODEL_PATH`，fresh clone 脚本会额外运行 Qwen exact-match：

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
REPORT_DIR=/tmp/virt-llm-platform-repro-qwen \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

## 8. 验收标记

基础 gate 必须包含：

```text
probe ok:
gemm ok:
attention q16 ok:
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv64
```

Qwen gate 必须包含：

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ... output_tokens=785,6722,315,9625,374,12095,13,151645
```

## 9. 仍然存在的外部依赖

| 依赖 | 是否必需 | 说明 |
|---|---|---|
| QEMU/Linux 源码仓库 | 必需 | 可替换为内网镜像 |
| RISC-V cross compiler | 必需 | 默认 `riscv64-linux-gnu-`，可通过 `CROSS_COMPILE` 覆盖 |
| QEMU/Linux 系统构建依赖 | 必需 | 属于标准开源构建依赖 |
| Qwen 模型文件 | 可选 | 只用于 Qwen exact-match，不影响基础 gate |
| host Transformers 环境 | 可选 | 只用于生成/更新 golden |

## 10. 当前限制

- riscv64 路径当前目标是 CPU/guest boot 与 `virt-llm` PCIe 基础验证，不等价于 Qwen runtime 已在 riscv64 上完成长期回归。
- Qwen 推理仍优先在 riscv32 gate 上保持 exact-match。
- QEMU 设备仍未模拟真实 device DDR/SRAM timing；当前 device memory 多数仍是 QEMU host heap 或 guest DMA buffer 抽象。
- CUDA/RTX 后端不是 correctness baseline。
