# virt-llm Project Plan

Last updated: 2026-06-08

## 1. Current Baseline

The current baseline has:

- QEMU `virt-llm` PCIe accelerator model.
- Linux 6.12 `virt_llm_pci` driver and guest selftests.
- Submit/completion queues, DMA buffers, INTx/MSI/MSI-X support.
- Scalar dispatcher and kernel metadata validation.
- Vector and tensor backends for toy and Qwen-relevant operations.
- Qwen2.5-0.5B Instruct per-op FP32 path for a fixed prompt.
- 8-token exact-match output for:

```text
Prompt: What is the capital of France?
Output tokens: 785,6722,315,9625,374,12095,13,151645
Decoded text: The capital of France is Paris.<|im_end|>
```

- riscv32 and riscv64 QEMU/Linux platform validation.
- fresh-clone reproduction script for external machines or intranet mirrors.

## 2. Baseline Gates

These gates must remain stable while future work proceeds.

Basic platform:

```text
probe ok:
gemm ok:
attention q16 ok:
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv64
```

Qwen exact-match:

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ... output_tokens=785,6722,315,9625,374,12095,13,151645
```

Fresh clone:

```text
Conclusion: PASS
```

## 3. Milestones

### M0: Baseline Protection

Status: completed for the current baseline.

Goals:

- Keep QEMU and Linux commits pinned in reports.
- Preserve the 8-token Qwen exact-match gate.
- Preserve rv32 and rv64 basic validation.
- Preserve fresh-clone reproduction.

Acceptance:

- QEMU build passes.
- Linux rv32/rv64 builds pass.
- rv32/rv64 validation passes.
- Qwen exact-match passes.
- fresh-clone reproduction passes.

### M1: Per-Layer Golden Debug Framework

Goal:

Add a diagnostic framework that identifies the first layer/op where QEMU output
diverges from a host reference.

Key changes:

- Host reference exports embedding, per-layer attention residual, MLP residual,
  final norm, and LM-head checksum.
- Checksum format includes shape, sum, abs_sum, min, max, and optional top-k.
- Guest runtime can emit checksums behind a debug flag.
- QEMU can emit op-level checksums behind a debug flag.
- validation script can compare checksums and report the first divergence.

Acceptance:

- 8-token exact-match remains passing.
- per-layer compare mode reports matching checksums for the baseline prompt.
- an intentional injected mismatch is reported with layer/op information.

### M2: KV Cache and Prefill/Decode Split

Goal:

Stop recomputing the full context on every generated token. Add a real prefill
and decode split with KV cache.

Key changes:

- Define KV cache layout:

```text
[layer][k_or_v][seq][kv_heads][head_dim]
```

- Add cache write/read fields or reuse tensor request offsets.
- Qwen GQA attention reads historical K/V during decode.
- Runtime supports:
  - prefill prompt tokens;
  - decode one new token per step;
  - batch=1;
  - max context <= 64;
  - max new tokens <= 8.

Acceptance:

- KV decode 8-token output equals full-context 8-token output.
- Qwen validation emits `QWEN_KV_DECODE_OK`.
- basic rv32/rv64 gates remain passing.

### M3: Device Memory Arena and SRAM Scratchpad

Goal:

Move from ad hoc QEMU heap plus guest DMA buffers toward a clearer device memory
model.

Key changes:

- Add model memory, activation arena, and KV cache arena.
- Add configurable capacities and alignment.
- Add arena query API.
- Add overflow/capacity errors.
- Keep old DMA buffer path for compatibility.

Acceptance:

- Qwen KV decode still passes.
- capacity-negative tests return clear errors.
- GEMM/attention selftests remain passing.

### M4: Async Command Worker and Queue Backpressure

Goal:

Make command execution closer to a real accelerator by moving away from purely
synchronous MMIO execution.

Key changes:

- QEMU worker or bottom half processes queue kicks.
- Add backend busy state.
- Add queue occupancy and backpressure.
- Add interrupt coalescing option.
- Add tracepoints for submit, op start/end, DMA read/write, CQ write, and IRQ.
- Runtime supports multiple completions in `WAIT_CQ`.

Acceptance:

- Qwen tokens do not change.
- logs include command counts and per-op latency.
- queue overflow/backpressure negative test passes.

### M5: Data Type Expansion

Goal:

Preserve the FP32 correctness baseline while adding inference-realistic data
types.

Key changes:

- Add BF16 runtime path or Q8 weights plus Q16 activations.
- Host converter/reference emits quantized golden data.
- Tensor backend supports quantized GEMM and LM head.
- Runtime can select dtype mode:
  - `fp32`;
  - `bf16`;
  - `q8_q16`.

Acceptance:

- FP32 exact-match remains passing.
- quantized path matches per-layer checksum thresholds.
- final token divergence, if any, reports logits top-k and first divergence.

### M6: CI and Intranet Reproduction

Goal:

Make the project easy to reproduce on another machine, including a company
intranet machine.

Key changes:

- Add CI/fresh-clone smoke.
- Keep basic gate mandatory.
- Run Qwen gate only when model path is provided.
- Document package dependencies and internal mirror variables.
- Add release checklist.

Acceptance:

- New machine can reproduce with repo URLs, branch names, toolchain, and optional
  model path.
- Missing model path gives basic PASS plus Qwen SKIP.
- Provided model path gives Qwen PASS.

## 4. Multi-Agent Execution Policy

All milestones use the process in:

```text
virt_llm_multi_agent_development_en.md
virt_llm_multi_agent_development_zh.md
```

Minimum role split:

- Coordinator;
- QEMU Backend Agent;
- Linux Driver/Runtime Agent;
- Reference Agent;
- Validation Agent;
- Docs Agent.

The coordinator is responsible for final integration and must not mark a
milestone complete until all required gates pass.

## 5. Standard Test Commands

```bash
cd /home/qemu/qemu
tools/virt_llm/build_qemu_virt_llm.sh
```

```bash
cd /home/qemu/qemu
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/run_virt_llm_validation.sh
```

```bash
cd /home/qemu/qemu
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/run_virt_llm_validation.sh
```

```bash
cd /home/qemu/qemu
VIRT_LLM_MODEL_PATH=/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

```bash
cd /home/qemu/qemu
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```
