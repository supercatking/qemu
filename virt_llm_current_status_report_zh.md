# virt-llm 当前进展与验证报告

生成时间：2026-06-07

## 1. 报告结论

当前 `virt-llm` 已经从基础 PCIe toy device 演进到可以在 QEMU 设备内执行
Qwen2.5-0.5B Instruct 最小真实推理的阶段。这里的“真实推理”定义为：

- guest 侧 Linux/RISC-V runtime 通过 `/dev/virt_llm0` 提交逐算子 op graph；
- QEMU `virt-llm` 设备加载本地 Qwen safetensors 权重；
- QEMU 设备按算子执行 embedding、RMSNorm、GEMM、RoPE、GQA attention、SwiGLU、residual add、LM head、argmax；
- 固定 prompt、batch=1、full-context recompute、greedy、生成 1 个 token；
- guest/QEMU 输出 token 与 host Transformers golden exact match。

最新验证结果：PASS。固定 prompt：

```text
What is the capital of France?
```

host Transformers golden 的 first generated token 为：

```text
token_id = 785
token_text = "The"
```

guest/QEMU `virt-llm` 输出：

```text
QWEN_INFER_OK ... output_tokens=785
```

这说明当前版本已经跑通了一个最小闭环：host golden -> guest token fixture ->
PCIe command queue -> QEMU per-op backend -> completion queue -> token exact-match。

## 2. 当前 GitHub 状态

### QEMU 仓库

- 仓库：[https://github.com/supercatking/qemu](https://github.com/supercatking/qemu)
- GitHub 分支：`llmdev`
- 本地分支：`llmdev-push`
- 本地目录：`/home/qemu/qemu`
- 当前 commit：

```text
a35aad1e4ecd212faee098ebde36e17a7c765f00
virt-llm: validate minimal qwen inference
```

远端 `supercatking/llmdev` 已确认指向同一 commit。

### Linux 仓库

- 仓库：[https://github.com/supercatking/linux](https://github.com/supercatking/linux)
- GitHub 分支：`llmdev-linux-6.12`
- 本地分支：`llmdev-linux-6.12-push`
- 本地目录：`/home/qemu/linux-6.12`
- 当前 commit：

```text
8bbf85ee08e702d5a20bcbbe31d630d6636a62a6
selftests: validate qwen virt-llm inference fixture
```

远端 `supercatking/llmdev-linux-6.12` 已确认指向同一 commit。

### 工作树状态

QEMU 工作树当前还有一个未跟踪目录：

```text
/home/qemu/qemu/build-riscv64-user/
```

它是既有 build 输出目录，不属于本次提交内容。Linux 工作树当前无未提交源码改动。

## 3. 已完成能力

### 3.1 基础 PCIe 设备与 Linux driver

当前系统具备：

- QEMU PCIe endpoint：`virt-llm`。
- BAR0 4 KiB MMIO register space。
- BAR1 MSI-X table/PBA space。
- Linux 6.12 RISC-V 32-bit guest driver。
- probe selftest。
- coherent DMA buffer 分配。
- submit queue/completion queue。
- INTx/MSI/MSI-X completion notification。
- userspace misc char device：`/dev/virt_llm0`。
- userspace ioctl ABI：
  - `GET_INFO`
  - `ALLOC_BUFFER`
  - `FREE_BUFFER`
  - `SUBMIT_DESC`
  - `WAIT_CQ`

关键 UAPI 定义位于：

- `/home/qemu/linux-6.12/include/uapi/linux/virt_llm.h`

关键 driver 位于：

- `/home/qemu/linux-6.12/drivers/misc/virt_llm_pci.c`

### 3.2 基础算子

已经支持并验证：

- `INFER_XOR`
- `DMA_COPY`
- `VEC_ADD_U32`
- `DOT_U32`
- `SOFTMAX_Q16`
- `POOL_MAX_U32`
- `GEMM_U32`
- `CONV2D_U32`
- `ATTENTION_Q16`

基础 validation 日志包含：

```text
gemm ok: m=2 n=2 k=2 checksum=0x00000086
attention q16 ok: seq=3 head_dim=2 checksum=0x0003f6ef
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

日志路径：

```text
/home/qemu/qemu/build/virt-llm-artifacts/logs/virt-llm-riscv32-linux-6.12.log
```

该日志最近修改时间为 `2026-06-07 11:06:37 +0800`。日志中同时包含
`probe ok: magic=0x4c4c4d31 version=3 abi=1`，说明 guest driver 已完成
PCI probe、BAR 配置、SQ/CQ 配置、IRQ 和基础 selftest。

### 3.3 Qwen2.5-0.5B per-op 推理路径

当前支持 Qwen2.5-0.5B Instruct 的最小 per-op 推理路径，新增/使用的 FP32 op 包括：

- `MODEL_LOAD`
- `MODEL_QUERY`
- `EMBED_LOOKUP_F32`
- `RMSNORM_F32`
- `ROPE_F32`
- `GEMM_F32`
- `ADD_F32`
- `SWIGLU_F32`
- `QWEN_GQA_ATTENTION_F32`
- `LM_HEAD_F32`
- `ARGMAX_F32`

Qwen 模型参数：

- layers：24
- hidden size：896
- heads：14
- kv heads：2
- head dim：64
- intermediate size：4864
- vocab size：151936
- rope theta：1000000
- dtype：safetensors 内 BF16，QEMU runtime 转 FP32 执行

Qwen guest runtime 位于：

```text
/home/qemu/linux-6.12/tools/testing/selftests/virt_llm/virt-llm-qwen.c
```

Qwen exact-match validation 日志路径：

```text
/home/qemu/qemu/build/virt-llm-artifacts/logs/virt-llm-qwen.log
```

该日志最近修改时间为 `2026-06-07 11:07:02 +0800`。

关键输出：

```text
qwen model load ok
qwen fixture prompt=What is the capital of France?
qwen full layers ok
qwen decode step token=785
QWEN_INFER_OK input_tokens=151644,8948,198,2610,525,1207,16948,11,3465,553,54364,14817,13,1446,525,264,10950,17847,13,151645,198,151644,872,198,3838,374,279,6722,315,9625,30,151645,198,151644,77091,198 output_tokens=785
```

## 4. Host Golden

host golden 使用 `tools/virt_llm/model_reference.py` 生成。

运行命令：

```bash
cd /home/qemu/qemu
/home/qemu/virt-llm-ref-venv/bin/python tools/virt_llm/model_reference.py \
  --model-id /home/zyz/llmsim/models/qwen2.5-0.5b-instruct \
  --out-dir /tmp/virt-llm-qwen-golden-main \
  --artifact-prefix qwen_minimal \
  --max-new-tokens 1 \
  --top-k 5 \
  --device cpu \
  --dtype float32 \
  --local-files-only
```

输出摘要：

```text
INPUT_IDS: [151644, 8948, 198, 2610, 525, 1207, 16948, 11, 3465, 553, 54364, 14817, 13, 1446, 525, 264, 10950, 17847, 13, 151645, 198, 151644, 872, 198, 3838, 374, 279, 6722, 315, 9625, 30, 151645, 198, 151644, 77091, 198]
NEW_TOKEN_IDS: [785]
EXPECTED_FIRST_NEW_TOKEN: 785
NEW_TEXT: 'The'
```

top-k logits：

| rank | token id | text | logit |
|---:|---:|---|---:|
| 1 | 785 | `The` | 21.527870178222656 |
| 2 | 59604 | `Paris` | 18.729646682739258 |
| 3 | 2121 | `As` | 18.1833553314209 |
| 4 | 40 | `I` | 17.33087158203125 |
| 5 | 49000 | `France` | 16.710844039916992 |

## 5. 构建与验证命令

### 5.1 QEMU build

```bash
cd /home/qemu/qemu/build
ninja qemu-system-riscv32
```

结果：PASS。

### 5.2 Linux 6.12 riscv32 Image build

```bash
cd /home/qemu/qemu
tools/virt_llm/build_linux_6_12_rv32.sh
```

结果：PASS。输出 Image：

```text
/home/qemu/linux-6.12-build-rv32/arch/riscv/boot/Image
```

### 5.3 基础 validation

```bash
cd /home/qemu/qemu
tools/virt_llm/run_virt_llm_validation.sh
```

结果：PASS。关键 marker：

```text
gemm ok
attention q16 ok
INITRAMFS_OK
```

### 5.4 Qwen exact-match validation

```bash
cd /home/qemu/qemu
VIRT_LLM_MODEL_PATH=/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

结果：PASS。关键 marker：

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ... output_tokens=785
```

## 6. Fresh Clone 复现结果

为了确认项目不依赖当前 `/home/qemu/qemu` 和 `/home/qemu/linux-6.12` 工作树，已从 GitHub fresh clone 到 `/tmp` 后重新编译验证。

运行命令：

```bash
cd /home/qemu/qemu
VIRT_LLM_MODEL_PATH=/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors \
REPORT_DIR=/tmp/virt-llm-fresh-repro-qwen \
  tools/virt_llm/reproduce_fresh_virt_llm.sh
```

fresh clone 使用：

- QEMU：`https://github.com/supercatking/qemu.git`，branch `llmdev`
- Linux：`https://github.com/supercatking/linux.git`，branch `llmdev-linux-6.12`

fresh clone 结果：PASS。

summary 路径：

```text
/tmp/virt-llm-fresh-repro-qwen/summary.md
```

summary 中的关键结果：

```text
model-path=<str>: PASS
probe ok: PASS
gemm ok: PASS
attention q16 ok: PASS
INITRAMFS_OK: PASS
Qwen exact-match: PASS
```

## 7. 当前仍依赖的外部输入

虽然脚本已经尽量参数化，当前仍需要以下外部条件：

- GitHub 或内网镜像能访问 `supercatking/qemu` 和 `supercatking/linux`。
- RISC-V cross compiler：默认 `riscv64-linux-gnu-gcc`。
- QEMU 标准构建依赖：`python3`、`meson`/QEMU pyvenv、`ninja`、C compiler、glib/pixman 等。
- Linux 标准构建依赖：`make`、cross toolchain、`bc`、`flex`、`bison`、`openssl` headers 等。
- Qwen safetensors 文件不入 git，需要通过：

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors
```

或 QEMU device property：

```bash
-device virt-llm,model-path=/path/to/model.safetensors
```

## 8. 当前能力边界

当前完成的是“最小真实推理”，不是完整产品态 LLM runtime。

已经实现：

- Qwen2.5-0.5B Instruct 固定 prompt。
- guest/RISC-V runtime 提交完整 24 层 op graph。
- 生成 1 个 token。
- first token exact match host Transformers。
- full-context recompute。
- CPU FP32 backend。

尚未实现：

- 任意 prompt tokenizer/chat template CLI。
- 多 token 稳定 decode 验证。
- KV cache。
- batch > 1。
- 量化 Q8/Q16/BF16 原生执行。
- CUDA/RTX backend。
- 真正的 device-side DDR/SRAM 容量模型。
- PCIe 带宽、DMA latency、backend latency、queue backpressure 性能模型。
- kernel graph batching 或异步 worker。

## 9. 风险与注意事项

- 当前 Qwen exact-match 依赖固定 token fixture。tokenizer 不在 guest 内运行。
- 当前 QEMU op 是同步执行，`MMIO kick` 后在 QEMU 主执行路径内完成，不代表真实硬件异步行为。
- activation buffer 当前在 guest coherent DMA buffer 中，QEMU 执行时会 `pci_dma_read()` 到 host heap，计算后 `pci_dma_write()` 回 guest；不是 zero-copy。
- 模型权重在 QEMU 侧从 safetensors lazy load/转换，未模拟 guest 上传整个模型到 device memory。
- 当前 Fresh clone 复现已经验证基础路径和 Qwen exact-match，但公司内网机器仍需准备模型文件和构建依赖。

## 10. 建议的下一步

优先建议：

1. 增加 per-layer golden 对齐工具，输出每层 residual/norm/logits checksum。
2. 支持 `max_new_tokens=8`，先 full-context recompute，再上 KV cache。
3. 把 tokenizer/chat template 放到 host 工具或 guest userspace CLI，允许任意 prompt。
4. 引入可配置 activation arena 和 buffer size query，替换固定小 buffer 假设。
5. 增加异步 command worker、latency 统计和 tracepoints。
6. 规划 Q8/Q16 或 BF16 路径，降低 FP32 内存和计算成本。
7. 建立 CI/fresh clone smoke test，避免未来破坏基础 gate。
