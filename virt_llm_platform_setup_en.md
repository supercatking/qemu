# virt-llm Platform Setup and Reproduction Guide

Last updated: 2026-06-08

## 1. Purpose

This guide explains how to reproduce the `virt-llm` platform on a new machine.
The current platform can:

1. Build QEMU with the `virt-llm` PCIe virtual accelerator.
2. Build both `qemu-system-riscv32` and `qemu-system-riscv64`.
3. Build Linux 6.12 guest Images for riscv32 and riscv64.
4. Generate initramfs images automatically.
5. Boot guest Linux and validate PCI probe, GEMM, attention, and boot markers.
6. Optionally run the Qwen2.5-0.5B Instruct 8-token exact-match gate.
7. Avoid hard-coded local paths through environment variables.

## 2. Repositories and Branches

| Project | GitHub | Branch |
|---|---|---|
| QEMU | `https://github.com/supercatking/qemu.git` | `llmdev` |
| Linux | `https://github.com/supercatking/linux.git` | `llmdev-linux-6.12` |

For an intranet environment, replace the URLs with internal mirrors through
`QEMU_REPO` and `LINUX_REPO`.

## 3. Required Dependencies

Required:

- `git`
- `make`
- `ninja`
- `python3`
- `cpio`
- RISC-V cross compiler, default: `riscv64-linux-gnu-gcc`
- Standard QEMU build dependencies such as glib, pixman, Meson/Python venv support
- Standard Linux build dependencies such as bc, bison, flex, OpenSSL headers, and elfutils headers

Optional:

- Qwen model file: `qwen2.5-0.5b-instruct/model.safetensors`
- Python Transformers environment for generating or updating host golden data

The Qwen model is only required for the Qwen exact-match gate. It is not needed
for basic boot/probe/GEMM/attention validation.

## 4. Environment Variables

| Variable | Default | Meaning |
|---|---|---|
| `QEMU_SRC` | current QEMU repo root | QEMU source tree |
| `QEMU_BUILD` | `$QEMU_SRC/build` | QEMU build directory |
| `QEMU_TARGET_LIST` | `riscv32-softmmu,riscv64-softmmu` | QEMU system targets |
| `VIRT_LLM_GUEST_BITS` | `32` | Current guest width: `32` or `64` |
| `VIRT_LLM_GUEST_BITS_LIST` | `32 64` | Guest widths covered by one-click scripts |
| `LINUX_SRC` | `/home/qemu/linux-6.12` | Linux source tree |
| `LINUX_BUILD` | `/home/qemu/linux-6.12-build-rv$VIRT_LLM_GUEST_BITS` | Linux build directory |
| `LINUX_IMAGE` | `$LINUX_BUILD/arch/riscv/boot/Image` | Guest kernel Image |
| `CROSS_COMPILE` | `riscv64-linux-gnu-` | Cross compiler prefix |
| `VIRT_LLM_MODEL_PATH` | local model path | Optional Qwen model file |
| `VIRT_LLM_LOG_DIR` | `$QEMU_BUILD/virt-llm-artifacts/logs` | Runtime log directory |
| `JOBS` | `nproc` | Parallel build jobs |

Reference file:

```bash
source tools/virt_llm/env.example
```

## 5. One-Click Build and Validation

Run inside the QEMU repository:

```bash
cd /home/qemu/qemu
tools/virt_llm/rebuild_and_validate_virt_llm.sh
```

Default behavior:

1. Build QEMU targets `riscv32-softmmu,riscv64-softmmu`.
2. Build Linux 6.12 riscv32 Image.
3. Boot `qemu-system-riscv32` and validate `virt-llm`.
4. Build Linux 6.12 riscv64 Image.
5. Boot `qemu-system-riscv64` and validate `virt-llm`.

Progress is printed with timestamps:

```text
[2026-06-08 10:00:00] [virt-llm] >>> build QEMU virt-llm targets=...
[2026-06-08 10:02:00] [virt-llm] >>> build Linux Image guest=riscv64 jobs=32
[2026-06-08 10:04:00] [virt-llm] >>> run basic validation guest=riscv64 ...
```

## 6. Single-Step Commands

Build QEMU only:

```bash
tools/virt_llm/build_qemu_virt_llm.sh
```

Build riscv32 Linux only:

```bash
tools/virt_llm/build_linux_6_12_rv32.sh
```

Build riscv64 Linux only:

```bash
tools/virt_llm/build_linux_6_12_rv64.sh
```

Validate riscv32:

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/run_virt_llm_validation.sh
```

Validate riscv64:

```bash
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/run_virt_llm_validation.sh
```

Run Qwen exact-match:

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

## 7. Fresh Clone Reproduction

Run a full fresh-clone reproduction under `/tmp`:

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

Use intranet mirrors:

```bash
QEMU_REPO=https://your-intranet/qemu.git \
LINUX_REPO=https://your-intranet/linux.git \
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

Enable optional Qwen validation:

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
REPORT_DIR=/tmp/virt-llm-platform-repro-qwen \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

## 8. Acceptance Markers

Basic platform gate:

```text
probe ok:
gemm ok:
attention q16 ok:
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv64
```

Qwen gate:

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ... output_tokens=785,6722,315,9625,374,12095,13,151645
```

Fresh clone summary:

```text
Conclusion: PASS
```

## 9. Current Limits

- riscv64 currently covers the CPU/guest boot and basic `virt-llm` PCIe path.
- Qwen exact-match remains the riscv32 correctness baseline.
- Device-side DDR and SRAM timing are not modeled yet.
- CUDA/RTX is not part of the correctness baseline.
