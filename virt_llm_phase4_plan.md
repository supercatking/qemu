# virt-llm Phase 4 Development Plan

## Purpose

Phase 4 introduces a firmware-like kernel table for the internal RISC-V32
scalar dispatcher. The goal is to make vector command dispatch look less like a
hard-coded switch and more like a real accelerator control processor selecting
small kernels from internal instruction memory.

This phase is still a simulation step. It will not execute real RISC-V
instructions yet. It will define kernel metadata, expose the table to the Linux
driver, and make scalar dispatch validate the selected kernel ABI before
launching the existing vector backend helpers.

## Current Baseline

The current QEMU device already has:

- PCIe enumeration and BAR0 MMIO.
- Submission queue and completion queue.
- Descriptor READY ownership.
- MSI-X, MSI, and INTx fallback interrupt paths.
- DMA backend for `DMA_COPY`.
- RISC-V32 scalar dispatcher skeleton for vector commands.
- Vector helpers for add, dot product, softmax, and pooling.
- Tensor core helpers for GEMM and valid CONV2D.

The Linux 6.12 validation driver has been pushed to:

```text
https://github.com/supercatking/linux/tree/llmdev-linux-6.12
```

## Non-goals

Phase 4 will not add:

- real RISC-V instruction execution;
- uploaded user kernels;
- a userspace character device;
- asynchronous queue scheduling;
- multi-queue support;
- model-level LLM token generation.

Those are later phases. This keeps the next iteration small enough to validate
and revert.

## Proposed Hardware Model

The scalar dispatcher gains a small internal kernel ROM/table:

```c
struct virt_llm_kernel_meta {
    uint32_t kernel_id;
    uint32_t abi_version;
    uint32_t supported_opcode;
    uint32_t entry_point;
    uint32_t binary_size;
    uint32_t binary_checksum;
    const char *name;
};
```

The binary data is fake at first. It represents what would eventually be
firmware or vector-kernel code, but QEMU will still call the existing C helper
for each vector operation.

Initial table:

| Kernel id | Name | Opcode | ABI | Backend |
| --- | --- | --- | --- | --- |
| `1` | `vec_add_u32` | `VECTOR_ADD_U32` | `1` | vector |
| `2` | `dot_u32` | `DOT_U32` | `1` | vector |
| `3` | `softmax_q16` | `SOFTMAX_Q16` | `1` | vector |
| `4` | `pool_max_u32` | `POOL_MAX_U32` | `1` | vector |

## Proposed Register Extension

Keep existing offsets stable. Add read-only kernel metadata view registers after
the current scalar dispatcher block:

| Offset | Name | Access | Description |
| --- | --- | --- | --- |
| `0x74` | `KERNEL_INDEX` | RW | Selects kernel table slot for metadata reads |
| `0x78` | `KERNEL_ID` | RO | Kernel id in selected slot |
| `0x7c` | `KERNEL_OPCODE` | RO | Supported opcode in selected slot |
| `0x80` | `KERNEL_ABI` | RO | Kernel ABI version |
| `0x84` | `KERNEL_ENTRY` | RO | Fake internal entry point |
| `0x88` | `KERNEL_SIZE` | RO | Fake binary size in bytes |
| `0x8c` | `KERNEL_CHECKSUM` | RO | Fake binary checksum |

Existing `SCALAR_KERNELS` remains the kernel count register.

## Descriptor ABI

For scalar-dispatched vector commands, `rsvd3` keeps the current layout:

| Bits | Name | Meaning |
| --- | --- | --- |
| `0..15` | `kernel_id` | Selected scalar kernel |
| `16..23` | `kernel_abi` | Required kernel ABI, `0` means default ABI |
| `24..31` | `dispatch_flags` | Reserved for future debug or async flags |

Phase 4 changes the validation rule:

1. Look up `kernel_id` in the kernel table.
2. Reject if no entry exists.
3. Reject if the entry does not support the requested opcode.
4. Reject if `kernel_abi` is non-zero and does not match the entry ABI.
5. Launch the existing vector backend helper only after metadata validation.

## Error Semantics

Add one descriptor status for kernel dispatch validation:

| Status | Meaning |
| --- | --- |
| `0x80000004` | Invalid kernel id, opcode mismatch, or ABI mismatch |

The queue error register should report a kernel dispatch error with a new code:

| Code | Meaning |
| --- | --- |
| `4` | Kernel dispatch validation failed |

This keeps unknown opcodes separate from known opcodes with invalid kernel
metadata.

## Implementation Steps

### Step 4.1: Documentation and Review

Status: this document.

Acceptance:

- Architecture and register proposal are reviewable before code changes.
- No QEMU device code changes are made in this step.

### Step 4.2: QEMU Kernel Table Metadata

Status: implemented and validated.

QEMU:

- Add a static built-in kernel metadata table.
- Add kernel metadata registers.
- Keep current scalar dispatch behavior unchanged.
- Add debug log output when a selected kernel is read or used.

Linux:

- Read `SCALAR_KERNELS`.
- Iterate kernel table slots with `KERNEL_INDEX`.
- Verify ids, opcodes, ABI, size, and checksum are non-zero and consistent.

Validation:

- QEMU build passes.
- Linux 6.12 build passes.
- Boot log prints `kernel table ok`.

Validation result:

```text
virt_llm_pci ... kernel table ok: kernels=4 abi=1 first_entry=0x00001000 last_entry=0x00001300
virt_llm_pci ... probe ok: ... scalar_kernels=4 ...
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

Commit boundary:

- One QEMU commit.
- One Linux driver validation commit.
- Push QEMU to `supercatking/qemu:llmdev`.
- Push Linux to `supercatking/linux:llmdev-linux-6.12` if approved.

### Step 4.3: Enforced Kernel ABI Validation

QEMU:

- Replace hard-coded scalar kernel checks with table lookup.
- Return `DESC_BAD_KERNEL` for missing kernel id, opcode mismatch, or ABI
  mismatch.
- Preserve successful vector add, dot, softmax, and pooling behavior.

Linux:

- Add one bad-kernel-id validation descriptor.
- Add one bad-kernel-ABI validation descriptor.
- Verify CQ backend is scalar and status is `DESC_BAD_KERNEL`.

Validation:

- Existing positive self-tests still pass.
- New negative kernel validation tests pass.
- Unknown opcode still returns `DESC_UNSUPP`, not `DESC_BAD_KERNEL`.

### Step 4.4: Kernel Binary Debug Metadata

QEMU:

- Add fake binary blobs or compact byte arrays for the built-in kernels.
- Expose binary checksum through `KERNEL_CHECKSUM`.
- Add scalar last-entry and last-ABI readout if useful.

Linux:

- Verify checksums against expected constants.
- Print selected kernel metadata during probe.

Validation:

- Boot log makes scalar dispatch decisions visible without QEMU tracing.

## Risks and Controls

| Risk | Control |
| --- | --- |
| Register map churn | Add new registers after existing scalar block only |
| ABI ambiguity | Document `rsvd3` kernel ABI bits before enforcing them |
| Confusing unknown opcode vs bad kernel | Use separate descriptor status values |
| Overbuilding before real execution | Keep fake binary data metadata-only |
| Linux and QEMU drift | Validate table entries from the Linux driver on every boot |

## Review Questions

- Should the kernel metadata registers expose one selected slot, or should they
  return a packed structure through DMA?
- Should `kernel_abi = 0` mean default ABI, or should Linux always provide an
  explicit ABI version?
- Should bad kernel metadata raise `IRQ_ERROR`, or complete as a normal command
  with error status and `IRQ_COMPLETE`?
- Should Phase 4.2 push Linux validation changes immediately, now that the
  Linux fork branch exists?
