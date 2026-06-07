# virt-llm 后续项目计划：从 1-token exact-match 到可扩展 LLM 加速卡模拟

生成时间：2026-06-07

## Summary

当前 frozen baseline 已完成：

- QEMU commit：`a35aad1e4ecd212faee098ebde36e17a7c765f00`
- Linux commit：`8bbf85ee08e702d5a20bcbbe31d630d6636a62a6`
- 模型：Qwen2.5-0.5B Instruct，本地 safetensors
- prompt：`What is the capital of France?`
- 1-token golden：`785`
- M1 8-token golden：`785,6722,315,9625,374,12095,13,151645`
- M1 decoded text：`The capital of France is Paris.<|im_end|>`

项目后续按三个优先级推进：

1. P0：推理正确性扩展。先把 1-token exact-match 扩到 8-token full-context decode，并建立 per-layer golden。
2. P1：LLM runtime 和硬件真实性。加入 KV cache、prefill/decode、device memory、SRAM/arena。
3. P2：性能、数据类型和工程化。加入异步 worker、latency/trace、量化、CI/fresh clone gate。

执行方式采用主协调者 + 多 agents。每个 milestone 的写入范围必须拆开，所有阶段必须保留基础 validation 和 Qwen exact-match gate。

## Milestones

### M0：基线保护与项目计划落地

目标：把当前基线固定成后续开发的安全网。

交付：

- `virt_llm_project_plan_zh.md`
- 当前状态报告和架构报告入库保存
- 明确 1-token 和 8-token golden
- 明确多 agent 执行规则和验收命令

验收：

- 基础 validation 通过
- Qwen 1-token exact-match 通过
- fresh clone 脚本在设置 `VIRT_LLM_MODEL_PATH` 后能跑 Qwen exact-match

### M1：从 1-token 推进到 8-token full-context decode

目标：固定 prompt，`max_new_tokens=8`，仍使用 full-context recompute，不引入 KV cache。

交付：

- host reference 能生成 8-token golden
- golden JSON 包含每步 top-k logits
- Linux guest runtime 检查完整 8-token 序列
- Qwen validation gate 必须检查完整 token 序列

验收：

- `QWEN_INFER_OK ... output_tokens=785,6722,315,9625,374,12095,13,151645`
- 基础 validation 仍通过
- fresh clone basic gate 通过
- 设置模型路径时 fresh clone Qwen 8-token gate 通过

### M2：per-layer golden 调试框架

目标：能定位第一处数值分叉在哪一层、哪个 op。

交付：

- host reference 导出 embedding、每层 attention residual、MLP residual、final norm、LM head logits checksum
- QEMU/guest runtime 增加可控 debug flag
- validation 增加 per-layer compare 模式

验收：

- 能输出每层关键 tensor checksum
- 8-token exact-match 继续通过
- 故意改错一个 op 时，validation 能报告第一处分叉位置

### M3：KV cache 与 prefill/decode 分离

目标：decode 阶段不再每步 full-context recompute。

交付：

- 定义 KV cache tensor ABI 或复用 `virt_llm_tensor_req`
- 明确 KV layout：`[layer][kv][seq][kv_heads][head_dim]`
- `QWEN_GQA_ATTENTION_F32` 支持读取历史 K/V
- guest runtime 分 prefill 和 decode

验收：

- 8-token KV decode 与 full-context 8-token 输出完全一致
- Qwen validation 输出 `QWEN_KV_DECODE_OK`
- 基础 validation 和 full-context gate 不回退

### M4：device memory model、activation arena、SRAM scratchpad

目标：从“QEMU heap + guest DMA buffer”过渡到更真实的 device-owned memory 抽象。

交付：

- model memory、activation arena、KV cache arena
- capacity、alignment、overflow/capacity error
- arena query UAPI
- Qwen activation 使用 arena-based handle

验收：

- Qwen 8-token KV decode 通过
- capacity 不足时返回明确错误
- 基础 GEMM/attention selftest 不受影响

### M5：异步 command worker、queue backpressure、trace/latency

目标：把同步 MMIO execution 改成更像硬件的异步执行模型。

交付：

- QEMU worker/bottom half 处理 command queue
- backend busy、queue occupancy、backpressure
- interrupt coalescing 选项
- tracepoints：submit、op start/end、DMA read/write、CQ write、IRQ

验收：

- Qwen decode token 不变
- 日志包含 per-op latency 和 command count
- queue backpressure 有 negative test
- INTx/MSI/MSI-X 路径不回退

### M6：量化与数据类型扩展

目标：从 FP32 correctness baseline 扩展到更接近推理加速卡的数据类型。

交付：

- BF16 runtime 或 Q8 weight + Q16 activation 路径
- quantized golden
- QEMU tensor backend 增加量化 GEMM/LM head
- runtime 支持 dtype mode：`fp32`、`bf16`、`q8_q16`

验收：

- FP32 exact-match 不回退
- quantized path 至少 per-layer checksum 在阈值内
- 若最终 token 分叉，必须输出 logits top-k 和第一处分叉证据

### M7：工程化、CI、公司内网复现

目标：让项目可被另一台机器稳定复现。

交付：

- CI/fresh clone smoke
- build dependency 文档
- 内网镜像变量说明
- release checklist

验收：

- 新机器只需设置 repo URL、branch、toolchain、model path 即可复现
- 未设置模型路径时 basic gate PASS，Qwen gate 明确 SKIP
- 设置模型路径时 Qwen exact-match PASS

## Public APIs / Interfaces

- 保留现有 ioctl ABI：`GET_INFO`、`ALLOC_BUFFER`、`FREE_BUFFER`、`SUBMIT_DESC`、`WAIT_CQ`
- M3 开始扩展 `virt_llm_tensor_req` 或新增 KV cache request，必须保持旧 runtime/selftest 可用
- M4 开始新增 arena/memory query，不删除 coherent DMA buffer path
- 成功 marker：
  - basic：`gemm ok`、`attention q16 ok`、`INITRAMFS_OK`
  - Qwen 1-token：`QWEN_INFER_OK ... output_tokens=785`
  - Qwen 8-token：`QWEN_INFER_OK ... output_tokens=785,6722,315,9625,374,12095,13,151645`
  - KV path：`QWEN_KV_DECODE_OK`
- 错误 marker 必须包含 expected/current token、step、first divergent layer/op、q_error/status/backend

## Test Plan

每个 milestone 必跑：

```bash
cd /home/qemu/qemu/build
ninja qemu-system-riscv32
```

```bash
cd /home/qemu/qemu
tools/virt_llm/build_linux_6_12_rv32.sh
```

```bash
cd /home/qemu/qemu
tools/virt_llm/run_virt_llm_validation.sh
```

```bash
cd /home/qemu/qemu
VIRT_LLM_MODEL_PATH=/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

Fresh clone gate：

```bash
cd /home/qemu/qemu
VIRT_LLM_MODEL_PATH=/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors \
REPORT_DIR=/tmp/virt-llm-fresh-repro-qwen \
  tools/virt_llm/reproduce_fresh_virt_llm.sh
```

Required scenarios：

- Basic PCI probe/selftest
- GEMM checksum
- attention q16 checksum
- Qwen exact-match
- Bad tensor id
- Bad shape
- Bad dtype
- Output buffer too small
- Queue overflow/backpressure
- Model path missing
- Fresh clone without model path：basic PASS + Qwen SKIP
- Fresh clone with model path：basic PASS + Qwen PASS

## Multi-Agent Execution Policy

- 主协调者负责拆分、集成、最终 build/test、commit/push。
- 每个 agent 必须有独立写入范围：
  - Reference agent：host golden、converter、fixtures
  - QEMU backend agent：`virt_llm.c` backend/dispatch/memory model
  - Linux runtime agent：driver/UAPI/runtime
  - Validation agent：run scripts、fresh repro、gate logic
  - Docs agent：Markdown/report/release checklist
- agents 不允许互相 revert。
- 每个 milestone 至少一个本地 commit；通过 gate 后 push：
  - QEMU -> `supercatking/qemu:llmdev`
  - Linux -> `supercatking/linux:llmdev-linux-6.12`
- exact-match 未达成时不能标记完成，只能报告 blocked，必须给出第一处分叉证据。

## Assumptions

- 目标模型固定为本地 Qwen2.5-0.5B Instruct safetensors。
- 第一阶段继续以 FP32 correctness 为主。
- tokenizer 暂不作为硬件 op；M1/M2 使用 host-generated token fixture，后续再做 guest CLI。
- `VIRT_LLM_MODEL_PATH` 是模型路径的标准输入方式。
- CUDA/RTX backend 不是 correctness baseline，后续只作为性能可选项。
- 当前未跟踪 build 输出目录不纳入提交。
