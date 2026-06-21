# virt-llm Architecture

## Current Scope

`virt-llm` is a QEMU PCI device that simulates the control path of a future LLM
inference accelerator. The current implementation focuses on hardware/software
plumbing rather than real model execution.

It currently validates:

- PCI device enumeration.
- BAR0 MMIO register access.
- Guest-to-device doorbell state.
- Guest-owned DMA descriptor queue.
- Queue enable/reset/status/error control.
- Device DMA reads and writes through `pci_dma_read()` and `pci_dma_write()`.
- Completion interrupts through MSI-X, MSI, or INTx fallback.
- A deterministic simulated inference command.
- An invalid-opcode descriptor error path.

## Source Locations

QEMU:

- Device model: `hw/misc/virt_llm.c`
- Build integration: `hw/misc/meson.build`
- Kconfig integration: `hw/misc/Kconfig`
- Planning docs: `virt_llm.md`, `virt_llm_queue_dma.md`

Linux side used for validation:

- Driver: `drivers/misc/virt_llm_pci.c`
- Kconfig: `drivers/misc/Kconfig`
- Makefile: `drivers/misc/Makefile`

## PCI Identity

| Field | Value |
| --- | --- |
| QEMU device name | `virt-llm` |
| Vendor ID | `0x1b36` |
| Device ID | `0x1100` |
| PCI class | `PCI_CLASS_OTHERS` |
| BAR0 | 4 KiB MMIO |
| MSI-X vectors | 1 |
| MSI vectors | 1 |
| INTx pin | INTA |

The QEMU command-line shape is:

```bash
-device virt-llm
```

## Device State

Current `VirtLLMState` contains:

| Field | Purpose |
| --- | --- |
| `PCIDevice parent_obj` | QEMU PCI base object |
| `MemoryRegion mmio` | BAR0 MMIO region |
| `doorbell` | Legacy self-test register state |
| `queue_addr` | Guest physical DMA address of descriptor ring |
| `queue_size` | Number of descriptors in ring |
| `queue_head` | Device-consumed descriptor index |
| `queue_tail` | Guest-produced descriptor index |
| `irq_status` | Pending interrupt cause bits |
| `irq_mask` | Enabled interrupt cause bits |
| `queue_ctrl` | Queue enable/reset control |
| `queue_status` | Queue enabled/error state |
| `queue_error` | Last queue error code |

## BAR0 Register Map

All registers are currently 32-bit little-endian accesses.

| Offset | Name | Access | Description |
| --- | --- | --- | --- |
| `0x00` | `MAGIC` | RO | Returns `0x4c4c4d31` (`LLM1`) |
| `0x04` | `VERSION` | RO | Returns `3` |
| `0x08` | `DOORBELL` | RW | Legacy self-test input |
| `0x0c` | `STATUS` | RO | Returns `DOORBELL ^ 0xa5a5a5a5` |
| `0x10` | `FEATURES` | RO | Queue, MSI, and MSI-X support bits |
| `0x14` | `QUEUE_SIZE` | RW | Number of descriptors |
| `0x18` | `QUEUE_ADDR_LO` | RW | Low 32 bits of descriptor queue DMA address |
| `0x1c` | `QUEUE_ADDR_HI` | RW | High 32 bits of descriptor queue DMA address |
| `0x20` | `QUEUE_HEAD` | RO | Device-consumed index |
| `0x24` | `QUEUE_TAIL` | RW | Guest-produced index; write triggers processing |
| `0x28` | `IRQ_STATUS` | RW1C | Completion interrupt status |
| `0x2c` | `IRQ_MASK` | RW | Interrupt enable mask |
| `0x30` | `COMMAND` | WO | `1` kicks queue processing |
| `0x34` | `ABI` | RO | ABI version, currently `1` |
| `0x38` | `QUEUE_MAX` | RO | Maximum queue depth, currently `1024` |
| `0x3c` | `XFER_MAX` | RO | Maximum transfer length, currently `4096` |
| `0x40` | `IRQ_VEC` | RO | Available interrupt vectors, currently `1` |
| `0x44` | `QUEUE_CTRL` | RW | Queue enable/reset bits |
| `0x48` | `QUEUE_STATUS` | RO | Queue enabled/error status |
| `0x4c` | `QUEUE_ERROR` | RO | Last queue error code |

Feature bits:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `QUEUE` | DMA descriptor queue exists |
| 1 | `MSI` | Device advertises MSI support |
| 2 | `MSI-X` | Device advertises MSI-X support |
| 3 | `QCTRL` | Queue control/status/error registers exist |

Interrupt bits:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `COMPLETE` | At least one descriptor completed |
| 1 | `ERROR` | At least one descriptor or queue error occurred |

Queue control bits:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `ENABLE` | Queue may consume ready descriptors |
| 1 | `RESET` | Reset queue registers, status, and error |

Queue status bits:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `ENABLED` | Queue is enabled |
| 1 | `ERROR` | Queue has reported an error |

Queue error codes:

| Value | Meaning |
| --- | --- |
| `0` | No queue error |
| `1` | Invalid queue configuration |
| `2` | Invalid descriptor |
| `3` | Unsupported descriptor opcode |

## Descriptor ABI

The queue points to an array of packed 64-byte descriptors:

```c
struct virt_llm_desc {
    uint32_t opcode;
    uint32_t flags;
    uint64_t input_addr;
    uint64_t output_addr;
    uint32_t len;
    uint32_t status;
    uint32_t result;
    uint32_t rsvd0;
    uint64_t rsvd1;
    uint64_t rsvd2;
    uint64_t rsvd3;
};
```

Current opcode:

| Opcode | Name | Behavior |
| --- | --- | --- |
| `1` | `INFER` | DMA-read input, write `input[i] ^ 0x5a` to output, return checksum |

Descriptor flags:

| Bit | Name | Meaning |
| --- | --- | --- |
| 0 | `READY` | Guest has finished writing the descriptor |

Current status values:

| Value | Meaning |
| --- | --- |
| `1` | Descriptor complete |
| `0x80000001` | Invalid descriptor or unsupported opcode |
| `0x80000002` | Unsupported opcode |
| `0x80000003` | Invalid transfer length or DMA buffer address |

## Command Lifecycle

```mermaid
sequenceDiagram
    participant Driver as Linux driver
    participant BAR as BAR0 MMIO
    participant QEMU as virt-llm device
    participant RAM as Guest RAM via DMA

    Driver->>RAM: Fill input buffer and descriptor
    Driver->>BAR: Program queue address and size
    Driver->>BAR: Enable IRQ mask and QUEUE_CTRL.ENABLE
    Driver->>BAR: Write QUEUE_TAIL / COMMAND=KICK
    BAR->>QEMU: virt_llm_process_queue()
    QEMU->>RAM: pci_dma_read(descriptor)
    QEMU->>RAM: pci_dma_read(input)
    QEMU->>RAM: pci_dma_write(output)
    QEMU->>RAM: pci_dma_write(descriptor status/result)
    QEMU->>BAR: Set IRQ_STATUS.COMPLETE
    QEMU-->>Driver: MSI-X, MSI, or INTx complete/error interrupt
    Driver->>BAR: Read and clear IRQ_STATUS
    Driver->>RAM: Verify descriptor/output/checksum
```

## Interrupt Model

The device initializes both MSI and MSI-X capability:

- MSI: one vector through `msi_init()`.
- MSI-X: one vector through `msix_init_exclusive_bar()`.
- INTx: fallback through `pci_set_irq()`.

Completion path:

1. QEMU sets `IRQ_STATUS.COMPLETE` for successful descriptors or
   `IRQ_STATUS.ERROR` for descriptor/queue errors.
2. If the matching mask bit is set:
   - notify MSI-X vector 0 when MSI-X is enabled;
   - else notify MSI vector 0 when MSI is enabled;
   - else assert INTx.
3. Driver clears observed IRQ status bits with write-one-to-clear.
4. QEMU deasserts INTx when no interrupt status remains.

## Linux Driver Contract

The validation driver is expected to:

1. Enable the PCI device.
2. Call `pci_set_master()` so the device can issue DMA.
3. Set a 32-bit coherent DMA mask.
4. Map BAR0.
5. Verify `MAGIC` and legacy `DOORBELL`/`STATUS`.
6. Allocate coherent queue, input, and output buffers.
7. Allocate MSI-X/MSI, falling back to INTx.
8. Enable the queue through `QUEUE_CTRL.ENABLE`.
9. Submit one ready descriptor.
10. Wait for interrupt-driven completion.
11. Verify descriptor status, output bytes, and checksum.
12. Submit an unsupported opcode descriptor and verify the error interrupt,
    descriptor status, queue status, and queue error code.

## Current Limitations

- Queue ownership exists through a descriptor READY flag, but there is no
  device-owned completion flag yet.
- Queue processing is synchronous inside MMIO write handling.
- There is only one queue and one interrupt vector.
- Error reporting is still coarse and should grow per-cause codes.
- There is no userspace interface yet.
- There is no QEMU migration state.
- There are no QEMU trace events for queue or interrupt activity.
- The simulated inference command is a deterministic byte transform, not a
  model-oriented token interface.

## Recommended Target Architecture

The next stable shape should look like this:

- BAR0 remains the control register aperture.
- Queue configuration becomes explicit: queue reset, queue enable, queue status,
  queue depth, and queue error.
- Descriptors gain ownership and richer status codes.
- Interrupt causes are split into completion, queue error, device error, and
  reset completion.
- QEMU moves command completion toward an asynchronous model using a timer or
  bottom half when latency simulation is enabled.
- Linux grows a userspace entry point after the kernel self-test path is solid.

That architecture keeps the current bring-up path intact while creating a
natural route toward a usable simulated LLM accelerator.
