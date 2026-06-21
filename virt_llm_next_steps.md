# virt-llm Next Development Plan

## Purpose

The current `virt-llm` device proves the basic PCIe accelerator path:
enumeration, BAR MMIO, a guest-owned DMA descriptor queue, MSI/MSI-X/INTx
completion, and one deterministic simulated inference command.

The next stage should turn this prototype into a more useful accelerator
simulation platform. The goal is not yet to run a real model. The goal is to
make the QEMU device and Linux driver look like a small but credible inference
card so future model-runtime work has stable hardware semantics to build on.

## Guiding Principles

- Keep the ABI small and explicit.
- Preserve existing register offsets where possible.
- Version the device ABI before adding incompatible behavior.
- Prefer one feature per validation step.
- Keep QEMU behavior deterministic so guest-side tests are repeatable.
- Make every device feature observable from Linux logs or a simple userspace
  test.

## Proposed Work Phases

### Phase 0: Command-Dispatcher Architecture Document

This phase captures the target architecture requested for the LLM PCIe
accelerator:

- command queue front end parses and dispatches commands;
- simple DMA commands execute directly in a DMA engine;
- vector operations such as vector add, softmax, and pooling dispatch to a
  simulated RISC-V vector processor backend;
- GEMM and future tensor kernels dispatch to a simulated tensor core backend;
- every command eventually writes a completion entry and raises an interrupt.

Acceptance:

- Architecture document explains command queue, backend dispatch, opcode
  ranges, descriptor arguments, and completion policy.
- Implementation phases are small enough that each one can be committed and
  reverted independently.

### Phase 1: ABI Cleanup and Versioning

Add a clearer device ABI boundary before growing the feature set.

Tasks:

- Define a stable public header for register offsets, feature bits, opcodes,
  descriptor layout, status codes, and limits.
- Decide whether the shared ABI header lives only in documentation for now or
  is duplicated in QEMU and Linux with matching comments.
- Add register fields for ABI version, device capabilities, queue depth limit,
  maximum transfer size, and interrupt vector count.
- Document reset behavior for all registers.
- Add stricter validation for invalid queue size, unaligned queue address,
  descriptor overflow, and unsupported commands.

Acceptance:

- Existing Linux driver still passes the self-test.
- Guest logs show ABI version and capabilities.
- Invalid descriptor tests return documented error status instead of silent
  behavior.

### Phase 2: Queue Model Upgrade

Move from a single simple queue to a realistic command ring.

Tasks:

- Add a queue control register set: enable, reset, pause, and error state.
- Treat queue head/tail as ring indices with wrap behavior.
- Add descriptor ownership semantics so QEMU cannot consume descriptors before
  the driver marks them ready.
- Add multiple outstanding descriptors in one submission.
- Add per-descriptor completion status and optional completion counter.
- Add queue error reporting for invalid address, invalid length, and unknown
  opcode.

Acceptance:

- Linux driver submits several descriptors in one batch.
- QEMU completes all descriptors in order.
- Driver verifies output for every descriptor.
- Queue wrap-around is covered by a validation run.

### Phase 3: Interrupt and Completion Refinement

Make interrupt behavior closer to real PCIe accelerator hardware.

Tasks:

- Separate interrupt cause bits: descriptor complete, queue error, device fatal
  error, and reset complete.
- Add interrupt mask and interrupt clear behavior for every cause bit.
- Keep MSI-X as preferred, MSI as fallback, INTx as compatibility fallback.
- Add optional interrupt coalescing fields: completion threshold and delay.
- Make the Linux driver print the selected interrupt mode and cause bits.

Acceptance:

- Completion interrupt is verified without polling.
- Error interrupt path is tested with an intentionally invalid descriptor.
- INTx fallback remains functional on the current riscv32 validation platform.

### Phase 4: Simulated Inference Command Set

Extend the placeholder inference operation into a small command family.

Tasks:

- Keep opcode `1` as the deterministic byte transform compatibility command.
- Add an `INFER_SUBMIT` command descriptor with fields for model id, input
  token count, output token budget, and flags.
- Add a `QUERY_MODEL` or `GET_INFO` command that returns simulated model
  metadata.
- Add deterministic fake token generation so userspace can observe inference
  style behavior without a real model.
- Add latency knobs in QEMU properties, for example fixed delay, bytes-per-ms,
  or tokens-per-second.

Acceptance:

- Linux driver can submit a simulated inference descriptor and verify returned
  metadata/output.
- QEMU command-line properties can change simulated latency.
- The compatibility XOR self-test remains available for quick bring-up.

### Phase 5: Driver Interface for Userspace

Expose the accelerator through a small Linux interface instead of only probe
self-tests.

Tasks:

- Decide between a misc character device, debugfs test interface, or ioctl
  based control device.
- Add a minimal userspace test program that opens the device, submits a command,
  and reads completion.
- Keep kernel probe self-test optional through a module parameter.
- Add proper cleanup paths for queue teardown and device remove.

Acceptance:

- Boot does not need to run a destructive self-test by default.
- Userspace can trigger the deterministic inference operation.
- Driver unload/reload works when built as a module.

### Phase 6: Robustness and Migration Readiness

Prepare the QEMU model for longer-running tests.

Tasks:

- Add VMState migration fields for device state.
- Add reset tests for queue and interrupt state.
- Avoid unbounded synchronous work in MMIO write handlers when simulated
  inference latency is enabled.
- Consider a QEMU bottom half or timer for asynchronous command completion.
- Add tracing points for queue submit, descriptor complete, DMA error, and IRQ.

Acceptance:

- Device state survives QEMU reset in a documented way.
- Long-running command simulation does not block the vCPU in a surprising way.
- Trace output can explain each command lifecycle.

## Implemented in the First Approved Step

The first approved coding step implemented the ABI cleanup slice and a small
queue-control slice:

- Bumped the QEMU device version to `3`.
- Added ABI, maximum queue depth, maximum transfer size, and interrupt vector
  registers.
- Added queue control, queue status, and queue error registers.
- Added descriptor READY ownership semantics.
- Split interrupt causes into completion and error bits.
- Added explicit descriptor status codes for unsupported opcode and bad length.
- Updated the Linux validation driver to verify both the successful inference
  descriptor and an invalid-opcode descriptor error path.

Validation result:

```text
virt_llm_pci ... dma inference ok: irq=intx ... checksum=0x000017e0
virt_llm_pci ... error path ok: desc_status=0x80000002 q_status=0x00000003 q_error=3
virt_llm_pci ... probe ok: magic=0x4c4c4d31 version=3 abi=1 q_max=1024 xfer_max=4096 irq_vec=1 ...
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

## Implemented in the Dispatcher Backend Step

This step implemented the first version of the architecture requested for the
LLM PCIe accelerator:

- Added a central QEMU command dispatcher.
- Refactored opcode `0x0001` into the compatibility `INFER_XOR` backend.
- Added opcode `0x0010` as a direct DMA engine `DMA_COPY` command.
- Added opcode `0x0100` as a RISC-V vector backend `VECTOR_ADD_U32` command.
- Added opcode `0x0200` as a tensor core backend `GEMM_U32` command.
- Added QEMU debug logging for opcode, backend id, status, and result.
- Updated the Linux validation driver to submit and verify one command per
  backend.

Validation result:

```text
virt_llm_pci ... dma inference ok: irq=intx ... checksum=0x000017e0
virt_llm_pci ... dma copy ok: len=64 checksum=0x00002fe0
virt_llm_pci ... vector add ok: count=8 checksum=0x00000360
virt_llm_pci ... gemm ok: m=2 n=2 k=2 checksum=0x00000086
virt_llm_pci ... error path ok: desc_status=0x80000002 q_status=0x00000003 q_error=3
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

## Implemented in the Completion Queue Step

This step added the first real completion queue path:

- Added CQ registers: base address, size, guest head, and device tail.
- Added a compact CQ entry with command id, opcode, backend, status, result,
  and queue head snapshot.
- Kept inline descriptor status for compatibility.
- Updated Linux validation to program a CQ and verify every command completion
  by command id.

Validation result:

```text
virt_llm_pci ... dma buffers: queue=... cq=... input=... output=...
virt_llm_pci ... dma inference ok: ...
virt_llm_pci ... dma copy ok: ...
virt_llm_pci ... vector add ok: ...
virt_llm_pci ... gemm ok: ...
virt_llm_pci ... error path ok: ...
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

## Implemented in the Vector Kernel Step

This step extended the RISC-V vector backend:

- Added `SOFTMAX_Q16`, a deterministic fixed-point normalization command.
- Added `POOL_MAX_U32`, a deterministic max-pooling command.
- Extended Linux validation with known-answer tests.
- Kept completions flowing through the CQ path.

Validation result:

```text
virt_llm_pci ... softmax q16 ok: count=4 checksum=0x00010000
virt_llm_pci ... pool max ok: count=8 window=2 checksum=0x00000018
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

## Immediate Next Coding Step

The best next coding step is to make the command queue more realistic:

1. Add command batching with several descriptors submitted before one kick.
2. Add ring wrap-around tests for SQ and CQ.
3. Add device-owned completion flags or generation bits.
4. Add queue-full and CQ-full error handling.
5. Re-run the riscv32 Linux 6.12 boot validation.

This will make the front-end queue behave more like hardware instead of a
single-command-at-a-time validation path.

## Implementation Phases for the LLM Accelerator Shape

### Implementation Phase A: Dispatcher and Backend Stubs

QEMU:

- Add backend ids for compatibility, DMA, vector, and tensor.
- Add a central `virt_llm_dispatch_desc()` function.
- Refactor existing XOR inference command into the compatibility backend.
- Add lightweight debug logs for opcode, backend, status, and result.

Linux:

- Keep current validation unchanged.

Commit boundary:

- One QEMU-only commit.

### Implementation Phase B: DMA Engine Command

QEMU:

- Add opcode `DMA_COPY`.
- Read `len` bytes from `input_addr`, write to `output_addr`, return checksum.

Linux:

- Submit a DMA copy descriptor and verify output bytes/checksum.

Commit boundary:

- One QEMU commit and one Linux driver commit after validation.

### Implementation Phase C: RISC-V Vector Backend

QEMU:

- Add opcode `VECTOR_ADD_U32`.
- Treat `input_addr` and `rsvd1` as two u32 arrays.
- Treat `len` as element count.
- Write u32 output to `output_addr`.

Linux:

- Allocate a second input buffer.
- Submit vector add and verify output/checksum.

Commit boundary:

- One QEMU commit and one Linux driver commit after validation.

### Implementation Phase D: Tensor Core Backend

QEMU:

- Add opcode `GEMM_U32`.
- Treat `input_addr` as matrix A, `rsvd1` as matrix B, `output_addr` as C.
- Pack dimensions into `rsvd2`: low 16 bits M, next 16 bits N, next 16 bits K.
- Keep matrix sizes small for validation.

Linux:

- Submit a small GEMM, for example 2x2 times 2x2, and verify output.

Commit boundary:

- One QEMU commit and one Linux driver commit after validation.

### Implementation Phase E: Completion Queue

QEMU:

- Add CQ base/size/head/tail registers.
- Write completion entries rather than relying only on inline descriptor status.
- Preserve inline status temporarily for compatibility.

Linux:

- Allocate and program CQ.
- Consume completions by command id.

Commit boundary:

- One ABI-breaking or ABI-extending QEMU/Linux pair after validation.

## Review Questions

- Should the next public interface be a kernel misc char device, debugfs, or
  module self-test only?
- Should simulated inference latency be synchronous first, or should we move
  directly to an asynchronous timer/bottom-half model?
- Do we want one queue for now, or should the next ABI already reserve register
  space for multiple queues?
- Should this device model remain under `hw/misc`, or move later toward a
  dedicated accelerator directory if the model grows?
