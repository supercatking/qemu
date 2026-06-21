# virt-llm Tools Reference

Last updated: 2026-06-08

## 1. Purpose

This document describes each script under `tools/virt_llm/`, the important
environment variables, generated artifacts, logs, and success markers.

## 2. Common Environment Variables

These variables are resolved by `tools/virt_llm/common.sh`.

| Variable | Default | Meaning |
|---|---|---|
| `QEMU_SRC` | current QEMU repo | QEMU source tree |
| `QEMU_BUILD` | `$QEMU_SRC/build` | QEMU build directory |
| `QEMU_TARGET_LIST` | `riscv32-softmmu,riscv64-softmmu` | QEMU targets |
| `QEMU_SYSTEM` | `qemu-system-riscv$VIRT_LLM_GUEST_BITS` | active QEMU binary name |
| `QEMU_BIN` | `$QEMU_BUILD/$QEMU_SYSTEM` | active QEMU binary path |
| `VIRT_LLM_GUEST_BITS` | `32` | active guest width: `32` or `64` |
| `VIRT_LLM_GUEST_BITS_LIST` | `32 64` | guest widths covered by one-click scripts |
| `LINUX_SRC` | `/home/qemu/linux-6.12` | Linux source tree |
| `LINUX_BUILD` | `/home/qemu/linux-6.12-build-rv$VIRT_LLM_GUEST_BITS` | Linux build directory |
| `LINUX_IMAGE` | `$LINUX_BUILD/arch/riscv/boot/Image` | Linux kernel Image |
| `CROSS_COMPILE` | `riscv64-linux-gnu-` | cross compiler prefix |
| `VIRT_LLM_MODEL_PATH` | local Qwen safetensors | optional model path |
| `VIRT_LLM_ARTIFACT_DIR` | `$QEMU_BUILD/virt-llm-artifacts` | generated artifacts |
| `VIRT_LLM_LOG_DIR` | `$VIRT_LLM_ARTIFACT_DIR/logs` | runtime logs |
| `JOBS` | `nproc` | parallel build jobs |

## 3. `build_qemu_virt_llm.sh`

Builds QEMU system targets.

```bash
tools/virt_llm/build_qemu_virt_llm.sh
```

Default outputs:

```text
qemu-system-riscv32
qemu-system-riscv64
```

Useful variables:

| Variable | Meaning |
|---|---|
| `QEMU_TARGET_LIST` | override target list |
| `QEMU_CONFIGURE_FLAGS` | extra configure flags, default `--disable-werror` |
| `QEMU_DEBUG_BUILD=1` | append `--enable-debug` |
| `QEMU_FORCE_CONFIGURE=1` | force configure |

## 4. `build_linux_6_12_riscv.sh`

Generic Linux 6.12 riscv32/riscv64 build entry.

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/build_linux_6_12_riscv.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/build_linux_6_12_riscv.sh
```

Wrappers:

```bash
tools/virt_llm/build_linux_6_12_rv32.sh
tools/virt_llm/build_linux_6_12_rv64.sh
```

Output:

```text
$LINUX_BUILD/arch/riscv/boot/Image
```

## 5. `build_initramfs.sh`

Builds freestanding initramfs images.

| mode | init program | use |
|---|---|---|
| `test` | inline assembly init | boot marker and driver probe selftest |
| `console` | `virt-llm-console.c` | manual console |
| `qwen` | `virt-llm-qwen.c` | Qwen exact-match runtime |

Examples:

```bash
tools/virt_llm/build_initramfs.sh --mode test --out /tmp/initramfs-test.cpio
tools/virt_llm/build_initramfs.sh --mode console --out /tmp/initramfs-console.cpio
tools/virt_llm/build_initramfs.sh --mode qwen --out /tmp/initramfs-qwen.cpio
```

With `VIRT_LLM_GUEST_BITS=64`, the script uses rv64/lp64 compiler arguments.

## 6. `run_virt_llm_validation.sh`

Boots QEMU + Linux and runs the basic gate.

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/run_virt_llm_validation.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/run_virt_llm_validation.sh
```

Success markers:

```text
probe ok:
gemm ok:
attention q16 ok:
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv64
```

Optional variables:

| Variable | Meaning |
|---|---|
| `INITRD` | custom initramfs |
| `LOG` | custom log path |
| `VIRT_LLM_BOOT_TIMEOUT` | boot timeout, default `90s` |

## 7. `run_virt_llm_qwen.sh`

Runs the Qwen2.5-0.5B Instruct 8-token exact-match gate.

```bash
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

Success markers:

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK ... output_tokens=785,6722,315,9625,374,12095,13,151645
```

## 8. `rebuild_and_validate_virt_llm.sh`

One-click platform build and validation.

```bash
tools/virt_llm/rebuild_and_validate_virt_llm.sh
```

Useful variables:

| Variable | Meaning |
|---|---|
| `VIRT_LLM_GUEST_BITS_LIST` | default `32 64` |
| `VIRT_LLM_RUN_VALIDATION` | run basic validation, default `1` |
| `VIRT_LLM_RUN_QWEN` | append Qwen validation, default `0` |
| `VIRT_LLM_QWEN_BITS` | guest bits for Qwen validation, default `32` |

## 9. `reproduce_fresh_virt_llm_platform.sh`

Fresh-clones QEMU/Linux under `/tmp`, rebuilds both, and runs validation.

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

Outputs include `summary.md`, QEMU/Linux build logs, per-guest validation logs,
and optional Qwen validation logs.
