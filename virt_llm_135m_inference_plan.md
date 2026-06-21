# virt-llm Local Instruct Model Bring-up

## Target

The current real inference target is the local model snapshot at
`/home/zyz/llmsim/models/qwen2.5-0.5b-instruct`.

This is Qwen2.5-0.5B-Instruct with:

- model type: `qwen2`;
- hidden size: 896;
- layers: 24;
- attention heads: 14;
- KV heads: 2;
- intermediate size: 4864;
- vocab size: 151936;
- weight file size: about 943 MB.

The bring-up path starts with host-side reference generation, then adds a guest
userspace ABI, model loading, transformer operators, KV cache, and finally an
end-to-end greedy decode loop.

The initial functional target is correctness and observability, not performance.

## Milestone M1: Reference and Manifest

Status: implemented by `tools/virt_llm/model_reference.py`.

The reference tool:

- loads a local Hugging Face model snapshot through Transformers;
- records model config fields needed by the converter/runtime;
- runs deterministic greedy decode for fixed short prompts;
- writes:
  - `<artifact-prefix>_manifest.json`;
  - `<artifact-prefix>_golden.json`.

Example:

```bash
python3 -m venv /home/qemu/virt-llm-ref-venv
/home/qemu/virt-llm-ref-venv/bin/pip install -r tools/virt_llm/requirements-reference.txt
/home/qemu/virt-llm-ref-venv/bin/python tools/virt_llm/model_reference.py \
  --model-id /home/zyz/llmsim/models/qwen2.5-0.5b-instruct \
  --local-files-only \
  --out-dir /home/qemu/virt-llm-artifacts/qwen2.5-0.5b-instruct \
  --artifact-prefix qwen2_5_0_5b
```

Current Qwen reference result:

```text
PROMPT: What is the capital of France?
NEW_TOKEN_IDS: [785, 6722, 315, 9625, 374, 12095, 13, 151645]
NEW_TEXT: 'The capital of France is Paris.<|im_end|>'
PROMPT: Write one short sentence about RISC-V.
NEW_TOKEN_IDS: [49, 27629, 19625, 374, 458, 1787, 30774, 17646]
NEW_TEXT: 'RISC-V is an open-source architecture'
```

The generated fixture files are checked in under
`tests/fixtures/virt_llm/qwen2_5_0_5b`.

## Remaining Milestones

1. Add host-side weight conversion and guest-side model loading.
2. Add Q8/Q16 transformer operators.
3. Add KV cache and greedy decode loop.
4. Run end-to-end SmolLM-135M-Instruct prompts in the guest.
5. Add async execution, command graphs, tracing, and optional accelerated
   backends.

## Milestone M2: Guest Userspace Runtime Interface

Status: implemented in the Linux 6.12 driver.

The driver now exposes `/dev/virt_llm0` as a misc character device with:

- `GET_INFO`;
- `ALLOC_BUFFER`;
- `FREE_BUFFER`;
- `SUBMIT_DESC`;
- `WAIT_CQ`;
- coherent DMA buffer mmap by handle.

The probe selftests remain enabled by default and can be disabled with the
`run_selftest` module parameter. Before registering `/dev/virt_llm0`, the driver
resets and re-enables SQ/CQ so userspace starts from a clean queue state.

The guest smoke test is a freestanding riscv32 `/init` program at
`tools/testing/selftests/virt_llm/virt-llm-test.c` in the Linux tree. It creates
the device node, allocates/mmap's DMA buffers, runs `GET_INFO`, submits
`GEMM_U32`, submits `ATTENTION_Q16`, validates CQ entries and output data, then
prints `INITRAMFS_OK`.

Validation result:

```text
virt-llm-test info ok: magic=0x4c4c4d31 version=3
virt-llm-test gemm ok: checksum=0x00000086
virt-llm-test attention ok: checksum=0x0003f6ef
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```
