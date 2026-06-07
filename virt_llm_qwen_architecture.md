# virt-llm Qwen per-op architecture

## Control flow

The guest runtime opens `/dev/virt_llm0`, allocates DMA buffers, fills a
`virt_llm_tensor_req`, submits a descriptor, and waits for a CQ entry.  QEMU
decodes the opcode, validates the request, executes the operation in the
selected backend, writes output DMA memory, updates descriptor status, and posts
an interrupt-backed CQ completion.

## Data flow

Tokenization is outside the device.  The device receives token ids and tensor
buffers.  For Qwen2.5-0.5B the graph is:

1. `EMBED_LOOKUP_F32`
2. For each layer:
   - `RMSNORM_F32`
   - `GEMM_F32` for Q, K, V
   - `ROPE_F32` for Q and K
   - `QWEN_GQA_ATTENTION_F32`
   - `GEMM_F32` for output projection
   - `ADD_F32`
   - `RMSNORM_F32`
   - `GEMM_F32` for gate and up
   - `SWIGLU_F32`
   - `GEMM_F32` for down projection
   - `ADD_F32`
3. `RMSNORM_F32`
4. `LM_HEAD_F32`
5. `ARGMAX_F32`

## Backend split

- DMA/model backend: `MODEL_LOAD`, `MODEL_QUERY`, simple metadata movement.
- Vector backend: embedding, RMSNorm, RoPE, residual add, SwiGLU, argmax.
- Tensor backend: GEMM, LM head, and GQA attention.

The backend value in CQ entries is used as a validation signal.  Unknown opcodes
continue to return `DESC_UNSUPP`; malformed tensor requests return
`DESC_BAD_LEN` or `DESC_BAD_TENSOR`.

## Minimal true-inference validation

The strict Qwen validation entry point is:

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
  /home/qemu/qemu/tools/virt_llm/run_virt_llm_qwen.sh
```

The script boots the RISC-V guest, passes `model-path` to the QEMU device,
loads the safetensors model, runs the Qwen per-op graph, and now requires these
markers:

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ...
```

`QWEN_SINGLE_TOKEN_OK` and `QWEN_DECODE_OK` are legacy smoke-test markers.  They
are still useful for debugging older guest runtimes, but they are not accepted
as the true-inference success condition.

Current validation limits are fixed prompt/token fixture, batch size 1,
full-context recompute, greedy generation, and no KV cache or tokenizer inside
the virtual device.  The model file is not checked into git and must be supplied
with `VIRT_LLM_MODEL_PATH` or the QEMU `model-path` device property.

## Initial Qwen constants

- layers: 24
- hidden size: 896
- attention heads: 14
- KV heads: 2
- head dimension: 64
- intermediate size: 4864
- vocab size: 151936
- RoPE theta: 1000000.0
- RMSNorm epsilon: 1e-6

The initial implementation runs FP32 tensors and treats BF16 weight conversion
as a model-loader concern.

## Tensor ids

Device-owned Qwen weights use stable tensor ids so the guest graph can refer to
model weights without DMA-uploading them every command.

- `1`: `model.embed_tokens.weight`
- `2`: `model.norm.weight`
- Per layer: `1000 + layer * 16 + slot`
- Layer slots:
  - `0`: `input_layernorm.weight`
  - `1`: `post_attention_layernorm.weight`
  - `2`: `self_attn.q_proj.weight`
  - `3`: `self_attn.k_proj.weight`
  - `4`: `self_attn.v_proj.weight`
  - `5`: `self_attn.o_proj.weight`
  - `6`: `mlp.gate_proj.weight`
  - `7`: `mlp.up_proj.weight`
  - `8`: `mlp.down_proj.weight`
  - `9`: `self_attn.q_proj.bias`
  - `10`: `self_attn.k_proj.bias`
  - `11`: `self_attn.v_proj.bias`

HF linear weights are stored as `[out, in]`; the tensor backend presents them to
`GEMM_F32` as `[in, out]` when used by the Qwen runtime.

## Weight loading policy

`MODEL_LOAD` parses only the safetensors header and builds the tensor table.
Tensor payloads are loaded lazily when an op references a non-zero `tensor_id`.
BF16 payloads are converted to FP32 inside QEMU.  This keeps device
initialization fast and avoids allocating the full model until an operator
actually needs a weight.
