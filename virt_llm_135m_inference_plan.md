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

1. Add Linux guest userspace access through `/dev/virt_llm0`.
2. Add host-side weight conversion and guest-side model loading.
3. Add Q8/Q16 transformer operators.
4. Add KV cache and greedy decode loop.
5. Run end-to-end SmolLM-135M-Instruct prompts in the guest.
6. Add async execution, command graphs, tracing, and optional accelerated
   backends.
