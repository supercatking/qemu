/*
 * Minimal virtual PCIe LLM accelerator prototype.
 *
 * This is intentionally tiny: it only proves PCI enumeration, BAR mapping,
 * MMIO read/write, and a device-side state response.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qom/object.h"
#include <math.h>

#define TYPE_VIRT_LLM "virt-llm"
OBJECT_DECLARE_SIMPLE_TYPE(VirtLLMState, VIRT_LLM)

#define VIRT_LLM_DEVICE_ID 0x1100

#define VIRT_LLM_REG_MAGIC    0x00
#define VIRT_LLM_REG_VERSION  0x04
#define VIRT_LLM_REG_DOORBELL 0x08
#define VIRT_LLM_REG_STATUS   0x0c
#define VIRT_LLM_REG_FEATURES 0x10
#define VIRT_LLM_REG_Q_SIZE   0x14
#define VIRT_LLM_REG_Q_LO     0x18
#define VIRT_LLM_REG_Q_HI     0x1c
#define VIRT_LLM_REG_Q_HEAD   0x20
#define VIRT_LLM_REG_Q_TAIL   0x24
#define VIRT_LLM_REG_IRQ_STS  0x28
#define VIRT_LLM_REG_IRQ_MASK 0x2c
#define VIRT_LLM_REG_COMMAND  0x30
#define VIRT_LLM_REG_ABI      0x34
#define VIRT_LLM_REG_Q_MAX    0x38
#define VIRT_LLM_REG_XFER_MAX 0x3c
#define VIRT_LLM_REG_IRQ_VEC  0x40
#define VIRT_LLM_REG_Q_CTRL   0x44
#define VIRT_LLM_REG_Q_STATUS 0x48
#define VIRT_LLM_REG_Q_ERROR  0x4c
#define VIRT_LLM_REG_CQ_SIZE  0x50
#define VIRT_LLM_REG_CQ_LO    0x54
#define VIRT_LLM_REG_CQ_HI    0x58
#define VIRT_LLM_REG_CQ_HEAD  0x5c
#define VIRT_LLM_REG_CQ_TAIL  0x60
#define VIRT_LLM_REG_SCALAR_STATUS 0x64
#define VIRT_LLM_REG_SCALAR_KERNELS 0x68
#define VIRT_LLM_REG_SCALAR_LAST_KERNEL 0x6c
#define VIRT_LLM_REG_SCALAR_LAST_OPCODE 0x70
#define VIRT_LLM_REG_KERNEL_INDEX 0x74
#define VIRT_LLM_REG_KERNEL_ID 0x78
#define VIRT_LLM_REG_KERNEL_OPCODE 0x7c
#define VIRT_LLM_REG_KERNEL_ABI 0x80
#define VIRT_LLM_REG_KERNEL_ENTRY 0x84
#define VIRT_LLM_REG_KERNEL_SIZE 0x88
#define VIRT_LLM_REG_KERNEL_CHECKSUM 0x8c

#define VIRT_LLM_MAGIC        0x4c4c4d31u /* "LLM1" */
#define VIRT_LLM_VERSION      3u
#define VIRT_LLM_ABI_VERSION  1u
#define VIRT_LLM_STATUS_XOR   0xa5a5a5a5u
#define VIRT_LLM_MMIO_SIZE    4 * KiB
#define VIRT_LLM_MSIX_VECTORS 1
#define VIRT_LLM_MAX_QUEUE    1024

#define VIRT_LLM_FEATURE_QUEUE BIT(0)
#define VIRT_LLM_FEATURE_MSI   BIT(1)
#define VIRT_LLM_FEATURE_MSIX  BIT(2)
#define VIRT_LLM_FEATURE_QCTRL BIT(3)
#define VIRT_LLM_FEATURE_CQ    BIT(4)
#define VIRT_LLM_FEATURE_SCALAR BIT(5)

#define VIRT_LLM_IRQ_COMPLETE  BIT(0)
#define VIRT_LLM_IRQ_ERROR     BIT(1)
#define VIRT_LLM_IRQ_ALL       (VIRT_LLM_IRQ_COMPLETE | VIRT_LLM_IRQ_ERROR)
#define VIRT_LLM_CMD_KICK      1
#define VIRT_LLM_OP_INFER_XOR  0x0001
#define VIRT_LLM_OP_DMA_COPY   0x0010
#define VIRT_LLM_OP_VEC_ADD_U32 0x0100
#define VIRT_LLM_OP_SOFTMAX_Q16 0x0101
#define VIRT_LLM_OP_POOL_MAX_U32 0x0102
#define VIRT_LLM_OP_DOT_U32    0x0103
#define VIRT_LLM_OP_GEMM_U32   0x0200
#define VIRT_LLM_OP_CONV2D_U32 0x0201
#define VIRT_LLM_OP_ATTENTION_Q16 0x0202
#define VIRT_LLM_OP_MODEL_LOAD 0x0300
#define VIRT_LLM_OP_MODEL_QUERY 0x0301
#define VIRT_LLM_OP_EMBED_LOOKUP_F32 0x0310
#define VIRT_LLM_OP_RMSNORM_F32 0x0311
#define VIRT_LLM_OP_ROPE_F32 0x0312
#define VIRT_LLM_OP_GEMM_F32 0x0313
#define VIRT_LLM_OP_ADD_F32 0x0314
#define VIRT_LLM_OP_SWIGLU_F32 0x0315
#define VIRT_LLM_OP_QWEN_GQA_ATTENTION_F32 0x0316
#define VIRT_LLM_OP_LM_HEAD_F32 0x0317
#define VIRT_LLM_OP_ARGMAX_F32 0x0318
#define VIRT_LLM_DESC_F_READY  BIT(0)
#define VIRT_LLM_DESC_COMPLETE 1
#define VIRT_LLM_DESC_INVALID  0x80000001u
#define VIRT_LLM_DESC_UNSUPP   0x80000002u
#define VIRT_LLM_DESC_BAD_LEN  0x80000003u
#define VIRT_LLM_DESC_BAD_KERNEL 0x80000004u
#define VIRT_LLM_DESC_BAD_TENSOR 0x80000005u
#define VIRT_LLM_MAX_XFER      4096
#define VIRT_LLM_TENSOR_MAX_XFER (1 * GiB)
#define VIRT_LLM_Q_CTRL_ENABLE BIT(0)
#define VIRT_LLM_Q_CTRL_RESET  BIT(1)
#define VIRT_LLM_Q_STATUS_EN   BIT(0)
#define VIRT_LLM_Q_STATUS_ERR  BIT(1)
#define VIRT_LLM_Q_ERR_NONE    0u
#define VIRT_LLM_Q_ERR_CONFIG  1u
#define VIRT_LLM_Q_ERR_DESC    2u
#define VIRT_LLM_Q_ERR_OPCODE  3u
#define VIRT_LLM_Q_ERR_KERNEL  4u

#define VIRT_LLM_BACKEND_COMPAT 0u
#define VIRT_LLM_BACKEND_DMA    1u
#define VIRT_LLM_BACKEND_VECTOR 2u
#define VIRT_LLM_BACKEND_TENSOR 3u
#define VIRT_LLM_BACKEND_SCALAR 4u

#define VIRT_LLM_SCALAR_IDLE    0u
#define VIRT_LLM_SCALAR_RUNNING 1u
#define VIRT_LLM_SCALAR_ERROR   2u
#define VIRT_LLM_KERNEL_ABI_VERSION 1u
#define VIRT_LLM_KERNEL_VEC_ADD_U32 1u
#define VIRT_LLM_KERNEL_DOT_U32 2u
#define VIRT_LLM_KERNEL_SOFTMAX_Q16 3u
#define VIRT_LLM_KERNEL_POOL_MAX_U32 4u
#define VIRT_LLM_TENSOR_ABI_VERSION 1u
#define VIRT_LLM_DTYPE_U32 1u
#define VIRT_LLM_DTYPE_F32 2u
#define VIRT_LLM_DTYPE_BF16 3u
#define VIRT_LLM_TENSOR_F_CAUSAL BIT(0)
#define VIRT_LLM_QWEN_LAYERS 24u
#define VIRT_LLM_QWEN_HIDDEN 896u
#define VIRT_LLM_QWEN_HEADS 14u
#define VIRT_LLM_QWEN_KV_HEADS 2u
#define VIRT_LLM_QWEN_HEAD_DIM 64u
#define VIRT_LLM_QWEN_INTERMEDIATE 4864u
#define VIRT_LLM_QWEN_VOCAB 151936u
#define VIRT_LLM_QWEN_TENSORS (2u + VIRT_LLM_QWEN_LAYERS * 12u)
#define VIRT_LLM_DEFAULT_SAFETENSORS_PATH \
    "/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors"
#define VIRT_LLM_SAFETENSORS_HEADER_MAX (16 * MiB)
#define VIRT_LLM_TENSOR_ID_EMBED 1u
#define VIRT_LLM_TENSOR_ID_FINAL_NORM 2u
#define VIRT_LLM_TENSOR_ID_LAYER_BASE 1000u
#define VIRT_LLM_TENSOR_ID_LAYER_STRIDE 16u

typedef struct VirtLLMDesc {
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
} QEMU_PACKED VirtLLMDesc;

typedef struct VirtLLMCpl {
    uint32_t command_id;
    uint32_t opcode;
    uint32_t backend;
    uint32_t status;
    uint32_t result;
    uint32_t q_head;
    uint64_t rsvd0;
} QEMU_PACKED VirtLLMCpl;

typedef struct VirtLLMKernelMeta {
    uint32_t kernel_id;
    uint32_t abi_version;
    uint32_t opcode;
    uint32_t entry_point;
    uint32_t binary_size;
    uint32_t binary_checksum;
    const char *name;
} VirtLLMKernelMeta;

typedef struct VirtLLMModelTensor {
    uint32_t tensor_id;
    uint32_t dtype;
    uint32_t rank;
    uint32_t dims[4];
    uint64_t data_begin;
    uint64_t data_end;
    char name[96];
} VirtLLMModelTensor;

typedef struct VirtLLMTensorReq {
    uint32_t abi;
    uint32_t dtype;
    uint32_t rank;
    uint32_t flags;
    uint32_t layer_id;
    uint32_t tensor_id;
    uint32_t aux_tensor_id;
    uint32_t reserved0;
    uint32_t dims[4];
    uint32_t input_offset;
    uint32_t weight_offset;
    uint32_t aux_offset;
    uint32_t output_offset;
    uint32_t input2_offset;
    uint32_t reserved1;
    uint64_t scalar0_bits;
    uint64_t scalar1_bits;
} QEMU_PACKED VirtLLMTensorReq;

typedef struct VirtLLMModelQuery {
    uint32_t abi;
    uint32_t model_loaded;
    uint32_t layers;
    uint32_t hidden_size;
    uint32_t attention_heads;
    uint32_t kv_heads;
    uint32_t head_dim;
    uint32_t intermediate_size;
    uint32_t vocab_size;
    uint32_t dtype;
    uint64_t rope_theta_bits;
    uint64_t rms_eps_bits;
} QEMU_PACKED VirtLLMModelQuery;

static const VirtLLMKernelMeta virt_llm_kernels[] = {
    {
        .kernel_id = VIRT_LLM_KERNEL_VEC_ADD_U32,
        .abi_version = VIRT_LLM_KERNEL_ABI_VERSION,
        .opcode = VIRT_LLM_OP_VEC_ADD_U32,
        .entry_point = 0x1000,
        .binary_size = 64,
        .binary_checksum = 0xadd00101,
        .name = "vec_add_u32",
    },
    {
        .kernel_id = VIRT_LLM_KERNEL_DOT_U32,
        .abi_version = VIRT_LLM_KERNEL_ABI_VERSION,
        .opcode = VIRT_LLM_OP_DOT_U32,
        .entry_point = 0x1100,
        .binary_size = 72,
        .binary_checksum = 0xd0700103,
        .name = "dot_u32",
    },
    {
        .kernel_id = VIRT_LLM_KERNEL_SOFTMAX_Q16,
        .abi_version = VIRT_LLM_KERNEL_ABI_VERSION,
        .opcode = VIRT_LLM_OP_SOFTMAX_Q16,
        .entry_point = 0x1200,
        .binary_size = 96,
        .binary_checksum = 0x50170101,
        .name = "softmax_q16",
    },
    {
        .kernel_id = VIRT_LLM_KERNEL_POOL_MAX_U32,
        .abi_version = VIRT_LLM_KERNEL_ABI_VERSION,
        .opcode = VIRT_LLM_OP_POOL_MAX_U32,
        .entry_point = 0x1300,
        .binary_size = 80,
        .binary_checksum = 0x90010102,
        .name = "pool_max_u32",
    },
};

struct VirtLLMState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint32_t doorbell;
    uint64_t queue_addr;
    uint32_t queue_size;
    uint32_t queue_head;
    uint32_t queue_tail;
    uint32_t irq_status;
    uint32_t irq_mask;
    uint32_t queue_ctrl;
    uint32_t queue_status;
    uint32_t queue_error;
    uint64_t cq_addr;
    uint32_t cq_size;
    uint32_t cq_head;
    uint32_t cq_tail;
    uint32_t scalar_status;
    uint32_t scalar_last_kernel;
    uint32_t scalar_last_opcode;
    uint32_t kernel_index;
    bool model_loaded;
    uint32_t model_tensor_count;
    uint32_t model_checksum;
    uint64_t model_data_base;
    char *model_path;
    VirtLLMModelTensor model_tensors[VIRT_LLM_QWEN_TENSORS];
};

static const VirtLLMKernelMeta *virt_llm_selected_kernel(VirtLLMState *s)
{
    if (s->kernel_index >= ARRAY_SIZE(virt_llm_kernels)) {
        return NULL;
    }

    return &virt_llm_kernels[s->kernel_index];
}

static const VirtLLMKernelMeta *virt_llm_find_kernel(uint32_t kernel_id)
{
    for (size_t i = 0; i < ARRAY_SIZE(virt_llm_kernels); i++) {
        if (virt_llm_kernels[i].kernel_id == kernel_id) {
            return &virt_llm_kernels[i];
        }
    }

    return NULL;
}

static double virt_llm_double_from_bits(uint64_t bits, double fallback)
{
    union {
        uint64_t u;
        double d;
    } v;

    if (!bits) {
        return fallback;
    }
    v.u = bits;
    return v.d;
}

static uint64_t virt_llm_double_to_bits(double d)
{
    union {
        uint64_t u;
        double d;
    } v = { .d = d };

    return v.u;
}

static bool virt_llm_tensor_req_read(VirtLLMState *s, VirtLLMDesc *desc,
                                     VirtLLMTensorReq *req)
{
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint32_t len = le32_to_cpu(desc->len);

    if (!input_addr || len < sizeof(*req)) {
        return false;
    }
    pci_dma_read(PCI_DEVICE(s), input_addr, req, sizeof(*req));
    return le32_to_cpu(req->abi) == VIRT_LLM_TENSOR_ABI_VERSION;
}

static bool virt_llm_tensor_bytes(uint64_t elems, uint64_t elem_size,
                                  uint64_t *bytes)
{
    if (!elems || elems > VIRT_LLM_TENSOR_MAX_XFER / elem_size) {
        return false;
    }
    *bytes = elems * elem_size;
    return true;
}

static uint32_t virt_llm_f32_checksum(const float *data, uint64_t elems)
{
    uint32_t checksum = 0;

    for (uint64_t i = 0; i < elems; i++) {
        checksum += (uint32_t)lrintf(fabsf(data[i]) * 1000.0f);
    }
    return checksum;
}

static float virt_llm_silu(float x)
{
    return x / (1.0f + expf(-x));
}

static void virt_llm_raise_irq(VirtLLMState *s, uint32_t cause)
{
    PCIDevice *pdev = PCI_DEVICE(s);

    s->irq_status |= cause & VIRT_LLM_IRQ_ALL;
    if (!(s->irq_mask & s->irq_status)) {
        return;
    }

    if (msix_enabled(pdev)) {
        msix_notify(pdev, 0);
    } else if (msi_enabled(pdev)) {
        msi_notify(pdev, 0);
    } else {
        pci_set_irq(pdev, 1);
    }
}

static void virt_llm_lower_intx_if_idle(VirtLLMState *s)
{
    if (!s->irq_status && !msix_enabled(PCI_DEVICE(s)) &&
        !msi_enabled(PCI_DEVICE(s))) {
        pci_set_irq(PCI_DEVICE(s), 0);
    }
}

static uint64_t virt_llm_desc_addr(VirtLLMState *s, uint32_t idx)
{
    return s->queue_addr + (idx * sizeof(VirtLLMDesc));
}

static bool virt_llm_cq_config_valid(VirtLLMState *s)
{
    return s->cq_addr &&
           s->cq_size > 0 &&
           s->cq_size <= VIRT_LLM_MAX_QUEUE &&
           QEMU_IS_ALIGNED(s->cq_addr, sizeof(VirtLLMCpl));
}

static void virt_llm_write_cq(VirtLLMState *s, VirtLLMDesc *desc,
                              uint32_t opcode, uint32_t backend,
                              uint32_t status)
{
    VirtLLMCpl cpl = { 0 };
    uint32_t idx;

    if (!virt_llm_cq_config_valid(s)) {
        return;
    }

    idx = s->cq_tail % s->cq_size;
    cpl.command_id = desc->rsvd0;
    cpl.opcode = cpu_to_le32(opcode);
    cpl.backend = cpu_to_le32(backend);
    cpl.status = cpu_to_le32(status);
    cpl.result = desc->result;
    cpl.q_head = cpu_to_le32(s->queue_head);
    pci_dma_write(PCI_DEVICE(s), s->cq_addr + idx * sizeof(cpl),
                  &cpl, sizeof(cpl));
    s->cq_tail++;
}

static void virt_llm_queue_reset(VirtLLMState *s)
{
    s->queue_addr = 0;
    s->queue_size = 0;
    s->queue_head = 0;
    s->queue_tail = 0;
    s->queue_ctrl = 0;
    s->queue_status = 0;
    s->queue_error = VIRT_LLM_Q_ERR_NONE;
    s->cq_addr = 0;
    s->cq_size = 0;
    s->cq_head = 0;
    s->cq_tail = 0;
    s->kernel_index = 0;
}

static bool virt_llm_queue_config_valid(VirtLLMState *s)
{
    return s->queue_addr &&
           s->queue_size > 0 &&
           s->queue_size <= VIRT_LLM_MAX_QUEUE &&
           QEMU_IS_ALIGNED(s->queue_addr, sizeof(VirtLLMDesc));
}

static void virt_llm_queue_error(VirtLLMState *s, uint32_t error)
{
    s->queue_error = error;
    s->queue_status |= VIRT_LLM_Q_STATUS_ERR;
    virt_llm_raise_irq(s, VIRT_LLM_IRQ_ERROR);
}

static uint32_t virt_llm_process_infer_xor(VirtLLMState *s, VirtLLMDesc *desc)
{
    g_autofree uint8_t *input = NULL;
    g_autofree uint8_t *output = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint32_t len = le32_to_cpu(desc->len);
    uint32_t checksum = 0;

    if (!input_addr || !output_addr || len == 0 || len > VIRT_LLM_MAX_XFER) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    input = g_malloc(len);
    output = g_malloc(len);
    pci_dma_read(PCI_DEVICE(s), input_addr, input, len);

    for (uint32_t i = 0; i < len; i++) {
        output[i] = input[i] ^ 0x5a;
        checksum += output[i];
    }

    pci_dma_write(PCI_DEVICE(s), output_addr, output, len);
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_dma_copy(VirtLLMState *s, VirtLLMDesc *desc)
{
    g_autofree uint8_t *buf = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint32_t len = le32_to_cpu(desc->len);
    uint32_t checksum = 0;

    if (!input_addr || !output_addr || len == 0 || len > VIRT_LLM_MAX_XFER) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    buf = g_malloc(len);
    pci_dma_read(PCI_DEVICE(s), input_addr, buf, len);
    for (uint32_t i = 0; i < len; i++) {
        checksum += buf[i];
    }
    pci_dma_write(PCI_DEVICE(s), output_addr, buf, len);
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_vec_add_u32(VirtLLMState *s,
                                             VirtLLMDesc *desc)
{
    g_autofree uint32_t *a = NULL;
    g_autofree uint32_t *b = NULL;
    g_autofree uint32_t *out = NULL;
    uint64_t a_addr = le64_to_cpu(desc->input_addr);
    uint64_t b_addr = le64_to_cpu(desc->rsvd1);
    uint64_t out_addr = le64_to_cpu(desc->output_addr);
    uint32_t count = le32_to_cpu(desc->len);
    uint32_t checksum = 0;
    size_t bytes;

    if (!a_addr || !b_addr || !out_addr || count == 0 ||
        count > VIRT_LLM_MAX_XFER / sizeof(uint32_t)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    bytes = count * sizeof(uint32_t);
    a = g_malloc(bytes);
    b = g_malloc(bytes);
    out = g_malloc(bytes);
    pci_dma_read(PCI_DEVICE(s), a_addr, a, bytes);
    pci_dma_read(PCI_DEVICE(s), b_addr, b, bytes);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = le32_to_cpu(a[i]) + le32_to_cpu(b[i]);

        out[i] = cpu_to_le32(v);
        checksum += v;
    }

    pci_dma_write(PCI_DEVICE(s), out_addr, out, bytes);
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_dot_u32(VirtLLMState *s, VirtLLMDesc *desc)
{
    g_autofree uint32_t *a = NULL;
    g_autofree uint32_t *b = NULL;
    uint64_t a_addr = le64_to_cpu(desc->input_addr);
    uint64_t b_addr = le64_to_cpu(desc->rsvd1);
    uint32_t count = le32_to_cpu(desc->len);
    uint32_t dot = 0;
    size_t bytes;

    if (!a_addr || !b_addr || count == 0 ||
        count > VIRT_LLM_MAX_XFER / sizeof(uint32_t)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    bytes = count * sizeof(uint32_t);
    a = g_malloc(bytes);
    b = g_malloc(bytes);
    pci_dma_read(PCI_DEVICE(s), a_addr, a, bytes);
    pci_dma_read(PCI_DEVICE(s), b_addr, b, bytes);

    for (uint32_t i = 0; i < count; i++) {
        dot += le32_to_cpu(a[i]) * le32_to_cpu(b[i]);
    }

    desc->result = cpu_to_le32(dot);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_softmax_q16(VirtLLMState *s,
                                             VirtLLMDesc *desc)
{
    g_autofree uint32_t *input = NULL;
    g_autofree uint32_t *output = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint32_t count = le32_to_cpu(desc->len);
    uint64_t sum = 0;
    uint32_t checksum = 0;
    size_t bytes;

    if (!input_addr || !output_addr || count == 0 ||
        count > VIRT_LLM_MAX_XFER / sizeof(uint32_t)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    bytes = count * sizeof(uint32_t);
    input = g_malloc(bytes);
    output = g_malloc(bytes);
    pci_dma_read(PCI_DEVICE(s), input_addr, input, bytes);

    for (uint32_t i = 0; i < count; i++) {
        sum += le32_to_cpu(input[i]);
    }
    if (!sum) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t q16 = (le32_to_cpu(input[i]) << 16) / sum;

        output[i] = cpu_to_le32(q16);
        checksum += q16;
    }

    pci_dma_write(PCI_DEVICE(s), output_addr, output, bytes);
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_pool_max_u32(VirtLLMState *s,
                                              VirtLLMDesc *desc)
{
    g_autofree uint32_t *input = NULL;
    g_autofree uint32_t *output = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint64_t args = le64_to_cpu(desc->rsvd2);
    uint32_t count = le32_to_cpu(desc->len);
    uint32_t window = extract64(args, 0, 16);
    uint32_t out_count;
    uint32_t checksum = 0;
    size_t input_bytes;
    size_t output_bytes;

    if (!input_addr || !output_addr || count == 0 || window == 0 ||
        count % window ||
        count > VIRT_LLM_MAX_XFER / sizeof(uint32_t)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    out_count = count / window;
    input_bytes = count * sizeof(uint32_t);
    output_bytes = out_count * sizeof(uint32_t);
    input = g_malloc(input_bytes);
    output = g_malloc(output_bytes);
    pci_dma_read(PCI_DEVICE(s), input_addr, input, input_bytes);

    for (uint32_t o = 0; o < out_count; o++) {
        uint32_t max = 0;

        for (uint32_t i = 0; i < window; i++) {
            uint32_t v = le32_to_cpu(input[o * window + i]);

            if (i == 0 || v > max) {
                max = v;
            }
        }
        output[o] = cpu_to_le32(max);
        checksum += max;
    }

    pci_dma_write(PCI_DEVICE(s), output_addr, output, output_bytes);
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_kernel_id(VirtLLMDesc *desc)
{
    return extract32(le64_to_cpu(desc->rsvd3), 0, 16);
}

static uint32_t virt_llm_kernel_abi(VirtLLMDesc *desc)
{
    uint32_t abi = extract32(le64_to_cpu(desc->rsvd3), 16, 8);

    return abi ? abi : VIRT_LLM_KERNEL_ABI_VERSION;
}

static uint32_t virt_llm_scalar_dispatch(VirtLLMState *s, VirtLLMDesc *desc,
                                         uint32_t opcode)
{
    uint32_t kernel_id = virt_llm_kernel_id(desc);
    uint32_t kernel_abi = virt_llm_kernel_abi(desc);
    const VirtLLMKernelMeta *kernel = virt_llm_find_kernel(kernel_id);
    uint32_t status;

    s->scalar_status = VIRT_LLM_SCALAR_RUNNING;
    s->scalar_last_kernel = kernel_id;
    s->scalar_last_opcode = opcode;

    if (!kernel || kernel->opcode != opcode ||
        kernel->abi_version != kernel_abi) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "virt-llm: bad scalar kernel id=%u abi=%u opcode=0x%04x\n",
                      kernel_id, kernel_abi, opcode);
        status = VIRT_LLM_DESC_BAD_KERNEL;
        goto out;
    }

    switch (opcode) {
    case VIRT_LLM_OP_VEC_ADD_U32:
        status = virt_llm_process_vec_add_u32(s, desc);
        break;
    case VIRT_LLM_OP_DOT_U32:
        status = virt_llm_process_dot_u32(s, desc);
        break;
    case VIRT_LLM_OP_SOFTMAX_Q16:
        status = virt_llm_process_softmax_q16(s, desc);
        break;
    case VIRT_LLM_OP_POOL_MAX_U32:
        status = virt_llm_process_pool_max_u32(s, desc);
        break;
    default:
        status = VIRT_LLM_DESC_UNSUPP;
        break;
    }

out:
    s->scalar_status = status == VIRT_LLM_DESC_COMPLETE ?
                       VIRT_LLM_SCALAR_IDLE : VIRT_LLM_SCALAR_ERROR;
    return status;
}

static uint32_t virt_llm_process_gemm_u32(VirtLLMState *s, VirtLLMDesc *desc)
{
    g_autofree uint32_t *a = NULL;
    g_autofree uint32_t *b = NULL;
    g_autofree uint32_t *c = NULL;
    uint64_t a_addr = le64_to_cpu(desc->input_addr);
    uint64_t b_addr = le64_to_cpu(desc->rsvd1);
    uint64_t c_addr = le64_to_cpu(desc->output_addr);
    uint64_t dims = le64_to_cpu(desc->rsvd2);
    uint32_t m = extract64(dims, 0, 16);
    uint32_t n = extract64(dims, 16, 16);
    uint32_t k = extract64(dims, 32, 16);
    uint32_t checksum = 0;
    uint64_t a_elems = (uint64_t)m * k;
    uint64_t b_elems = (uint64_t)k * n;
    uint64_t c_elems = (uint64_t)m * n;

    if (!a_addr || !b_addr || !c_addr || !m || !n || !k ||
        a_elems > VIRT_LLM_MAX_XFER / sizeof(uint32_t) ||
        b_elems > VIRT_LLM_MAX_XFER / sizeof(uint32_t) ||
        c_elems > VIRT_LLM_MAX_XFER / sizeof(uint32_t)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    a = g_malloc(a_elems * sizeof(uint32_t));
    b = g_malloc(b_elems * sizeof(uint32_t));
    c = g_new0(uint32_t, c_elems);
    pci_dma_read(PCI_DEVICE(s), a_addr, a, a_elems * sizeof(uint32_t));
    pci_dma_read(PCI_DEVICE(s), b_addr, b, b_elems * sizeof(uint32_t));

    for (uint32_t row = 0; row < m; row++) {
        for (uint32_t col = 0; col < n; col++) {
            uint32_t sum = 0;

            for (uint32_t inner = 0; inner < k; inner++) {
                sum += le32_to_cpu(a[row * k + inner]) *
                       le32_to_cpu(b[inner * n + col]);
            }
            c[row * n + col] = cpu_to_le32(sum);
            checksum += sum;
        }
    }

    pci_dma_write(PCI_DEVICE(s), c_addr, c, c_elems * sizeof(uint32_t));
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_conv2d_u32(VirtLLMState *s, VirtLLMDesc *desc)
{
    g_autofree uint32_t *input = NULL;
    g_autofree uint32_t *kernel = NULL;
    g_autofree uint32_t *output = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t kernel_addr = le64_to_cpu(desc->rsvd1);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint64_t in_dims = le64_to_cpu(desc->rsvd2);
    uint64_t out_dims = le64_to_cpu(desc->rsvd3);
    uint32_t in_h = extract64(in_dims, 0, 16);
    uint32_t in_w = extract64(in_dims, 16, 16);
    uint32_t k_h = extract64(in_dims, 32, 16);
    uint32_t k_w = extract64(in_dims, 48, 16);
    uint32_t out_h = extract64(out_dims, 0, 16);
    uint32_t out_w = extract64(out_dims, 16, 16);
    uint32_t checksum = 0;
    uint64_t input_elems = (uint64_t)in_h * in_w;
    uint64_t kernel_elems = (uint64_t)k_h * k_w;
    uint64_t output_elems = (uint64_t)out_h * out_w;

    if (!input_addr || !kernel_addr || !output_addr ||
        !in_h || !in_w || !k_h || !k_w || !out_h || !out_w ||
        k_h > in_h || k_w > in_w ||
        out_h != in_h - k_h + 1 || out_w != in_w - k_w + 1 ||
        input_elems > VIRT_LLM_MAX_XFER / sizeof(uint32_t) ||
        kernel_elems > VIRT_LLM_MAX_XFER / sizeof(uint32_t) ||
        output_elems > VIRT_LLM_MAX_XFER / sizeof(uint32_t)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    input = g_malloc(input_elems * sizeof(uint32_t));
    kernel = g_malloc(kernel_elems * sizeof(uint32_t));
    output = g_new0(uint32_t, output_elems);
    pci_dma_read(PCI_DEVICE(s), input_addr, input,
                 input_elems * sizeof(uint32_t));
    pci_dma_read(PCI_DEVICE(s), kernel_addr, kernel,
                 kernel_elems * sizeof(uint32_t));

    for (uint32_t oy = 0; oy < out_h; oy++) {
        for (uint32_t ox = 0; ox < out_w; ox++) {
            uint32_t sum = 0;

            for (uint32_t ky = 0; ky < k_h; ky++) {
                for (uint32_t kx = 0; kx < k_w; kx++) {
                    uint32_t iv = le32_to_cpu(input[(oy + ky) * in_w + ox + kx]);
                    uint32_t kv = le32_to_cpu(kernel[ky * k_w + kx]);

                    sum += iv * kv;
                }
            }
            output[oy * out_w + ox] = cpu_to_le32(sum);
            checksum += sum;
        }
    }

    pci_dma_write(PCI_DEVICE(s), output_addr, output,
                  output_elems * sizeof(uint32_t));
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_exp_neg_q16(uint64_t delta)
{
    static const uint32_t lut[] = {
        65536, 24109, 8869, 3263, 1201, 442, 163, 60, 22,
    };
    uint32_t whole = delta >> 16;
    uint32_t frac = delta & 0xffff;
    uint64_t frac_sq = ((uint64_t)frac * frac) >> 16;
    uint32_t frac_exp = 65536 - frac + (frac_sq >> 1);

    if (whole >= ARRAY_SIZE(lut)) {
        return 0;
    }

    return ((uint64_t)lut[whole] * frac_exp + 0x8000) >> 16;
}

static uint32_t virt_llm_process_attention_q16(VirtLLMState *s,
                                               VirtLLMDesc *desc)
{
    g_autofree uint32_t *q = NULL;
    g_autofree uint32_t *k = NULL;
    g_autofree uint32_t *v = NULL;
    g_autofree uint32_t *out = NULL;
    uint64_t q_addr = le64_to_cpu(desc->input_addr);
    uint64_t k_addr = le64_to_cpu(desc->rsvd1);
    uint64_t v_addr = le64_to_cpu(desc->rsvd2);
    uint64_t out_addr = le64_to_cpu(desc->output_addr);
    uint32_t dims = le32_to_cpu(desc->len);
    uint32_t seq_len = extract32(dims, 0, 16);
    uint32_t head_dim = extract32(dims, 16, 16);
    uint32_t elems = seq_len * head_dim;
    uint32_t checksum = 0;
    size_t bytes;

    if (!q_addr || !k_addr || !v_addr || !out_addr ||
        !seq_len || !head_dim || seq_len > 8 || head_dim > 8 ||
        elems > VIRT_LLM_MAX_XFER / sizeof(uint32_t)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    bytes = elems * sizeof(uint32_t);
    q = g_malloc(bytes);
    k = g_malloc(bytes);
    v = g_malloc(bytes);
    out = g_new0(uint32_t, elems);
    pci_dma_read(PCI_DEVICE(s), q_addr, q, bytes);
    pci_dma_read(PCI_DEVICE(s), k_addr, k, bytes);
    pci_dma_read(PCI_DEVICE(s), v_addr, v, bytes);

    for (uint32_t row = 0; row < seq_len; row++) {
        uint64_t scores[8] = { 0 };
        uint32_t weights[8] = { 0 };
        uint64_t max_score = 0;
        uint64_t weight_sum = 0;
        uint32_t cols = row + 1;

        for (uint32_t col = 0; col < cols; col++) {
            uint64_t acc = 0;

            for (uint32_t d = 0; d < head_dim; d++) {
                uint64_t qv = le32_to_cpu(q[row * head_dim + d]);
                uint64_t kv = le32_to_cpu(k[col * head_dim + d]);

                acc += (qv * kv) >> 16;
            }
            scores[col] = acc;
            if (col == 0 || scores[col] > max_score) {
                max_score = scores[col];
            }
        }

        for (uint32_t col = 0; col < cols; col++) {
            uint64_t diff = max_score - scores[col];

            weights[col] = virt_llm_exp_neg_q16(diff);
            weight_sum += weights[col];
        }

        if (!weight_sum) {
            return VIRT_LLM_DESC_BAD_LEN;
        }

        for (uint32_t d = 0; d < head_dim; d++) {
            uint64_t sum = 0;

            for (uint32_t col = 0; col < cols; col++) {
                uint64_t prob = ((uint64_t)weights[col] << 16) / weight_sum;
                uint64_t vv = le32_to_cpu(v[col * head_dim + d]);

                sum += (prob * vv) >> 16;
            }
            sum = MIN(sum, (uint64_t)UINT32_MAX);
            out[row * head_dim + d] = cpu_to_le32((uint32_t)sum);
            checksum += (uint32_t)sum;
        }
    }

    pci_dma_write(PCI_DEVICE(s), out_addr, out, bytes);
    desc->result = cpu_to_le32(checksum);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_model_checksum_mix(uint32_t checksum, uint64_t value)
{
    checksum ^= (uint32_t)value;
    checksum *= 16777619u;
    checksum ^= (uint32_t)(value >> 32);
    checksum *= 16777619u;
    return checksum;
}

static const char *virt_llm_json_skip_ws(const char *p, const char *end)
{
    while (p < end && g_ascii_isspace(*p)) {
        p++;
    }
    return p;
}

static const char *virt_llm_json_find_token(const char *json, const char *end,
                                            const char *token)
{
    size_t token_len = strlen(token);

    for (const char *p = json; p + token_len <= end; p++) {
        if (!memcmp(p, token, token_len)) {
            return p;
        }
    }
    return NULL;
}

static bool virt_llm_json_object_for_key(const char *json, size_t json_len,
                                         const char *key, const char **obj,
                                         const char **obj_end)
{
    g_autofree char *needle = g_strdup_printf("\"%s\"", key);
    const char *end = json + json_len;
    const char *p = virt_llm_json_find_token(json, end, needle);
    bool in_string = false;
    bool escaped = false;
    uint32_t depth = 0;

    if (!p) {
        return false;
    }
    p += strlen(needle);
    p = virt_llm_json_skip_ws(p, end);
    if (p >= end || *p != ':') {
        return false;
    }
    p = virt_llm_json_skip_ws(p + 1, end);
    if (p >= end || *p != '{') {
        return false;
    }

    *obj = p;
    for (; p < end; p++) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (*p == '\\') {
                escaped = true;
            } else if (*p == '"') {
                in_string = false;
            }
            continue;
        }
        if (*p == '"') {
            in_string = true;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (!depth) {
                return false;
            }
            depth--;
            if (!depth) {
                *obj_end = p + 1;
                return true;
            }
        }
    }
    return false;
}

static bool virt_llm_json_field_string(const char *obj, const char *obj_end,
                                       const char *field, char *out,
                                       size_t out_size)
{
    g_autofree char *needle = g_strdup_printf("\"%s\"", field);
    const char *p = virt_llm_json_find_token(obj, obj_end, needle);
    const char *start;
    size_t len;

    if (!p || out_size == 0) {
        return false;
    }
    p += strlen(needle);
    p = virt_llm_json_skip_ws(p, obj_end);
    if (p >= obj_end || *p != ':') {
        return false;
    }
    p = virt_llm_json_skip_ws(p + 1, obj_end);
    if (p >= obj_end || *p != '"') {
        return false;
    }
    start = ++p;
    while (p < obj_end && *p != '"') {
        if (*p == '\\') {
            return false;
        }
        p++;
    }
    if (p >= obj_end) {
        return false;
    }
    len = p - start;
    if (len >= out_size) {
        return false;
    }
    memcpy(out, start, len);
    out[len] = 0;
    return true;
}

static bool virt_llm_json_parse_u64(const char **p, const char *end,
                                    uint64_t *value)
{
    uint64_t v = 0;
    bool any = false;

    *p = virt_llm_json_skip_ws(*p, end);
    while (*p < end && g_ascii_isdigit(**p)) {
        uint32_t digit = **p - '0';

        if (v > (UINT64_MAX - digit) / 10) {
            return false;
        }
        v = v * 10 + digit;
        (*p)++;
        any = true;
    }
    *value = v;
    return any;
}

static bool virt_llm_json_field_u64_array(const char *obj, const char *obj_end,
                                          const char *field, uint64_t *values,
                                          uint32_t max_values,
                                          uint32_t *count)
{
    g_autofree char *needle = g_strdup_printf("\"%s\"", field);
    const char *p = virt_llm_json_find_token(obj, obj_end, needle);
    uint32_t n = 0;

    if (!p) {
        return false;
    }
    p += strlen(needle);
    p = virt_llm_json_skip_ws(p, obj_end);
    if (p >= obj_end || *p != ':') {
        return false;
    }
    p = virt_llm_json_skip_ws(p + 1, obj_end);
    if (p >= obj_end || *p != '[') {
        return false;
    }
    p++;
    for (;;) {
        p = virt_llm_json_skip_ws(p, obj_end);
        if (p >= obj_end) {
            return false;
        }
        if (*p == ']') {
            p++;
            break;
        }
        if (n >= max_values ||
            !virt_llm_json_parse_u64(&p, obj_end, &values[n])) {
            return false;
        }
        n++;
        p = virt_llm_json_skip_ws(p, obj_end);
        if (p < obj_end && *p == ',') {
            p++;
            continue;
        }
        if (p < obj_end && *p == ']') {
            p++;
            break;
        }
        return false;
    }
    *count = n;
    return n > 0;
}

static uint32_t virt_llm_safetensors_dtype(const char *dtype)
{
    if (!g_strcmp0(dtype, "F32")) {
        return VIRT_LLM_DTYPE_F32;
    }
    if (!g_strcmp0(dtype, "BF16")) {
        return VIRT_LLM_DTYPE_BF16;
    }
    return 0;
}

static bool virt_llm_parse_safetensors_tensor(VirtLLMModelTensor *tensor,
                                              const char *json,
                                              size_t json_len,
                                              const char *name,
                                              uint32_t tensor_id)
{
    const char *obj;
    const char *obj_end;
    char dtype[8];
    uint64_t shape[4] = { 0 };
    uint64_t offsets[2] = { 0 };
    uint32_t rank;
    uint32_t offset_count;
    uint32_t dtype_id;

    if (!virt_llm_json_object_for_key(json, json_len, name, &obj, &obj_end) ||
        !virt_llm_json_field_string(obj, obj_end, "dtype", dtype,
                                    sizeof(dtype)) ||
        !virt_llm_json_field_u64_array(obj, obj_end, "shape", shape,
                                       ARRAY_SIZE(shape), &rank) ||
        !virt_llm_json_field_u64_array(obj, obj_end, "data_offsets", offsets,
                                       ARRAY_SIZE(offsets), &offset_count) ||
        offset_count != 2 || offsets[1] < offsets[0]) {
        return false;
    }

    dtype_id = virt_llm_safetensors_dtype(dtype);
    if (!dtype_id) {
        return false;
    }

    memset(tensor, 0, sizeof(*tensor));
    tensor->tensor_id = tensor_id;
    tensor->dtype = dtype_id;
    tensor->rank = rank;
    for (uint32_t i = 0; i < rank; i++) {
        if (shape[i] > UINT32_MAX) {
            return false;
        }
        tensor->dims[i] = shape[i];
    }
    tensor->data_begin = offsets[0];
    tensor->data_end = offsets[1];
    g_strlcpy(tensor->name, name, sizeof(tensor->name));
    return true;
}

static uint32_t virt_llm_tensor_checksum(const VirtLLMModelTensor *tensor,
                                         uint32_t checksum)
{
    checksum = virt_llm_model_checksum_mix(checksum, tensor->tensor_id);
    checksum = virt_llm_model_checksum_mix(checksum, tensor->dtype);
    checksum = virt_llm_model_checksum_mix(checksum, tensor->rank);
    for (uint32_t i = 0; i < tensor->rank; i++) {
        checksum = virt_llm_model_checksum_mix(checksum, tensor->dims[i]);
    }
    checksum = virt_llm_model_checksum_mix(checksum, tensor->data_begin);
    checksum = virt_llm_model_checksum_mix(checksum, tensor->data_end);
    return checksum;
}

static const char *virt_llm_model_path(VirtLLMState *s)
{
    return s->model_path && s->model_path[0] ?
           s->model_path : VIRT_LLM_DEFAULT_SAFETENSORS_PATH;
}

static bool virt_llm_read_safetensors_header(VirtLLMState *s, char **json,
                                             size_t *json_len)
{
    FILE *fp;
    uint8_t len_raw[8];
    uint64_t header_len = 0;
    char *buf;

    fp = fopen(virt_llm_model_path(s), "rb");
    if (!fp) {
        return false;
    }
    if (fread(len_raw, 1, sizeof(len_raw), fp) != sizeof(len_raw)) {
        fclose(fp);
        return false;
    }
    for (uint32_t i = 0; i < sizeof(len_raw); i++) {
        header_len |= (uint64_t)len_raw[i] << (i * 8);
    }
    if (!header_len || header_len > VIRT_LLM_SAFETENSORS_HEADER_MAX) {
        fclose(fp);
        return false;
    }

    buf = g_malloc(header_len + 1);
    if (fread(buf, 1, header_len, fp) != header_len) {
        g_free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    buf[header_len] = 0;
    *json = buf;
    *json_len = header_len;
    return true;
}

static bool virt_llm_load_qwen_tensor_table(VirtLLMState *s)
{
    static const char * const layer_slots[] = {
        "input_layernorm.weight",
        "post_attention_layernorm.weight",
        "self_attn.q_proj.weight",
        "self_attn.k_proj.weight",
        "self_attn.v_proj.weight",
        "self_attn.o_proj.weight",
        "mlp.gate_proj.weight",
        "mlp.up_proj.weight",
        "mlp.down_proj.weight",
        "self_attn.q_proj.bias",
        "self_attn.k_proj.bias",
        "self_attn.v_proj.bias",
    };
    g_autofree char *json = NULL;
    size_t json_len = 0;
    uint32_t count = 0;
    uint32_t checksum = 2166136261u;

    memset(s->model_tensors, 0, sizeof(s->model_tensors));
    s->model_loaded = false;
    s->model_tensor_count = 0;
    s->model_checksum = 0;
    s->model_data_base = 0;

    if (!virt_llm_read_safetensors_header(s, &json, &json_len)) {
        return false;
    }
    s->model_data_base = 8 + json_len;

    if (!virt_llm_parse_safetensors_tensor(&s->model_tensors[count], json,
                                           json_len,
                                           "model.embed_tokens.weight",
                                           VIRT_LLM_TENSOR_ID_EMBED)) {
        return false;
    }
    checksum = virt_llm_tensor_checksum(&s->model_tensors[count], checksum);
    count++;

    if (!virt_llm_parse_safetensors_tensor(&s->model_tensors[count], json,
                                           json_len, "model.norm.weight",
                                           VIRT_LLM_TENSOR_ID_FINAL_NORM)) {
        return false;
    }
    checksum = virt_llm_tensor_checksum(&s->model_tensors[count], checksum);
    count++;

    for (uint32_t layer = 0; layer < VIRT_LLM_QWEN_LAYERS; layer++) {
        for (uint32_t slot = 0; slot < ARRAY_SIZE(layer_slots); slot++) {
            char name[96];
            uint32_t tensor_id = VIRT_LLM_TENSOR_ID_LAYER_BASE +
                                 layer * VIRT_LLM_TENSOR_ID_LAYER_STRIDE + slot;

            snprintf(name, sizeof(name), "model.layers.%u.%s", layer,
                     layer_slots[slot]);
            if (!virt_llm_parse_safetensors_tensor(&s->model_tensors[count],
                                                   json, json_len, name,
                                                   tensor_id)) {
                return false;
            }
            checksum = virt_llm_tensor_checksum(&s->model_tensors[count],
                                                checksum);
            count++;
        }
    }

    s->model_loaded = true;
    s->model_tensor_count = count;
    s->model_checksum = checksum;
    return count == VIRT_LLM_QWEN_TENSORS;
}

static const VirtLLMModelTensor *virt_llm_find_model_tensor(VirtLLMState *s,
                                                            uint32_t tensor_id)
{
    if (!s->model_loaded || !tensor_id) {
        return NULL;
    }
    for (uint32_t i = 0; i < s->model_tensor_count; i++) {
        if (s->model_tensors[i].tensor_id == tensor_id) {
            return &s->model_tensors[i];
        }
    }
    return NULL;
}

static bool virt_llm_model_tensor_elems(const VirtLLMModelTensor *tensor,
                                        uint64_t *elems)
{
    uint64_t n = 1;

    if (!tensor->rank || tensor->rank > ARRAY_SIZE(tensor->dims)) {
        return false;
    }
    for (uint32_t i = 0; i < tensor->rank; i++) {
        if (!tensor->dims[i] ||
            n > VIRT_LLM_TENSOR_MAX_XFER / tensor->dims[i]) {
            return false;
        }
        n *= tensor->dims[i];
    }
    *elems = n;
    return true;
}

static float virt_llm_bf16_to_f32(uint16_t raw)
{
    union {
        uint32_t u;
        float f;
    } v = { .u = (uint32_t)raw << 16 };

    return v.f;
}

static bool virt_llm_read_model_tensor_f32(VirtLLMState *s, uint32_t tensor_id,
                                           bool transpose_2d, float **out,
                                           uint32_t *rows, uint32_t *cols)
{
    const VirtLLMModelTensor *tensor = virt_llm_find_model_tensor(s, tensor_id);
    g_autofree uint8_t *raw = NULL;
    FILE *fp;
    uint64_t elems;
    uint64_t elem_size;
    uint64_t expected_bytes;
    uint64_t payload_bytes;
    float *data;

    if (!tensor || !virt_llm_model_tensor_elems(tensor, &elems) ||
        elems > VIRT_LLM_TENSOR_MAX_XFER / sizeof(float)) {
        return false;
    }
    elem_size = tensor->dtype == VIRT_LLM_DTYPE_BF16 ? 2 :
                tensor->dtype == VIRT_LLM_DTYPE_F32 ? 4 : 0;
    if (!elem_size) {
        return false;
    }
    expected_bytes = elems * elem_size;
    payload_bytes = tensor->data_end - tensor->data_begin;
    if (payload_bytes != expected_bytes ||
        payload_bytes > VIRT_LLM_TENSOR_MAX_XFER) {
        return false;
    }

    fp = fopen(virt_llm_model_path(s), "rb");
    if (!fp) {
        return false;
    }
    if (fseeko(fp, s->model_data_base + tensor->data_begin, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    raw = g_malloc(payload_bytes);
    if (fread(raw, 1, payload_bytes, fp) != payload_bytes) {
        fclose(fp);
        return false;
    }
    fclose(fp);

    data = g_malloc(elems * sizeof(float));
    if (tensor->dtype == VIRT_LLM_DTYPE_BF16) {
        for (uint64_t i = 0; i < elems; i++) {
            uint16_t v = raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8);

            data[i] = virt_llm_bf16_to_f32(v);
        }
    } else {
        memcpy(data, raw, elems * sizeof(float));
    }

    if (transpose_2d) {
        float *transposed;
        uint32_t r;
        uint32_t c;

        if (tensor->rank != 2) {
            g_free(data);
            return false;
        }
        r = tensor->dims[0];
        c = tensor->dims[1];
        transposed = g_malloc(elems * sizeof(float));
        for (uint32_t row = 0; row < r; row++) {
            for (uint32_t col = 0; col < c; col++) {
                transposed[(uint64_t)col * r + row] =
                    data[(uint64_t)row * c + col];
            }
        }
        g_free(data);
        data = transposed;
        if (rows) {
            *rows = c;
        }
        if (cols) {
            *cols = r;
        }
    } else {
        if (rows) {
            *rows = tensor->rank > 0 ? tensor->dims[0] : 0;
        }
        if (cols) {
            *cols = tensor->rank > 1 ? tensor->dims[1] : 1;
        }
    }

    *out = data;
    return true;
}

static uint32_t virt_llm_process_model_load(VirtLLMState *s,
                                            VirtLLMDesc *desc)
{
    if (!virt_llm_load_qwen_tensor_table(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "virt-llm: failed to parse safetensors metadata from %s\n",
                      virt_llm_model_path(s));
        return VIRT_LLM_DESC_BAD_TENSOR;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "virt-llm: loaded safetensors metadata tensors=%u checksum=0x%08x\n",
                  s->model_tensor_count, s->model_checksum);
    desc->result = cpu_to_le32(s->model_tensor_count);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_model_query(VirtLLMState *s,
                                             VirtLLMDesc *desc)
{
    VirtLLMModelQuery query = { 0 };
    uint64_t output_addr = le64_to_cpu(desc->output_addr);

    if (!output_addr) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    query.abi = cpu_to_le32(VIRT_LLM_TENSOR_ABI_VERSION);
    query.model_loaded = cpu_to_le32(s->model_loaded ? 1 : 0);
    query.layers = cpu_to_le32(VIRT_LLM_QWEN_LAYERS);
    query.hidden_size = cpu_to_le32(VIRT_LLM_QWEN_HIDDEN);
    query.attention_heads = cpu_to_le32(VIRT_LLM_QWEN_HEADS);
    query.kv_heads = cpu_to_le32(VIRT_LLM_QWEN_KV_HEADS);
    query.head_dim = cpu_to_le32(VIRT_LLM_QWEN_HEAD_DIM);
    query.intermediate_size = cpu_to_le32(VIRT_LLM_QWEN_INTERMEDIATE);
    query.vocab_size = cpu_to_le32(VIRT_LLM_QWEN_VOCAB);
    query.dtype = cpu_to_le32(VIRT_LLM_DTYPE_F32);
    query.rope_theta_bits = cpu_to_le64(virt_llm_double_to_bits(1000000.0));
    query.rms_eps_bits = cpu_to_le64(virt_llm_double_to_bits(1.0e-6));
    pci_dma_write(PCI_DEVICE(s), output_addr, &query, sizeof(query));
    desc->result = cpu_to_le32(VIRT_LLM_QWEN_HIDDEN);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_embed_lookup_f32(VirtLLMState *s,
                                                  VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree uint32_t *tokens = NULL;
    g_autofree float *embedding = NULL;
    g_autofree float *out = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t weight_addr = le64_to_cpu(desc->rsvd1);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint32_t seq, hidden, vocab;
    uint64_t token_bytes, embedding_bytes, out_bytes;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    seq = le32_to_cpu(req.dims[0]);
    hidden = le32_to_cpu(req.dims[1]);
    vocab = le32_to_cpu(req.dims[2]);
    if (!input_addr || !output_addr || !seq || !hidden ||
        !vocab ||
        !virt_llm_tensor_bytes(seq, sizeof(uint32_t), &token_bytes) ||
        !virt_llm_tensor_bytes((uint64_t)vocab * hidden, sizeof(float),
                               &embedding_bytes) ||
        !virt_llm_tensor_bytes((uint64_t)seq * hidden, sizeof(float),
                               &out_bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    tokens = g_malloc(token_bytes);
    out = g_malloc(out_bytes);
    pci_dma_read(PCI_DEVICE(s), input_addr + le32_to_cpu(req.input_offset),
                 tokens, token_bytes);
    if (le32_to_cpu(req.tensor_id)) {
        uint32_t rows;
        uint32_t cols;

        if (!virt_llm_read_model_tensor_f32(s, le32_to_cpu(req.tensor_id),
                                            false, &embedding, &rows, &cols) ||
            rows != vocab || cols != hidden) {
            return VIRT_LLM_DESC_BAD_TENSOR;
        }
    } else {
        if (!weight_addr) {
            return VIRT_LLM_DESC_BAD_LEN;
        }
        embedding = g_malloc(embedding_bytes);
        pci_dma_read(PCI_DEVICE(s), weight_addr + le32_to_cpu(req.weight_offset),
                     embedding, embedding_bytes);
    }
    for (uint32_t t = 0; t < seq; t++) {
        uint32_t token = le32_to_cpu(tokens[t]);

        if (token >= vocab) {
            return VIRT_LLM_DESC_BAD_TENSOR;
        }
        memcpy(&out[(uint64_t)t * hidden], &embedding[(uint64_t)token * hidden],
               hidden * sizeof(float));
    }
    pci_dma_write(PCI_DEVICE(s), output_addr + le32_to_cpu(req.output_offset),
                  out, out_bytes);
    desc->result = cpu_to_le32(virt_llm_f32_checksum(out, (uint64_t)seq * hidden));
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_rmsnorm_f32(VirtLLMState *s, VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree float *input = NULL;
    g_autofree float *weight = NULL;
    g_autofree float *out = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t weight_addr = le64_to_cpu(desc->rsvd1);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint32_t rows, cols;
    uint64_t input_bytes, weight_bytes;
    float eps;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    rows = le32_to_cpu(req.dims[0]);
    cols = le32_to_cpu(req.dims[1]);
    if (!input_addr || !output_addr || !rows || !cols ||
        !virt_llm_tensor_bytes((uint64_t)rows * cols, sizeof(float),
                               &input_bytes) ||
        !virt_llm_tensor_bytes(cols, sizeof(float), &weight_bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }

    eps = (float)virt_llm_double_from_bits(le64_to_cpu(req.scalar0_bits), 1.0e-6);
    input = g_malloc(input_bytes);
    out = g_malloc(input_bytes);
    pci_dma_read(PCI_DEVICE(s), input_addr + le32_to_cpu(req.input_offset),
                 input, input_bytes);
    if (le32_to_cpu(req.tensor_id)) {
        uint32_t w_rows;
        uint32_t w_cols;

        if (!virt_llm_read_model_tensor_f32(s, le32_to_cpu(req.tensor_id),
                                            false, &weight, &w_rows, &w_cols) ||
            w_rows * w_cols != cols) {
            return VIRT_LLM_DESC_BAD_TENSOR;
        }
    } else {
        if (!weight_addr) {
            return VIRT_LLM_DESC_BAD_LEN;
        }
        weight = g_malloc(weight_bytes);
        pci_dma_read(PCI_DEVICE(s), weight_addr + le32_to_cpu(req.weight_offset),
                     weight, weight_bytes);
    }
    for (uint32_t r = 0; r < rows; r++) {
        double ss = 0.0;

        for (uint32_t c = 0; c < cols; c++) {
            float v = input[(uint64_t)r * cols + c];

            ss += (double)v * v;
        }
        float scale = 1.0f / sqrtf((float)(ss / cols) + eps);
        for (uint32_t c = 0; c < cols; c++) {
            uint64_t idx = (uint64_t)r * cols + c;

            out[idx] = input[idx] * scale * weight[c];
        }
    }
    pci_dma_write(PCI_DEVICE(s), output_addr + le32_to_cpu(req.output_offset),
                  out, input_bytes);
    desc->result = cpu_to_le32(virt_llm_f32_checksum(out, (uint64_t)rows * cols));
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_add_f32(VirtLLMState *s, VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree float *a = NULL;
    g_autofree float *b = NULL;
    g_autofree float *out = NULL;
    uint64_t a_addr = le64_to_cpu(desc->input_addr);
    uint64_t b_addr = le64_to_cpu(desc->rsvd1);
    uint64_t out_addr = le64_to_cpu(desc->output_addr);
    uint64_t elems, bytes;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    elems = (uint64_t)le32_to_cpu(req.dims[0]) * le32_to_cpu(req.dims[1]);
    if (!a_addr || !b_addr || !out_addr ||
        !virt_llm_tensor_bytes(elems, sizeof(float), &bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }
    a = g_malloc(bytes);
    b = g_malloc(bytes);
    out = g_malloc(bytes);
    pci_dma_read(PCI_DEVICE(s), a_addr + le32_to_cpu(req.input_offset), a, bytes);
    pci_dma_read(PCI_DEVICE(s), b_addr + le32_to_cpu(req.input2_offset), b, bytes);
    for (uint64_t i = 0; i < elems; i++) {
        out[i] = a[i] + b[i];
    }
    pci_dma_write(PCI_DEVICE(s), out_addr + le32_to_cpu(req.output_offset),
                  out, bytes);
    desc->result = cpu_to_le32(virt_llm_f32_checksum(out, elems));
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_swiglu_f32(VirtLLMState *s, VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree float *gate = NULL;
    g_autofree float *up = NULL;
    g_autofree float *out = NULL;
    uint64_t gate_addr = le64_to_cpu(desc->input_addr);
    uint64_t up_addr = le64_to_cpu(desc->rsvd1);
    uint64_t out_addr = le64_to_cpu(desc->output_addr);
    uint64_t elems, bytes;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    elems = (uint64_t)le32_to_cpu(req.dims[0]) * le32_to_cpu(req.dims[1]);
    if (!gate_addr || !up_addr || !out_addr ||
        !virt_llm_tensor_bytes(elems, sizeof(float), &bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }
    gate = g_malloc(bytes);
    up = g_malloc(bytes);
    out = g_malloc(bytes);
    pci_dma_read(PCI_DEVICE(s), gate_addr + le32_to_cpu(req.input_offset),
                 gate, bytes);
    pci_dma_read(PCI_DEVICE(s), up_addr + le32_to_cpu(req.input2_offset),
                 up, bytes);
    for (uint64_t i = 0; i < elems; i++) {
        out[i] = virt_llm_silu(gate[i]) * up[i];
    }
    pci_dma_write(PCI_DEVICE(s), out_addr + le32_to_cpu(req.output_offset),
                  out, bytes);
    desc->result = cpu_to_le32(virt_llm_f32_checksum(out, elems));
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_argmax_f32(VirtLLMState *s, VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree float *input = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint32_t count, best = 0;
    uint64_t bytes;
    uint32_t out_token;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    count = le32_to_cpu(req.dims[0]);
    if (!input_addr || !output_addr ||
        !virt_llm_tensor_bytes(count, sizeof(float), &bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }
    input = g_malloc(bytes);
    pci_dma_read(PCI_DEVICE(s), input_addr + le32_to_cpu(req.input_offset),
                 input, bytes);
    for (uint32_t i = 1; i < count; i++) {
        if (input[i] > input[best]) {
            best = i;
        }
    }
    out_token = cpu_to_le32(best);
    pci_dma_write(PCI_DEVICE(s), output_addr + le32_to_cpu(req.output_offset),
                  &out_token, sizeof(out_token));
    desc->result = cpu_to_le32(best);
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_gemm_f32(VirtLLMState *s, VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree float *a = NULL;
    g_autofree float *b = NULL;
    g_autofree float *bias = NULL;
    g_autofree float *c = NULL;
    uint64_t a_addr = le64_to_cpu(desc->input_addr);
    uint64_t b_addr = le64_to_cpu(desc->rsvd1);
    uint64_t c_addr = le64_to_cpu(desc->output_addr);
    uint32_t m, n, k;
    uint64_t a_bytes, b_bytes, c_bytes;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    m = le32_to_cpu(req.dims[0]);
    n = le32_to_cpu(req.dims[1]);
    k = le32_to_cpu(req.dims[2]);
    if (!a_addr || !c_addr || !m || !n || !k ||
        !virt_llm_tensor_bytes((uint64_t)m * k, sizeof(float), &a_bytes) ||
        !virt_llm_tensor_bytes((uint64_t)k * n, sizeof(float), &b_bytes) ||
        !virt_llm_tensor_bytes((uint64_t)m * n, sizeof(float), &c_bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }
    a = g_malloc(a_bytes);
    c = g_new0(float, (uint64_t)m * n);
    pci_dma_read(PCI_DEVICE(s), a_addr + le32_to_cpu(req.input_offset), a,
                 a_bytes);
    if (le32_to_cpu(req.tensor_id)) {
        uint32_t w_rows;
        uint32_t w_cols;

        if (!virt_llm_read_model_tensor_f32(s, le32_to_cpu(req.tensor_id),
                                            true, &b, &w_rows, &w_cols) ||
            w_rows != k || w_cols != n) {
            return VIRT_LLM_DESC_BAD_TENSOR;
        }
    } else {
        if (!b_addr) {
            return VIRT_LLM_DESC_BAD_LEN;
        }
        b = g_malloc(b_bytes);
        pci_dma_read(PCI_DEVICE(s), b_addr + le32_to_cpu(req.weight_offset), b,
                     b_bytes);
    }
    if (le32_to_cpu(req.aux_tensor_id)) {
        uint32_t bias_rows;
        uint32_t bias_cols;

        if (!virt_llm_read_model_tensor_f32(s, le32_to_cpu(req.aux_tensor_id),
                                            false, &bias, &bias_rows,
                                            &bias_cols) ||
            (uint64_t)bias_rows * bias_cols != n) {
            return VIRT_LLM_DESC_BAD_TENSOR;
        }
    }
    for (uint32_t row = 0; row < m; row++) {
        for (uint32_t col = 0; col < n; col++) {
            double sum = 0.0;

            for (uint32_t inner = 0; inner < k; inner++) {
                sum += (double)a[(uint64_t)row * k + inner] *
                       b[(uint64_t)inner * n + col];
            }
            if (bias) {
                sum += bias[col];
            }
            c[(uint64_t)row * n + col] = (float)sum;
        }
    }
    pci_dma_write(PCI_DEVICE(s), c_addr + le32_to_cpu(req.output_offset), c,
                  c_bytes);
    desc->result = cpu_to_le32(virt_llm_f32_checksum(c, (uint64_t)m * n));
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_rope_f32(VirtLLMState *s, VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree float *input = NULL;
    g_autofree float *out = NULL;
    uint64_t input_addr = le64_to_cpu(desc->input_addr);
    uint64_t output_addr = le64_to_cpu(desc->output_addr);
    uint32_t seq, heads, head_dim;
    uint64_t elems, bytes;
    double theta;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    seq = le32_to_cpu(req.dims[0]);
    heads = le32_to_cpu(req.dims[1]);
    head_dim = le32_to_cpu(req.dims[2]);
    elems = (uint64_t)seq * heads * head_dim;
    if (!input_addr || !output_addr || !seq || !heads || !head_dim ||
        head_dim % 2 ||
        !virt_llm_tensor_bytes(elems, sizeof(float), &bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }
    theta = virt_llm_double_from_bits(le64_to_cpu(req.scalar0_bits), 1000000.0);
    input = g_malloc(bytes);
    out = g_malloc(bytes);
    pci_dma_read(PCI_DEVICE(s), input_addr + le32_to_cpu(req.input_offset),
                 input, bytes);
    memcpy(out, input, bytes);
    for (uint32_t pos = 0; pos < seq; pos++) {
        for (uint32_t h = 0; h < heads; h++) {
            float *base = &out[((uint64_t)pos * heads + h) * head_dim];
            uint32_t half = head_dim / 2;

            for (uint32_t d = 0; d < half; d++) {
                double inv = pow(theta, -(double)(2 * d) / head_dim);
                double angle = pos * inv;
                float x0 = base[d];
                float x1 = base[d + half];
                float c = cos(angle);
                float si = sin(angle);

                base[d] = x0 * c - x1 * si;
                base[d + half] = x0 * si + x1 * c;
            }
        }
    }
    pci_dma_write(PCI_DEVICE(s), output_addr + le32_to_cpu(req.output_offset),
                  out, bytes);
    desc->result = cpu_to_le32(virt_llm_f32_checksum(out, elems));
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_process_qwen_gqa_attention_f32(VirtLLMState *s,
                                                        VirtLLMDesc *desc)
{
    VirtLLMTensorReq req;
    g_autofree float *q = NULL;
    g_autofree float *k = NULL;
    g_autofree float *v = NULL;
    g_autofree float *out = NULL;
    g_autofree float *scores = NULL;
    uint64_t q_addr = le64_to_cpu(desc->input_addr);
    uint64_t k_addr = le64_to_cpu(desc->rsvd1);
    uint64_t v_addr = le64_to_cpu(desc->rsvd2);
    uint64_t out_addr = le64_to_cpu(desc->output_addr);
    uint32_t seq, heads, kv_heads, head_dim;
    uint64_t q_bytes, kv_bytes, out_bytes;
    float scale;

    if (!virt_llm_tensor_req_read(s, desc, &req) ||
        le32_to_cpu(req.dtype) != VIRT_LLM_DTYPE_F32) {
        return VIRT_LLM_DESC_BAD_TENSOR;
    }
    seq = le32_to_cpu(req.dims[0]);
    heads = le32_to_cpu(req.dims[1]);
    kv_heads = le32_to_cpu(req.dims[2]);
    head_dim = le32_to_cpu(req.dims[3]);
    if (!q_addr || !k_addr || !v_addr || !out_addr || !seq || !heads ||
        !kv_heads || !head_dim || heads % kv_heads ||
        !virt_llm_tensor_bytes((uint64_t)seq * heads * head_dim, sizeof(float),
                               &q_bytes) ||
        !virt_llm_tensor_bytes((uint64_t)seq * kv_heads * head_dim,
                               sizeof(float), &kv_bytes) ||
        !virt_llm_tensor_bytes((uint64_t)seq * heads * head_dim, sizeof(float),
                               &out_bytes)) {
        return VIRT_LLM_DESC_BAD_LEN;
    }
    q = g_malloc(q_bytes);
    k = g_malloc(kv_bytes);
    v = g_malloc(kv_bytes);
    out = g_new0(float, (uint64_t)seq * heads * head_dim);
    scores = g_malloc(seq * sizeof(float));
    pci_dma_read(PCI_DEVICE(s), q_addr + le32_to_cpu(req.input_offset), q,
                 q_bytes);
    pci_dma_read(PCI_DEVICE(s), k_addr + le32_to_cpu(req.weight_offset), k,
                 kv_bytes);
    pci_dma_read(PCI_DEVICE(s), v_addr + le32_to_cpu(req.aux_offset), v,
                 kv_bytes);
    scale = 1.0f / sqrtf((float)head_dim);

    for (uint32_t t = 0; t < seq; t++) {
        for (uint32_t h = 0; h < heads; h++) {
            uint32_t kvh = h / (heads / kv_heads);
            uint32_t cols = (le32_to_cpu(req.flags) & VIRT_LLM_TENSOR_F_CAUSAL) ?
                            t + 1 : seq;
            float max_score = -INFINITY;
            float sum_exp = 0.0f;

            for (uint32_t j = 0; j < cols; j++) {
                float score = 0.0f;
                float *qv = &q[((uint64_t)t * heads + h) * head_dim];
                float *kv = &k[((uint64_t)j * kv_heads + kvh) * head_dim];

                for (uint32_t d = 0; d < head_dim; d++) {
                    score += qv[d] * kv[d];
                }
                score *= scale;
                scores[j] = score;
                if (score > max_score) {
                    max_score = score;
                }
            }
            for (uint32_t j = 0; j < cols; j++) {
                scores[j] = expf(scores[j] - max_score);
                sum_exp += scores[j];
            }
            if (sum_exp == 0.0f) {
                return VIRT_LLM_DESC_BAD_TENSOR;
            }
            for (uint32_t d = 0; d < head_dim; d++) {
                float acc = 0.0f;

                for (uint32_t j = 0; j < cols; j++) {
                    float prob = scores[j] / sum_exp;
                    float vv = v[((uint64_t)j * kv_heads + kvh) * head_dim + d];

                    acc += prob * vv;
                }
                out[((uint64_t)t * heads + h) * head_dim + d] = acc;
            }
        }
    }
    pci_dma_write(PCI_DEVICE(s), out_addr + le32_to_cpu(req.output_offset), out,
                  out_bytes);
    desc->result = cpu_to_le32(virt_llm_f32_checksum(out,
                                                     (uint64_t)seq * heads * head_dim));
    return VIRT_LLM_DESC_COMPLETE;
}

static uint32_t virt_llm_dispatch_desc(VirtLLMState *s, VirtLLMDesc *desc,
                                       uint32_t opcode, uint32_t *backend)
{
    switch (opcode) {
    case VIRT_LLM_OP_INFER_XOR:
        *backend = VIRT_LLM_BACKEND_COMPAT;
        return virt_llm_process_infer_xor(s, desc);
    case VIRT_LLM_OP_DMA_COPY:
        *backend = VIRT_LLM_BACKEND_DMA;
        return virt_llm_process_dma_copy(s, desc);
    case VIRT_LLM_OP_VEC_ADD_U32:
    case VIRT_LLM_OP_DOT_U32:
    case VIRT_LLM_OP_SOFTMAX_Q16:
    case VIRT_LLM_OP_POOL_MAX_U32:
        *backend = VIRT_LLM_BACKEND_SCALAR;
        return virt_llm_scalar_dispatch(s, desc, opcode);
    case VIRT_LLM_OP_GEMM_U32:
        *backend = VIRT_LLM_BACKEND_TENSOR;
        return virt_llm_process_gemm_u32(s, desc);
    case VIRT_LLM_OP_CONV2D_U32:
        *backend = VIRT_LLM_BACKEND_TENSOR;
        return virt_llm_process_conv2d_u32(s, desc);
    case VIRT_LLM_OP_ATTENTION_Q16:
        *backend = VIRT_LLM_BACKEND_TENSOR;
        return virt_llm_process_attention_q16(s, desc);
    case VIRT_LLM_OP_MODEL_LOAD:
        *backend = VIRT_LLM_BACKEND_DMA;
        return virt_llm_process_model_load(s, desc);
    case VIRT_LLM_OP_MODEL_QUERY:
        *backend = VIRT_LLM_BACKEND_DMA;
        return virt_llm_process_model_query(s, desc);
    case VIRT_LLM_OP_EMBED_LOOKUP_F32:
        *backend = VIRT_LLM_BACKEND_VECTOR;
        return virt_llm_process_embed_lookup_f32(s, desc);
    case VIRT_LLM_OP_RMSNORM_F32:
        *backend = VIRT_LLM_BACKEND_VECTOR;
        return virt_llm_process_rmsnorm_f32(s, desc);
    case VIRT_LLM_OP_ROPE_F32:
        *backend = VIRT_LLM_BACKEND_VECTOR;
        return virt_llm_process_rope_f32(s, desc);
    case VIRT_LLM_OP_GEMM_F32:
    case VIRT_LLM_OP_LM_HEAD_F32:
        *backend = VIRT_LLM_BACKEND_TENSOR;
        return virt_llm_process_gemm_f32(s, desc);
    case VIRT_LLM_OP_ADD_F32:
        *backend = VIRT_LLM_BACKEND_VECTOR;
        return virt_llm_process_add_f32(s, desc);
    case VIRT_LLM_OP_SWIGLU_F32:
        *backend = VIRT_LLM_BACKEND_VECTOR;
        return virt_llm_process_swiglu_f32(s, desc);
    case VIRT_LLM_OP_QWEN_GQA_ATTENTION_F32:
        *backend = VIRT_LLM_BACKEND_TENSOR;
        return virt_llm_process_qwen_gqa_attention_f32(s, desc);
    case VIRT_LLM_OP_ARGMAX_F32:
        *backend = VIRT_LLM_BACKEND_VECTOR;
        return virt_llm_process_argmax_f32(s, desc);
    default:
        *backend = UINT32_MAX;
        return VIRT_LLM_DESC_UNSUPP;
    }
}

static void virt_llm_process_queue(VirtLLMState *s)
{
    if (!(s->queue_ctrl & VIRT_LLM_Q_CTRL_ENABLE)) {
        return;
    }

    if (!virt_llm_queue_config_valid(s)) {
        virt_llm_queue_error(s, VIRT_LLM_Q_ERR_CONFIG);
        return;
    }

    while (s->queue_head != s->queue_tail) {
        uint32_t idx = s->queue_head % s->queue_size;
        uint64_t addr = virt_llm_desc_addr(s, idx);
        VirtLLMDesc desc;
        uint32_t opcode;
        uint32_t status;
        uint32_t backend;

        pci_dma_read(PCI_DEVICE(s), addr, &desc, sizeof(desc));
        if (!(le32_to_cpu(desc.flags) & VIRT_LLM_DESC_F_READY)) {
            return;
        }

        opcode = le32_to_cpu(desc.opcode);
        status = virt_llm_dispatch_desc(s, &desc, opcode, &backend);

        desc.status = cpu_to_le32(status);
        pci_dma_write(PCI_DEVICE(s), addr, &desc, sizeof(desc));
        s->queue_head++;
        virt_llm_write_cq(s, &desc, opcode, backend, status);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "virt-llm: opcode=0x%04x backend=%u status=0x%08x result=0x%08x\n",
                      opcode, backend, status, le32_to_cpu(desc.result));
        if (status == VIRT_LLM_DESC_COMPLETE) {
            virt_llm_raise_irq(s, VIRT_LLM_IRQ_COMPLETE);
        } else {
            virt_llm_queue_error(s,
                                 backend == UINT32_MAX ? VIRT_LLM_Q_ERR_OPCODE :
                                 status == VIRT_LLM_DESC_BAD_KERNEL ?
                                 VIRT_LLM_Q_ERR_KERNEL : VIRT_LLM_Q_ERR_DESC);
        }
    }
}

static uint64_t virt_llm_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    VirtLLMState *s = opaque;

    if (size != 4) {
        return UINT32_MAX;
    }

    switch (addr) {
    case VIRT_LLM_REG_MAGIC:
        return VIRT_LLM_MAGIC;
    case VIRT_LLM_REG_VERSION:
        return VIRT_LLM_VERSION;
    case VIRT_LLM_REG_DOORBELL:
        return s->doorbell;
    case VIRT_LLM_REG_STATUS:
        return s->doorbell ^ VIRT_LLM_STATUS_XOR;
    case VIRT_LLM_REG_FEATURES:
        return VIRT_LLM_FEATURE_QUEUE | VIRT_LLM_FEATURE_MSI |
               VIRT_LLM_FEATURE_MSIX | VIRT_LLM_FEATURE_QCTRL |
               VIRT_LLM_FEATURE_CQ | VIRT_LLM_FEATURE_SCALAR;
    case VIRT_LLM_REG_Q_SIZE:
        return s->queue_size;
    case VIRT_LLM_REG_Q_LO:
        return s->queue_addr;
    case VIRT_LLM_REG_Q_HI:
        return s->queue_addr >> 32;
    case VIRT_LLM_REG_Q_HEAD:
        return s->queue_head;
    case VIRT_LLM_REG_Q_TAIL:
        return s->queue_tail;
    case VIRT_LLM_REG_IRQ_STS:
        return s->irq_status;
    case VIRT_LLM_REG_IRQ_MASK:
        return s->irq_mask;
    case VIRT_LLM_REG_ABI:
        return VIRT_LLM_ABI_VERSION;
    case VIRT_LLM_REG_Q_MAX:
        return VIRT_LLM_MAX_QUEUE;
    case VIRT_LLM_REG_XFER_MAX:
        return VIRT_LLM_MAX_XFER;
    case VIRT_LLM_REG_IRQ_VEC:
        return VIRT_LLM_MSIX_VECTORS;
    case VIRT_LLM_REG_Q_CTRL:
        return s->queue_ctrl;
    case VIRT_LLM_REG_Q_STATUS:
        return s->queue_status;
    case VIRT_LLM_REG_Q_ERROR:
        return s->queue_error;
    case VIRT_LLM_REG_CQ_SIZE:
        return s->cq_size;
    case VIRT_LLM_REG_CQ_LO:
        return s->cq_addr;
    case VIRT_LLM_REG_CQ_HI:
        return s->cq_addr >> 32;
    case VIRT_LLM_REG_CQ_HEAD:
        return s->cq_head;
    case VIRT_LLM_REG_CQ_TAIL:
        return s->cq_tail;
    case VIRT_LLM_REG_SCALAR_STATUS:
        return s->scalar_status;
    case VIRT_LLM_REG_SCALAR_KERNELS:
        return ARRAY_SIZE(virt_llm_kernels);
    case VIRT_LLM_REG_SCALAR_LAST_KERNEL:
        return s->scalar_last_kernel;
    case VIRT_LLM_REG_SCALAR_LAST_OPCODE:
        return s->scalar_last_opcode;
    case VIRT_LLM_REG_KERNEL_INDEX:
        return s->kernel_index;
    case VIRT_LLM_REG_KERNEL_ID:
        return virt_llm_selected_kernel(s) ?
               virt_llm_selected_kernel(s)->kernel_id : 0;
    case VIRT_LLM_REG_KERNEL_OPCODE:
        return virt_llm_selected_kernel(s) ?
               virt_llm_selected_kernel(s)->opcode : 0;
    case VIRT_LLM_REG_KERNEL_ABI:
        return virt_llm_selected_kernel(s) ?
               virt_llm_selected_kernel(s)->abi_version : 0;
    case VIRT_LLM_REG_KERNEL_ENTRY:
        return virt_llm_selected_kernel(s) ?
               virt_llm_selected_kernel(s)->entry_point : 0;
    case VIRT_LLM_REG_KERNEL_SIZE:
        return virt_llm_selected_kernel(s) ?
               virt_llm_selected_kernel(s)->binary_size : 0;
    case VIRT_LLM_REG_KERNEL_CHECKSUM:
        return virt_llm_selected_kernel(s) ?
               virt_llm_selected_kernel(s)->binary_checksum : 0;
    default:
        return 0;
    }
}

static void virt_llm_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    VirtLLMState *s = opaque;

    if (size != 4) {
        return;
    }

    switch (addr) {
    case VIRT_LLM_REG_DOORBELL:
        s->doorbell = val;
        break;
    case VIRT_LLM_REG_Q_SIZE:
        s->queue_size = val;
        s->queue_head = 0;
        s->queue_tail = 0;
        s->queue_status &= ~VIRT_LLM_Q_STATUS_ERR;
        s->queue_error = VIRT_LLM_Q_ERR_NONE;
        break;
    case VIRT_LLM_REG_Q_LO:
        s->queue_addr = (s->queue_addr & 0xffffffff00000000ULL) |
                        (val & 0xffffffffULL);
        break;
    case VIRT_LLM_REG_Q_HI:
        s->queue_addr = (s->queue_addr & 0xffffffffULL) |
                        ((val & 0xffffffffULL) << 32);
        break;
    case VIRT_LLM_REG_Q_TAIL:
        s->queue_tail = val;
        virt_llm_process_queue(s);
        break;
    case VIRT_LLM_REG_IRQ_STS:
        s->irq_status &= ~(val & VIRT_LLM_IRQ_ALL);
        virt_llm_lower_intx_if_idle(s);
        break;
    case VIRT_LLM_REG_IRQ_MASK:
        s->irq_mask = val & VIRT_LLM_IRQ_ALL;
        break;
    case VIRT_LLM_REG_COMMAND:
        if (val == VIRT_LLM_CMD_KICK) {
            virt_llm_process_queue(s);
        }
        break;
    case VIRT_LLM_REG_Q_CTRL:
        if (val & VIRT_LLM_Q_CTRL_RESET) {
            virt_llm_queue_reset(s);
            break;
        }
        s->queue_ctrl = val & VIRT_LLM_Q_CTRL_ENABLE;
        if (s->queue_ctrl & VIRT_LLM_Q_CTRL_ENABLE) {
            s->queue_status |= VIRT_LLM_Q_STATUS_EN;
            s->queue_status &= ~VIRT_LLM_Q_STATUS_ERR;
            s->queue_error = VIRT_LLM_Q_ERR_NONE;
        } else {
            s->queue_status &= ~VIRT_LLM_Q_STATUS_EN;
        }
        break;
    case VIRT_LLM_REG_CQ_SIZE:
        s->cq_size = val;
        s->cq_head = 0;
        s->cq_tail = 0;
        break;
    case VIRT_LLM_REG_CQ_LO:
        s->cq_addr = (s->cq_addr & 0xffffffff00000000ULL) |
                     (val & 0xffffffffULL);
        break;
    case VIRT_LLM_REG_CQ_HI:
        s->cq_addr = (s->cq_addr & 0xffffffffULL) |
                     ((val & 0xffffffffULL) << 32);
        break;
    case VIRT_LLM_REG_CQ_HEAD:
        s->cq_head = val;
        break;
    case VIRT_LLM_REG_KERNEL_INDEX:
        s->kernel_index = val;
        if (virt_llm_selected_kernel(s)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "virt-llm: selected kernel index=%u id=%u opcode=0x%04x abi=%u\n",
                          s->kernel_index,
                          virt_llm_selected_kernel(s)->kernel_id,
                          virt_llm_selected_kernel(s)->opcode,
                          virt_llm_selected_kernel(s)->abi_version);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps virt_llm_mmio_ops = {
    .read = virt_llm_mmio_read,
    .write = virt_llm_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void virt_llm_realize(PCIDevice *pdev, Error **errp)
{
    VirtLLMState *s = VIRT_LLM(pdev);
    uint8_t *pci_conf = pdev->config;

    pci_config_set_interrupt_pin(pci_conf, 1);

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    if (msix_init_exclusive_bar(pdev, VIRT_LLM_MSIX_VECTORS, 1, errp)) {
        msi_uninit(pdev);
        return;
    }
    msix_vector_use(pdev, 0);

    memory_region_init_io(&s->mmio, OBJECT(s), &virt_llm_mmio_ops, s,
                          "virt-llm-mmio", VIRT_LLM_MMIO_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void virt_llm_exit(PCIDevice *pdev)
{
    msix_vector_unuse(pdev, 0);
    msix_uninit_exclusive_bar(pdev);
    msi_uninit(pdev);
}

static void virt_llm_reset(DeviceState *dev)
{
    VirtLLMState *s = VIRT_LLM(dev);

    s->doorbell = 0;
    virt_llm_queue_reset(s);
    s->irq_status = 0;
    s->irq_mask = 0;
    s->scalar_status = VIRT_LLM_SCALAR_IDLE;
    s->scalar_last_kernel = 0;
    s->scalar_last_opcode = 0;
    s->model_loaded = false;
    s->model_tensor_count = 0;
    s->model_checksum = 0;
    s->model_data_base = 0;
    memset(s->model_tensors, 0, sizeof(s->model_tensors));
    pci_set_irq(PCI_DEVICE(s), 0);
    msi_reset(PCI_DEVICE(s));
    msix_reset(PCI_DEVICE(s));
}

static const Property virt_llm_properties[] = {
    DEFINE_PROP_STRING("model-path", VirtLLMState, model_path),
};

static void virt_llm_instance_init(Object *obj)
{
    VirtLLMState *s = VIRT_LLM(obj);

    s->model_path = g_strdup(VIRT_LLM_DEFAULT_SAFETENSORS_PATH);
}

static void virt_llm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = virt_llm_realize;
    pc->exit = virt_llm_exit;
    pc->vendor_id = PCI_VENDOR_ID_REDHAT;
    pc->device_id = VIRT_LLM_DEVICE_ID;
    pc->revision = 0x01;
    pc->class_id = PCI_CLASS_OTHERS;

    device_class_set_legacy_reset(dc, virt_llm_reset);
    device_class_set_props(dc, virt_llm_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo virt_llm_info = {
    .name = TYPE_VIRT_LLM,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VirtLLMState),
    .instance_init = virt_llm_instance_init,
    .class_init = virt_llm_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void virt_llm_register_types(void)
{
    type_register_static(&virt_llm_info);
}

type_init(virt_llm_register_types)
