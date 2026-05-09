/*
 * Minimal virtual PCIe LLM accelerator prototype.
 *
 * This is intentionally tiny: it only proves PCI enumeration, BAR mapping,
 * MMIO read/write, and a device-side state response.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_VIRT_LLM "virt-llm"
OBJECT_DECLARE_SIMPLE_TYPE(VirtLLMState, VIRT_LLM)

#define VIRT_LLM_DEVICE_ID 0x1100

#define VIRT_LLM_REG_MAGIC    0x00
#define VIRT_LLM_REG_VERSION  0x04
#define VIRT_LLM_REG_DOORBELL 0x08
#define VIRT_LLM_REG_STATUS   0x0c

#define VIRT_LLM_MAGIC        0x4c4c4d31u /* "LLM1" */
#define VIRT_LLM_VERSION      1u
#define VIRT_LLM_STATUS_XOR   0xa5a5a5a5u
#define VIRT_LLM_MMIO_SIZE    4 * KiB

struct VirtLLMState {
    PCIDevice parent_obj;
    MemoryRegion mmio;
    uint32_t doorbell;
};

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

    memory_region_init_io(&s->mmio, OBJECT(s), &virt_llm_mmio_ops, s,
                          "virt-llm-mmio", VIRT_LLM_MMIO_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
}

static void virt_llm_reset(DeviceState *dev)
{
    VirtLLMState *s = VIRT_LLM(dev);

    s->doorbell = 0;
}

static void virt_llm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    pc->realize = virt_llm_realize;
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
