# virt-llm Queue/DMA/Interrupt Requirements and Architecture

## Requirements

This stage turns the BAR self-test device into a small command-processing
accelerator model. It is still a prototype, but it should exercise the same
kernel/QEMU paths that a real LLM inference accelerator will need.

Functional requirements:

- Keep the existing PCI identity: vendor `0x1b36`, device `0x1100`, QEMU name
  `virt-llm`.
- Keep the existing identity registers so the old probe path remains stable.
- Add a guest-owned DMA command queue.
- Add one coherent DMA input buffer and one coherent DMA output buffer in the
  Linux driver validation path.
- Add a simulated inference command:
  - Device DMA-reads the input buffer.
  - Device transforms the data deterministically.
  - Device DMA-writes the output buffer.
  - Device DMA-writes descriptor completion status.
- Add interrupt completion using MSI-X when available, MSI as fallback, and
  INTx as the final fallback for platforms that expose no MSI/MSI-X IRQ domain.
- The driver must wait for an interrupt-driven completion, verify output data,
  and print a clear success line in the guest kernel log.

Non-goals for this stage:

- No userspace API yet.
- No multi-queue scheduling.
- No real tokenizer/model execution.
- No performance model beyond a deterministic command completion path.

## BAR0 Register Map

All registers are little-endian 32-bit. BAR0 remains 4 KiB.

| Offset | Name | Access | Description |
| --- | --- | --- | --- |
| `0x00` | MAGIC | RO | `0x4c4c4d31` (`LLM1`) |
| `0x04` | VERSION | RO | Device version, now `2` |
| `0x08` | DOORBELL | RW | Legacy self-test doorbell |
| `0x0c` | STATUS | RO | Legacy self-test status |
| `0x10` | FEATURES | RO | Feature bits |
| `0x14` | QUEUE_SIZE | RW | Number of descriptors |
| `0x18` | QUEUE_ADDR_LO | RW | Queue DMA address low 32 bits |
| `0x1c` | QUEUE_ADDR_HI | RW | Queue DMA address high 32 bits |
| `0x20` | QUEUE_HEAD | RO | Device-consumed descriptor index |
| `0x24` | QUEUE_TAIL | RW | Guest-produced descriptor index; write kicks processing |
| `0x28` | IRQ_STATUS | RW1C | Completion interrupt status bit |
| `0x2c` | IRQ_MASK | RW | Interrupt enable mask |
| `0x30` | COMMAND | WO | Explicit command/kick register |

Feature bits:

- Bit 0: DMA queue supported.
- Bit 1: MSI supported.
- Bit 2: MSI-X supported.

Interrupt bits:

- Bit 0: descriptor completion.

Command values:

- `1`: process pending descriptors.

## DMA Descriptor ABI

The queue is an array of 64-byte descriptors in coherent DMA memory.

```c
struct virt_llm_desc {
    __le32 opcode;
    __le32 flags;
    __le64 input_addr;
    __le64 output_addr;
    __le32 len;
    __le32 status;
    __le32 result;
    __le32 rsvd0;
    __le64 rsvd1;
    __le64 rsvd2;
    __le64 rsvd3;
};
```

Opcodes:

- `1`: simulated inference transform.

Status values:

- `0`: free/submitted but not complete.
- `1`: complete.
- `0x80000001`: invalid descriptor.

Result:

- For opcode `1`, QEMU returns a byte checksum of the output payload.

The simulated inference transform for validation is intentionally simple:

```text
output[i] = input[i] ^ 0x5a
result = sum(output[i]) & 0xffffffff
```

This proves bidirectional DMA and makes guest-side verification deterministic.

## QEMU Architecture

State added to `VirtLLMState`:

- BAR0 register fields: queue address, queue size, head, tail, irq status/mask.
- MSI support via `msi_init()`.
- MSI-X support via `msix_init_exclusive_bar()` with one vector.

Processing flow:

1. Guest writes queue size and queue DMA base.
2. Guest fills descriptor `tail % queue_size` in coherent memory.
3. Guest advances `QUEUE_TAIL` or writes `COMMAND=1`.
4. QEMU reads descriptors while `head != tail`.
5. For opcode `1`, QEMU reads input DMA buffer, writes transformed output, updates descriptor status/result.
6. QEMU sets `IRQ_STATUS.COMPLETE`.
7. If enabled, QEMU notifies MSI-X vector 0, else MSI vector 0, else INTx.

## Linux Driver Architecture

Probe flow:

1. Enable PCI device and map BAR0.
2. Allocate one IRQ vector with `PCI_IRQ_MSIX | PCI_IRQ_MSI`, falling back to
   `PCI_IRQ_INTX` when the guest platform does not provide MSI/MSI-X.
3. Register IRQ handler.
4. Allocate coherent DMA queue with 4 descriptors.
5. Allocate coherent input and output buffers.
6. Program queue registers.
7. Fill one descriptor for opcode `1`.
8. Enable completion interrupts through `IRQ_MASK`.
9. Submit by advancing `QUEUE_TAIL` and writing `COMMAND=1`.
10. Wait for completion with a kernel completion object.
11. Verify descriptor status, result checksum, and output bytes.
12. Print `dma inference ok` with irq mode, DMA addresses, length, and checksum.

## Validation Criteria

The QEMU boot log must include:

```text
pci 0000:00:01.0: [1b36:1100]
virt_llm_pci ... probe ok ...
virt_llm_pci ... dma inference ok ...
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

The driver must receive an interrupt for completion; polling-only completion is
not acceptable for this stage. MSI-X/MSI are preferred, but INTx is an accepted
fallback on the current riscv32 `virt` validation target.
