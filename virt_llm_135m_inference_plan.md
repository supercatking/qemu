# virt-llm 135M Instruct Inference Bring-up

## Target

The first real inference target is `HuggingFaceTB/SmolLM-135M-Instruct`.
The bring-up path starts with host-side reference generation, then adds a guest
userspace ABI, model loading, transformer operators, KV cache, and finally an
end-to-end greedy decode loop.

The initial functional target is correctness and observability, not performance.

## Milestone M1: Reference and Manifest

Status: implemented by `tools/virt_llm/smollm135_reference.py`.

The reference tool:

- loads `HuggingFaceTB/SmolLM-135M-Instruct` through Transformers;
- records model config fields needed by the converter/runtime;
- runs deterministic greedy decode for fixed short prompts;
- writes:
  - `smollm135_manifest.json`;
  - `smollm135_golden.json`.

Example:

```bash
python3 -m venv /home/qemu/virt-llm-ref-venv
/home/qemu/virt-llm-ref-venv/bin/pip install -r tools/virt_llm/requirements-smollm135.txt
/home/qemu/virt-llm-ref-venv/bin/python tools/virt_llm/smollm135_reference.py
```

If the model snapshot has already been downloaded or copied locally, pass the
local path as `--model-id`:

```bash
/home/qemu/virt-llm-ref-venv/bin/python tools/virt_llm/smollm135_reference.py \
  --model-id /home/qemu/models/smollm135 \
  --local-files-only
```

When only the config is available, `--manifest-only` writes the config-derived
manifest without loading the model weights.

Current environment note: WSL HTTPS access to Hugging Face/GitHub timed out
during bring-up, so generating golden token fixtures requires either restoring
WSL outbound network access or placing a local SmolLM snapshot under
`/home/qemu/models/smollm135`.

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
