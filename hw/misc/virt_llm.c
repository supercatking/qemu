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
#include "qemu/units.h"
#include "qom/object.h"

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

#define VIRT_LLM_IRQ_COMPLETE  BIT(0)
#define VIRT_LLM_IRQ_ERROR     BIT(1)
#define VIRT_LLM_IRQ_ALL       (VIRT_LLM_IRQ_COMPLETE | VIRT_LLM_IRQ_ERROR)
#define VIRT_LLM_CMD_KICK      1
#define VIRT_LLM_OP_INFER      1
#define VIRT_LLM_DESC_F_READY  BIT(0)
#define VIRT_LLM_DESC_COMPLETE 1
#define VIRT_LLM_DESC_INVALID  0x80000001u
#define VIRT_LLM_DESC_UNSUPP   0x80000002u
#define VIRT_LLM_DESC_BAD_LEN  0x80000003u
#define VIRT_LLM_MAX_XFER      4096
#define VIRT_LLM_Q_CTRL_ENABLE BIT(0)
#define VIRT_LLM_Q_CTRL_RESET  BIT(1)
#define VIRT_LLM_Q_STATUS_EN   BIT(0)
#define VIRT_LLM_Q_STATUS_ERR  BIT(1)
#define VIRT_LLM_Q_ERR_NONE    0u
#define VIRT_LLM_Q_ERR_CONFIG  1u
#define VIRT_LLM_Q_ERR_DESC    2u
#define VIRT_LLM_Q_ERR_OPCODE  3u

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
};

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

static void virt_llm_queue_reset(VirtLLMState *s)
{
    s->queue_addr = 0;
    s->queue_size = 0;
    s->queue_head = 0;
    s->queue_tail = 0;
    s->queue_ctrl = 0;
    s->queue_status = 0;
    s->queue_error = VIRT_LLM_Q_ERR_NONE;
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

static uint32_t virt_llm_process_infer(VirtLLMState *s, VirtLLMDesc *desc)
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

        pci_dma_read(PCI_DEVICE(s), addr, &desc, sizeof(desc));
        if (!(le32_to_cpu(desc.flags) & VIRT_LLM_DESC_F_READY)) {
            return;
        }

        opcode = le32_to_cpu(desc.opcode);
        switch (opcode) {
        case VIRT_LLM_OP_INFER:
            status = virt_llm_process_infer(s, &desc);
            break;
        default:
            status = VIRT_LLM_DESC_UNSUPP;
            break;
        }

        desc.status = cpu_to_le32(status);
        pci_dma_write(PCI_DEVICE(s), addr, &desc, sizeof(desc));
        s->queue_head++;
        if (status == VIRT_LLM_DESC_COMPLETE) {
            virt_llm_raise_irq(s, VIRT_LLM_IRQ_COMPLETE);
        } else {
            virt_llm_queue_error(s, opcode == VIRT_LLM_OP_INFER ?
                                 VIRT_LLM_Q_ERR_DESC : VIRT_LLM_Q_ERR_OPCODE);
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
               VIRT_LLM_FEATURE_MSIX | VIRT_LLM_FEATURE_QCTRL;
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
    pci_set_irq(PCI_DEVICE(s), 0);
    msi_reset(PCI_DEVICE(s));
    msix_reset(PCI_DEVICE(s));
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
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo virt_llm_info = {
    .name = TYPE_VIRT_LLM,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VirtLLMState),
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
