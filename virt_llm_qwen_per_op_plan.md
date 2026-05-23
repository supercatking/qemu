# virt-llm Qwen2.5-0.5B per-op plan

## Goal

Run Qwen2.5-0.5B inference through explicit `virt-llm` PCIe commands rather
than a monolithic host helper.  The guest runtime owns the graph schedule and
submits one command per operator.  The device owns tensor metadata, model
weights, activation buffers, completion status, and backend routing.

The first implementation target is correctness-oriented FP32 execution:
Qwen BF16 weights are represented as FP32 inside the simulator, activations are
FP32, and tokenizer/BPE processing stays in guest userspace or fixtures.

## ABI

New opcodes are grouped under `0x0300`:

- `MODEL_LOAD`: initialize the fixed Qwen model table.
- `MODEL_QUERY`: return Qwen config and selected tensor metadata.
- `EMBED_LOOKUP_F32`: token ids plus embedding table -> activations.
- `RMSNORM_F32`: activation plus weight vector -> normalized activation.
- `ROPE_F32`: apply Qwen RoPE to Q or K projections.
- `GEMM_F32`: row-major FP32 matrix multiply.
- `ADD_F32`: residual/vector add.
- `SWIGLU_F32`: `silu(gate) * up`.
- `QWEN_GQA_ATTENTION_F32`: causal grouped-query attention.
- `LM_HEAD_F32`: hidden state -> vocab logits.
- `ARGMAX_F32`: logits -> token id.

For v1, descriptors still use the existing queue and CQ ABI.  The descriptor
input buffer carries a packed `virt_llm_tensor_req`; the output buffer carries
operator-specific output data or `virt_llm_model_query`.  Descriptor `len` is
the input request byte size.  Larger buffers are supported by the Linux driver
through the existing `ALLOC_BUFFER` ioctl with a larger size field.

## Tensor request shape

`virt_llm_tensor_req` contains:

- `abi`, `dtype`, `rank`, `flags`
- `layer_id`, `tensor_id`, `aux_tensor_id`
- up to four dimensions
- byte offsets for input, weight, auxiliary input, and output inside DMA buffers
- scalar parameters such as epsilon and RoPE theta

The first QEMU implementation accepts DMA-backed tensors for all math ops.  The
same request format also carries model tensor ids so later phases can replace
DMA weights with device-owned Qwen weights without changing userspace.

## Execution phases

1. Freeze ABI and docs.
2. Add model query and model metadata path.
3. Add FP32 primitive ops with guest known-answer tests.
4. Add Qwen-specific RoPE, GQA attention, and SwiGLU.
5. Build a guest single-layer Qwen block test.
6. Build a full 24-layer single-token graph.
7. Add full-context greedy decode up to eight new tokens.

Each phase must preserve the existing `info`, `gemm`, `attention`, and
`INITRAMFS_OK` validation path.
