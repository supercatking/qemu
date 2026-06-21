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
