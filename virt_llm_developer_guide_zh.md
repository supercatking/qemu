# virt-llm 开发者指南

更新时间：2026-06-08

## 1. 目标

本文档面向要继续开发 `virt-llm` 的工程师，说明代码结构、常见修改流程、调试入口、验证 gate 和提交规则。平台搭建请先读 `virt_llm_platform_setup_zh.md`，硬件/软件架构请先读 `virt_llm_platform_architecture_zh.md`。

## 2. 源码结构

QEMU repo：

| 路径 | 作用 |
|---|---|
| `hw/misc/virt_llm.c` | QEMU PCIe 设备模型、BAR、queue、DMA、backend、Qwen op 实现 |
| `tools/virt_llm/common.sh` | 公共环境变量、路径解析、progress 输出 |
| `tools/virt_llm/build_qemu_virt_llm.sh` | 构建 riscv32/riscv64 QEMU |
| `tools/virt_llm/build_linux_6_12_riscv.sh` | 通用 Linux 6.12 riscv32/riscv64 构建入口 |
| `tools/virt_llm/build_initramfs.sh` | 构建 test/console/qwen initramfs |
| `tools/virt_llm/run_virt_llm_validation.sh` | 基础 boot/probe/GEMM/attention gate |
| `tools/virt_llm/run_virt_llm_qwen.sh` | Qwen 8-token exact-match gate |
| `tools/virt_llm/reproduce_fresh_virt_llm_platform.sh` | fresh clone 复现 gate |
| `tools/virt_llm/model_reference.py` | host golden/reference 生成工具 |
| `virt_llm*.md` | 项目计划、架构、搭建、多 agent 和开发文档 |

Linux repo：

| 路径 | 作用 |
|---|---|
| `drivers/misc/virt_llm_pci.c` | Linux 6.12 guest driver |
| `tools/testing/selftests/virt_llm/virt-llm-test.c` | freestanding 基础测试 runtime |
| `tools/testing/selftests/virt_llm/virt-llm-console.c` | 手动 console runtime |
| `tools/testing/selftests/virt_llm/virt-llm-qwen.c` | Qwen op graph runtime |

## 3. 标准开发循环

```bash
cd /home/qemu/qemu
git status --short
tools/virt_llm/build_qemu_virt_llm.sh
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/run_virt_llm_validation.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/run_virt_llm_validation.sh
```

如果改动 Qwen、tensor backend、模型加载、runtime 或 reference：

```bash
VIRT_LLM_MODEL_PATH=/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

如果改动脚本、路径、依赖、构建方式或复现文档：

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

## 4. 如何新增 opcode

必须同时考虑 QEMU、Linux/runtime、reference、validation 和文档。

步骤：

1. 在架构文档中定义 opcode 名称、编号、输入、输出、错误语义和 backend。
2. 在 `hw/misc/virt_llm.c` 中新增 opcode constant。
3. 在 descriptor dispatch 中加入 case。
4. 实现 backend helper。
5. 写 completion status/backend/error。
6. 在 Linux driver/runtime 中提交对应 descriptor。
7. 加 known-answer test。
8. 更新中英文架构文档和开发文档。
9. 跑基础 gate 和相关专项 gate。

注意：

- opcode 不允许和既有编号冲突。
- unknown opcode 必须继续返回 `DESC_UNSUPP` 和 opcode error。
- bad tensor、bad shape、bad dtype 应该有明确错误，不要混成 unknown opcode。

## 5. 如何修改 ABI

ABI 包括：

- BAR register offset；
- feature bit；
- descriptor field；
- completion entry field；
- queue status/error；
- tensor request layout；
- ioctl contract；
- success/failure marker。

ABI 修改规则：

1. 先更新 `virt_llm_platform_architecture_zh.md` 和 `virt_llm_platform_architecture_en.md`。
2. 由 ABI/Docs Steward 确认可见行为。
3. QEMU 和 Linux 必须成对修改。
4. 保留旧路径兼容性，除非 milestone 明确允许破坏兼容。
5. 更新 negative tests。

## 6. DMA 和内存调试

当前多数 op 的数据路径是：

```text
guest DMA buffer -> pci_dma_read -> QEMU host heap -> backend compute -> pci_dma_write -> guest DMA buffer
```

调试重点：

- descriptor 的 guest physical address 是否正确；
- input/output buffer 长度是否足够；
- QEMU 中 host heap copy 是否越界；
- CQ entry 的 status/backend/error 是否正确；
- Linux runtime 读取的是 output buffer 还是旧 input buffer；
- riscv32/riscv64 下结构体布局是否一致。

## 7. Qwen 开发规则

当前 Qwen correctness baseline：

```text
Prompt: What is the capital of France?
Output tokens: 785,6722,315,9625,374,12095,13,151645
```

开发 Qwen 相关功能时：

1. FP32 exact-match 是 correctness baseline。
2. tokenizer 暂不作为硬件 op。
3. `VIRT_LLM_MODEL_PATH` 是模型文件输入。
4. 修改 RoPE、RMSNorm、attention mask、GEMM、LM head、argmax 时必须跑 Qwen gate。
5. 如果 token 分叉，必须输出 expected/current token、step、第一处分叉 layer/op。

## 8. 日志和故障定位

基础日志：

```text
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv32-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv64-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-qwen.log
```

fresh clone 日志：

```text
/tmp/virt-llm-platform-repro/summary.md
/tmp/virt-llm-platform-repro/validation-rv32.log
/tmp/virt-llm-platform-repro/validation-rv64.log
/tmp/virt-llm-platform-repro/qwen-validation.log
```

常见失败：

| 现象 | 优先检查 |
|---|---|
| QEMU 找不到 `virt-llm` | 是否构建了正确 target，设备是否接入 `hw/misc` |
| Linux 没有 probe | kernel 是否启用 `CONFIG_VIRT_LLM_PCI=y` |
| DMA checksum 错 | descriptor 地址、buffer 长度、copy 方向 |
| CQ 无 completion | queue tail/head、doorbell、中断路径 |
| Qwen token 分叉 | RoPE、RMSNorm eps、attention mask、logits 位置 |
| fresh clone 失败 | repo URL、toolchain、QEMU build deps、模型路径 |

## 9. 提交前检查

```bash
cd /home/qemu/qemu
git status --short
git diff --check
```

必须排除：

- build 输出；
- log；
- model 文件；
- Python cache；
- 临时 patch；
- 既有未跟踪 `build-riscv64-user/`。

## 10. Push 规则

QEMU：

```bash
git push supercatking HEAD:llmdev
```

Linux：

```bash
cd /home/qemu/linux-6.12
git push supercatking HEAD:llmdev-linux-6.12
```

push 后必须核对：

```bash
git ls-remote supercatking refs/heads/llmdev
git rev-parse HEAD
```

Linux 分支同理。
