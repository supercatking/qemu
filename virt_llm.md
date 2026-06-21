# virt-llm PCIe Prototype Plan

## Goal

Build the smallest useful PCIe prototype for a future LLM inference accelerator model:

- QEMU exposes a synthetic PCI device named `virt-llm`.
- The device has one MMIO BAR with a few simple registers.
- A Linux driver binds to the PCI vendor/device ID.
- The driver reads identity registers, performs a simple write/read doorbell test, and reports success in the guest kernel log.
- QEMU riscv32 `virt` boots Linux 6.12 with the device attached and proves the whole path works.

## Device Contract

PCI identity:

- Vendor ID: `0x1b36` (QEMU experimental/vendor range)
- Device ID: `0x1100`
- Class: processor/co-processor style prototype device
- QEMU command line: `-device virt-llm`

BAR0 MMIO layout:

| Offset | Name | Access | Meaning |
| --- | --- | --- | --- |
| `0x00` | MAGIC | RO | `0x4c4c4d31` (`LLM1`) |
| `0x04` | VERSION | RO | Prototype version, initially `1` |
| `0x08` | DOORBELL | RW | Guest writes a command/test value |
| `0x0c` | STATUS | RO | Reflects `DOORBELL ^ 0xa5a5a5a5` |

This gives us a tiny but realistic PCIe/MMIO path: enumeration, BAR mapping, register read, register write, and device state response.

## QEMU Implementation

- Add a new QEMU PCI device under `hw/misc/virt_llm.c`.
- Register it as `virt-llm`.
- Implement BAR0 via `MemoryRegionOps`.
- Keep device state intentionally small: only the last doorbell value.
- Add the object to the misc build and PCI Kconfig path so it is compiled into `riscv32-softmmu`.

## Linux Driver Implementation

- Add a small built-in driver under Linux `drivers/misc/virt_llm_pci.c`.
- Match `PCI_VENDOR_ID_QEMU` / `0x1100`.
- Enable the PCI device, request regions, map BAR0, read `MAGIC`/`VERSION`, write the doorbell test value, read back `STATUS`, and print a clear success line.
- Build it into the riscv32 kernel for simple initramfs boot validation.

## Validation

Boot command shape:

```bash
/home/qemu/qemu/build/qemu-system-riscv32 \
  -machine virt \
  -nographic \
  -m 256M \
  -smp 1 \
  -no-reboot \
  -device virt-llm \
  -kernel /home/qemu/linux-6.12-build-rv32/arch/riscv/boot/Image \
  -initrd /home/qemu/initramfs.cpio \
  -append "console=ttyS0 earlycon=sbi rdinit=/init loglevel=8"
```

Success criteria:

- QEMU accepts `-device virt-llm`.
- Guest PCI enumerates the device.
- Linux driver probe runs.
- Kernel log contains a line like:

```text
virt_llm_pci ... probe ok: magic=0x4c4c4d31 version=1 status=<expected>
```

- Existing initramfs still prints:

```text
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
```
