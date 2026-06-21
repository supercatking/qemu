# virt-llm V1.0 Release Notes

## 1. Purpose

`virt-llm` is a QEMU PCIe device model for exploring the software and hardware
interface of an LLM inference accelerator.  V1.0 focuses on a debuggable
functional simulator rather than a cycle-accurate or performance-accurate
accelerator.

The current goal is to provide a concrete PCIe endpoint, Linux driver, command
queue ABI, completion path, and a set of neural-network operators that can be
used by a guest runtime.  The simulator is intentionally explicit: the guest
submits one hardware command per operator, and QEMU executes the corresponding
device-side backend.

V1.0 has two validation levels:

- Basic PCIe/device validation: DMA, queues, interrupts, GEMM, attention, and
  error paths.
- Qwen per-op validation: Qwen2.5-0.5B style FP32 operator graph, including
  model metadata, lazy weight loading, GEMM, GQA attention, MLP, LM head, and
  argmax.

## 2. Device Abstraction and Simulated Modules

`virt-llm` appears to the guest as a conventional PCI device:

- Vendor ID: `0x1b36` (`PCI_VENDOR_ID_REDHAT`)
- Device ID: `0x1100`
- Revision: `0x01`
- Class: `PCI_CLASS_OTHERS`
- BAR0: device MMIO register space
- BAR1: MSI-X table/PBA area allocated by QEMU

Inside that PCIe function, V1.0 models the following accelerator modules.

### PCIe Endpoint

The PCIe endpoint exposes BARs, interrupt capability, and a DMA-capable device
interface.  Linux enumerates it as `0000:00:01.0` in the current RISC-V `virt`
machine setup.

### MMIO Register Block

BAR0 is a 4 KiB little-endian MMIO region.  It contains device identity,
feature discovery, SQ/CQ configuration, interrupt state, queue control, queue
error reporting, and scalar kernel metadata registers.

### DMA Engine Model

The simulated DMA engine uses QEMU PCI DMA helpers:

- `pci_dma_read()` to copy from guest DMA memory into QEMU host buffers.
- `pci_dma_write()` to copy QEMU host buffers back into guest DMA memory.

This models device-initiated PCIe DMA transactions.  It is not a zero-copy path.

### Submit Queue and Completion Queue

The guest driver allocates coherent DMA memory for a submit queue (SQ) and
completion queue (CQ).  The device reads descriptors from SQ and writes
completion entries to CQ.

### Interrupt Model

V1.0 supports completion/error notification through INTx, MSI, and MSI-X.  The
current QEMU device initializes one MSI vector and one MSI-X vector.  The Linux
driver validates the active interrupt path during probe.

### Scalar Dispatcher

The scalar dispatcher validates kernel metadata before dispatching selected
simple kernels.  It covers:

- `VEC_ADD_U32`
- `DOT_U32`
- `SOFTMAX_Q16`
- `POOL_MAX_U32`

The dispatcher checks kernel id, ABI version, opcode compatibility, and reports
`DESC_BAD_KERNEL` for invalid kernel metadata.

### Vector Backend

The vector backend models elementwise and normalization-style accelerator
blocks.  V1.0 routes the following Qwen FP32 operators to this backend:

- `EMBED_LOOKUP_F32`
- `RMSNORM_F32`
- `ROPE_F32`
- `ADD_F32`
- `SWIGLU_F32`
- `ARGMAX_F32`

### Tensor Backend

The tensor backend models tensor-core style computation.  It is functional, not
performance-accurate.  V1.0 routes these operators to it:

- `GEMM_U32`
- `CONV2D_U32`
- `ATTENTION_Q16`
- `GEMM_F32`
- `LM_HEAD_F32`
- `QWEN_GQA_ATTENTION_F32`

`GEMM_F32` supports Qwen model tensor ids, lazy BF16 weight loading, conversion
to FP32, Hugging Face `[out, in]` to runtime `[in, out]` transposition, and
optional bias through `aux_tensor_id`.

### Model Metadata and Weight Loader

`MODEL_LOAD` parses the local Qwen safetensors header and builds a stable tensor
table.  Weight payloads are loaded lazily when an operator references a
non-zero `tensor_id`.

The fixed V1.0 model path is:

```text
/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors
```

The Qwen configuration encoded in V1.0 is:

- layers: `24`
- hidden size: `896`
- attention heads: `14`
- KV heads: `2`
- head dimension: `64`
- intermediate size: `4864`
- vocab size: `151936`
- RoPE theta: `1000000.0`
- RMSNorm epsilon: `1e-6`

## 3. Hardware Not Modeled in V1.0

V1.0 is not a complete hardware simulator.  The following hardware blocks are
not explicitly modeled:

- Device-side DDR, HBM, or GDDR.
- Device-side SRAM, scratchpad memory, register files, or tile buffers.
- Tensor-core pipeline stages, systolic arrays, warp scheduling, or occupancy.
- Memory banks, cache hierarchy, coherency, prefetch, or eviction policy.
- PCIe packet timing, link bandwidth, DMA latency, or backpressure.
- Asynchronous command workers and realistic command overlap.
- KV cache memory residency as a device-owned region.
- Quantized int8/int4 execution pipelines.
- CUDA/RTX backend.

Current activation storage is guest coherent DMA memory.  During execution,
QEMU copies tensors from guest DMA memory into temporary QEMU heap buffers,
computes on host CPU pointers, then copies results back to guest DMA memory.

The current data path is:

```text
guest userspace mmap buffer
  -> guest coherent DMA buffer
  -> QEMU pci_dma_read copy
  -> QEMU heap temporary input/weight/output buffers
  -> QEMU host CPU operator implementation
  -> QEMU pci_dma_write copy
  -> guest coherent DMA output buffer
```

## 4. PCIe BAR Space and Hardware Registers

### BAR Layout

- BAR0: `virt-llm-mmio`, 4 KiB, little-endian, 32-bit MMIO accesses.
- BAR1: MSI-X exclusive BAR for MSI-X table/PBA.

### BAR0 Register Map

| Offset | Register | Description |
|---:|---|---|
| `0x00` | `MAGIC` | Device magic, `0x4c4c4d31` (`"LLM1"`) |
| `0x04` | `VERSION` | Device version, currently `3` |
| `0x08` | `DOORBELL` | Basic doorbell test register |
| `0x0c` | `STATUS` | Doorbell/status test result |
| `0x10` | `FEATURES` | Feature bits: queue, MSI, MSI-X, QCTRL, CQ, scalar |
| `0x14` | `Q_SIZE` | Submit queue size |
| `0x18` | `Q_LO` | Submit queue DMA base low 32 bits |
| `0x1c` | `Q_HI` | Submit queue DMA base high 32 bits |
| `0x20` | `Q_HEAD` | Device-consumed submit queue head |
| `0x24` | `Q_TAIL` | Driver-submitted submit queue tail |
| `0x28` | `IRQ_STS` | Interrupt status |
| `0x2c` | `IRQ_MASK` | Interrupt mask |
| `0x30` | `COMMAND` | Write `1` to kick queue processing |
| `0x34` | `ABI` | Device ABI version |
| `0x38` | `Q_MAX` | Maximum queue size, currently `1024` |
| `0x3c` | `XFER_MAX` | Maximum simple transfer size |
| `0x40` | `IRQ_VEC` | Interrupt vector information |
| `0x44` | `Q_CTRL` | Queue enable/reset control |
| `0x48` | `Q_STATUS` | Queue enabled/error status |
| `0x4c` | `Q_ERROR` | Queue error code |
| `0x50` | `CQ_SIZE` | Completion queue size |
| `0x54` | `CQ_LO` | Completion queue DMA base low 32 bits |
| `0x58` | `CQ_HI` | Completion queue DMA base high 32 bits |
| `0x5c` | `CQ_HEAD` | Driver-consumed completion queue head |
| `0x60` | `CQ_TAIL` | Device-produced completion queue tail |
| `0x64` | `SCALAR_STATUS` | Scalar dispatcher state |
| `0x68` | `SCALAR_KERNELS` | Number of scalar kernel metadata entries |
| `0x6c` | `SCALAR_LAST_KERNEL` | Last scalar kernel id |
| `0x70` | `SCALAR_LAST_OPCODE` | Last scalar opcode |
| `0x74` | `KERNEL_INDEX` | Selected scalar metadata index |
| `0x78` | `KERNEL_ID` | Selected scalar kernel id |
| `0x7c` | `KERNEL_OPCODE` | Selected scalar kernel opcode |
| `0x80` | `KERNEL_ABI` | Selected scalar kernel ABI |
| `0x84` | `KERNEL_ENTRY` | Selected scalar kernel entry address |
| `0x88` | `KERNEL_SIZE` | Selected scalar kernel binary size |
| `0x8c` | `KERNEL_CHECKSUM` | Selected scalar kernel checksum |

## 5. Command Queue and Completion Queue ABI

### Device Descriptor

The QEMU device consumes this packed descriptor from guest DMA memory:

```c
struct VirtLLMDesc {
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

Field usage:

- `opcode`: operation selector.
- `flags`: includes `DESC_F_READY`.
- `input_addr`: guest DMA address of input/request buffer.
- `output_addr`: guest DMA address of output buffer.
- `len`: request length or simple transfer length.
- `status`: descriptor completion status written by device.
- `result`: operation-specific result or checksum.
- `rsvd0`: command id.
- `rsvd1`: secondary DMA address, commonly weight/B/K buffer.
- `rsvd2`: tertiary DMA address or packed dimensions for legacy ops.
- `rsvd3`: extra metadata, including scalar kernel selector.

### Userspace Descriptor ABI

Guest userspace does not pass DMA addresses directly.  It passes handles to the
Linux driver:

```c
struct virt_llm_user_desc {
    __u32 opcode;
    __u32 flags;
    __u32 input_handle;
    __u32 output_handle;
    __u32 len;
    __u32 command_id;
    __u32 rsvd0;
    __u32 rsvd1_handle;
    __u32 rsvd2_handle;
    __u32 reserved;
    __u64 rsvd1_addr;
    __u64 rsvd2_addr;
    __u64 rsvd3;
};
```

The driver translates handles to coherent DMA addresses before writing the SQ
descriptor.

### Completion Entry

The device writes a CQ entry for every completed descriptor:

```c
struct VirtLLMCpl {
    uint32_t command_id;
    uint32_t opcode;
    uint32_t backend;
    uint32_t status;
    uint32_t result;
    uint32_t q_head;
    uint64_t rsvd0;
};
```

Userspace receives a UAPI completion that also includes the latest queue error:

```c
struct virt_llm_user_cpl {
    __u32 command_id;
    __u32 opcode;
    __u32 backend;
    __u32 status;
    __u32 result;
    __u32 q_head;
    __u32 q_error;
    __u32 reserved;
};
```

### Submission Sequence

The normal command flow is:

1. Guest userspace allocates one or more DMA buffers with
   `VIRT_LLM_IOCTL_ALLOC_BUFFER`.
2. Userspace maps the buffers and writes request/input data.
3. Userspace submits `virt_llm_user_desc` with `VIRT_LLM_IOCTL_SUBMIT_DESC`.
4. The Linux driver translates handles to DMA addresses.
5. The driver writes an SQ descriptor in coherent DMA memory.
6. The driver writes `Q_TAIL` and `COMMAND=KICK` through BAR0.
7. QEMU reads the descriptor through `pci_dma_read()`.
8. QEMU dispatches the opcode to a backend.
9. QEMU writes output data through `pci_dma_write()`.
10. QEMU writes descriptor status and a CQ entry.
11. QEMU raises completion interrupt.
12. Guest userspace waits with `VIRT_LLM_IOCTL_WAIT_CQ`.

## 6. GEMM Control Flow and Data Flow

V1.0 has two GEMM forms:

- `GEMM_U32`: simple driver selftest path.
- `GEMM_F32`: Qwen per-op path and `LM_HEAD_F32` implementation path.

### GEMM_U32 Control Flow

The driver selftest fills two 2x2 matrices:

```text
A = [[1, 2],
     [3, 4]]

B = [[5, 6],
     [7, 8]]
```

It writes a descriptor:

```c
desc->opcode = VIRT_LLM_OP_GEMM_U32;
desc->flags = VIRT_LLM_DESC_F_READY;
desc->input_addr = vdev->input_dma;
desc->output_addr = vdev->output_dma;
desc->rsvd0 = 6;
desc->rsvd1 = vdev->input_b_dma;
desc->rsvd2 = 2 | (2ULL << 16) | (2ULL << 32);
```

`rsvd2` packs dimensions as:

```text
m = bits [15:0]
n = bits [31:16]
k = bits [47:32]
```

The driver then writes `Q_TAIL` and `COMMAND=KICK`.  QEMU dispatches
`GEMM_U32` to `VIRT_LLM_BACKEND_TENSOR`.

### GEMM_U32 Data Flow

The device data flow is:

```text
guest input_dma     -- pci_dma_read --> QEMU heap A
guest input_b_dma   -- pci_dma_read --> QEMU heap B
QEMU host CPU GEMM  -- compute      --> QEMU heap C
QEMU heap C         -- pci_dma_write -> guest output_dma
```

The calculation is:

```text
C[0,0] = 1*5 + 2*7 = 19
C[0,1] = 1*6 + 2*8 = 22
C[1,0] = 3*5 + 4*7 = 43
C[1,1] = 3*6 + 4*8 = 50
```

Checksum is:

```text
19 + 22 + 43 + 50 = 134 = 0x00000086
```

The device writes:

- `desc.status = DESC_COMPLETE`
- `desc.result = 0x86`
- CQ entry with `backend = TENSOR`

### GEMM_F32 Data Flow

`GEMM_F32` receives a `virt_llm_tensor_req` at the start of the input DMA
buffer:

```c
struct virt_llm_tensor_req {
    __u32 abi;
    __u32 dtype;
    __u32 rank;
    __u32 flags;
    __u32 layer_id;
    __u32 tensor_id;
    __u32 aux_tensor_id;
    __u32 dims[4];
    __u32 input_offset;
    __u32 weight_offset;
    __u32 aux_offset;
    __u32 output_offset;
    __u32 input2_offset;
    __u64 scalar0_bits;
    __u64 scalar1_bits;
};
```

For Qwen projection GEMM:

- `input_addr + input_offset`: activation matrix A.
- `tensor_id`: Qwen weight tensor, lazily loaded from safetensors.
- `aux_tensor_id`: optional Q/K/V bias tensor.
- `dims[0..2]`: `m`, `n`, `k`.
- `output_addr + output_offset`: output matrix C.

If `tensor_id` is non-zero, QEMU reads the weight from safetensors, converts
BF16 to FP32, and transposes HF layout `[out, in]` to runtime layout
`[in, out]`.

The calculation is:

```text
C[m, n] = A[m, k] * B[k, n] + optional_bias[n]
```

Like `GEMM_U32`, the implementation is copy-in, host CPU compute, copy-out.

## 7. Software Architecture

### QEMU Device Model

Location:

```text
/home/qemu/qemu/hw/misc/virt_llm.c
```

Responsibilities:

- PCIe endpoint realization.
- BAR0 MMIO implementation.
- MSI/MSI-X/INTx interrupt handling.
- SQ/CQ processing.
- Descriptor validation and opcode dispatch.
- DMA read/write simulation.
- Scalar/vector/tensor backend execution.
- Qwen safetensors metadata parsing.
- Lazy BF16 weight loading and FP32 conversion.

### Linux Kernel Driver

Location:

```text
/home/qemu/linux-6.12/drivers/misc/virt_llm_pci.c
```

Responsibilities:

- PCI probe and BAR mapping.
- DMA queue and CQ allocation.
- Interrupt setup and completion handling.
- Probe-time hardware selftests.
- `/dev/virt_llm0` misc char device.
- Userspace ioctls:
  - `GET_INFO`
  - `ALLOC_BUFFER`
  - `FREE_BUFFER`
  - `SUBMIT_DESC`
  - `WAIT_CQ`
- mmap of coherent DMA buffers into guest userspace.

### Linux UAPI Header

Location:

```text
/home/qemu/linux-6.12/include/uapi/linux/virt_llm.h
```

Responsibilities:

- Opcode definitions.
- Backend definitions.
- Descriptor/completion structures.
- Tensor request ABI.
- Qwen model constants and tensor id constants.

### Guest Test and Runtime Programs

Locations:

```text
/home/qemu/linux-6.12/tools/testing/selftests/virt_llm/virt-llm-test.c
/home/qemu/linux-6.12/tools/testing/selftests/virt_llm/virt-llm-qwen.c
```

Responsibilities:

- `virt-llm-test.c`: basic info, model load/query, GEMM, attention, primitive
  FP32 op validation.
- `virt-llm-qwen.c`: freestanding RISC-V init program that submits a Qwen
  per-op graph and reports the strict `QWEN_INFER_OK` true-inference marker
  once the generated token matches the host golden fixture.

### Host Run Scripts

Locations:

```text
$QEMU_SRC/run_virt_llm_validation.sh
/home/qemu/qemu/tools/virt_llm/run_virt_llm_qwen.sh
$QEMU_SRC/run_virt_llm_qwen.sh
```

Responsibilities:

- Build guest initramfs payloads.
- Launch `qemu-system-riscv32`.
- Pass `-device virt-llm`.
- Capture and filter validation logs.

## 8. Running the GEMM Demonstration

### Build QEMU

```bash
cd /home/qemu/qemu/build
ninja qemu-system-riscv32
```

### Build Linux 6.12 RISC-V Image

```bash
cd /home/qemu/linux-6.12
make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- \
  O=/home/qemu/linux-6.12-build-rv32 -j32 Image
```

### Run the Automated Validation

From WSL:

```bash
cd $QEMU_SRC
./run_virt_llm_validation.sh
```

The script boots Linux 6.12 on QEMU RISC-V32 with:

```text
-machine virt
-cpu rv32
-m 256M
-device virt-llm
```

### Expected GEMM Output

A successful run includes:

```text
virt_llm_pci 0000:00:01.0: gemm ok: m=2 n=2 k=2 checksum=0x00000086
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```

The GEMM line means:

- The guest Linux driver found the `virt-llm` PCI device.
- The driver configured SQ/CQ DMA memory and interrupts.
- The driver submitted a `GEMM_U32` command.
- QEMU dispatched it to the tensor backend.
- QEMU DMA-read A and B from guest memory.
- QEMU computed the 2x2 GEMM.
- QEMU DMA-wrote C to guest memory.
- The completion queue reported `DESC_COMPLETE`.
- The checksum matched `0x86`, which is decimal `134`.

For the selftest matrices:

```text
A = [[1, 2], [3, 4]]
B = [[5, 6], [7, 8]]
C = [[19, 22], [43, 50]]
sum(C) = 134 = 0x86
```

### Log Location

The validation script reports the log path at the end.  The current default is:

```text
/home/qemu/virt-llm-riscv32-linux-6.12.log
```

To inspect only the GEMM-related line:

```bash
grep 'gemm ok' /home/qemu/virt-llm-riscv32-linux-6.12.log
```

## V1.0 Scope Summary

V1.0 establishes the end-to-end software/hardware interface:

- PCIe device enumeration.
- BAR0 MMIO control plane.
- Guest coherent DMA buffer data plane.
- Submit queue and completion queue.
- Interrupt-driven completion.
- Scalar, vector, and tensor backend split.
- Basic tensor backend GEMM demonstration.
- Qwen2.5-0.5B per-op functional smoke path.

V1.0 deliberately leaves device DDR/SRAM, performance timing, KV cache
residency, quantized kernels, and strict Hugging Face golden alignment for later
milestones.


## Environment Portability Notes

V1.0 originally used the local development layout under `/home/qemu` and a local Qwen model under `/home/zyz/llmsim`. Current `llmdev` scripts keep those values as defaults, but they are now configuration defaults rather than hard requirements.

### Configurable Variables

| Variable | Purpose | Default |
|---|---|---|
| `QEMU_SRC` | QEMU source tree containing `hw/misc/virt_llm.c` | directory two levels above `tools/virt_llm` |
| `QEMU_BUILD` | QEMU build directory | `$QEMU_SRC/build` |
| `QEMU_BIN` | RISC-V QEMU executable | `$QEMU_BUILD/qemu-system-riscv32` |
| `LINUX_SRC` | Linux 6.12 source tree containing `virt_llm_pci.c` | `/home/qemu/linux-6.12` |
| `LINUX_BUILD` | Linux riscv32 out-of-tree build directory | `/home/qemu/linux-6.12-build-rv32` |
| `LINUX_IMAGE` | Guest kernel image | `$LINUX_BUILD/arch/riscv/boot/Image` |
| `CROSS_COMPILE` | RISC-V cross compiler prefix | `riscv64-linux-gnu-` |
| `VIRT_LLM_MODEL_PATH` | Qwen safetensors file used by `MODEL_LOAD` | `/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors` |
| `VIRT_LLM_ARTIFACT_DIR` | Generated initramfs and helper artifacts | `$QEMU_BUILD/virt-llm-artifacts` |
| `VIRT_LLM_LOG_DIR` | QEMU validation logs | `$VIRT_LLM_ARTIFACT_DIR/logs` |

Use `tools/virt_llm/env.example` as a starting point. The QEMU device also exposes the model path directly as a device property:

```bash
-device virt-llm,model-path=/path/to/qwen2.5-0.5b-instruct/model.safetensors
```

### Portable Entry Points

The repository now carries the helper entry points that were previously only present in the Windows-side working directory:

```bash
/home/qemu/qemu/tools/virt_llm/build_linux_6_12_rv32.sh
/home/qemu/qemu/tools/virt_llm/build_initramfs.sh --mode test|console|qwen
/home/qemu/qemu/tools/virt_llm/run_virt_llm_validation.sh
/home/qemu/qemu/tools/virt_llm/run_virt_llm_console.sh
/home/qemu/qemu/tools/virt_llm/run_virt_llm_qwen.sh
```

The validation and Qwen scripts generate their initramfs inputs before booting, check required binaries and files up front, and write logs under `VIRT_LLM_LOG_DIR`.

### Minimal Qwen True-Inference Validation

To run the strict Qwen path, provide the local safetensors file:

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
  /home/qemu/qemu/tools/virt_llm/run_virt_llm_qwen.sh
```

The required success markers are:

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ...
```

The older `QWEN_SINGLE_TOKEN_OK` and `QWEN_DECODE_OK` markers indicate only the
legacy smoke path and do not satisfy the strict true-inference gate.

For fresh-clone reproduction, the Windows-side helper keeps the basic validation
unchanged.  It runs the Qwen true-inference gate only when the caller explicitly
sets `VIRT_LLM_MODEL_PATH`; otherwise it records the Qwen check as skipped.

### Remaining Local Inputs

The Qwen model file is intentionally not checked into GitHub. Clone users must provide a compatible local safetensors file and set `VIRT_LLM_MODEL_PATH` or pass the `model-path` device property. QEMU build directories are also not portable; after cloning, run QEMU configure/build again instead of reusing an existing `build/pyvenv`.
