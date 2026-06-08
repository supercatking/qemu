# virt-llm Troubleshooting Guide

Last updated: 2026-06-08

## 1. First Triage

Classify the failure first:

1. QEMU build failure.
2. Linux build failure.
3. guest boot failure.
4. PCI probe/driver failure.
5. DMA/queue/CQ/IRQ failure.
6. GEMM/attention base operator failure.
7. Qwen model loading or token exact-match failure.
8. fresh-clone reproduction failure.

## 2. Logs to Check

```text
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv32-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv64-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-qwen.log
/tmp/virt-llm-platform-repro/summary.md
/tmp/virt-llm-platform-repro/validation-rv32.log
/tmp/virt-llm-platform-repro/validation-rv64.log
/tmp/virt-llm-platform-repro/qwen-validation.log
```

## 3. QEMU Build Failure

Run:

```bash
cd /home/qemu/qemu
tools/virt_llm/build_qemu_virt_llm.sh
```

Common causes:

| Symptom | Action |
|---|---|
| missing `ninja`/`python3`/`make` | install basic build tools |
| missing glib/pixman | install standard QEMU build dependencies |
| `build dir already exists` | set `QEMU_FORCE_CONFIGURE=1` or clean the build directory |
| missing `qemu-system-riscv64` | ensure `QEMU_TARGET_LIST` includes `riscv64-softmmu` |

## 4. Linux Build Failure

Run:

```bash
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/build_linux_6_12_riscv.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/build_linux_6_12_riscv.sh
```

Common causes:

| Symptom | Action |
|---|---|
| missing `riscv64-linux-gnu-gcc` | install toolchain or set `CROSS_COMPILE` |
| missing `bc`/`bison`/`flex`/OpenSSL headers | install Linux build dependencies |
| driver does not probe | check `.config` for `CONFIG_VIRT_LLM_PCI=y` |

## 5. Guest Boot or Probe Failure

Required markers:

```text
Linux version 6.12.0
virt_llm_pci ... probe ok:
INITRAMFS_OK: Linux 6.12 booted on QEMU riscv32/riscv64
```

If a marker is missing:

| Missing item | Check first |
|---|---|
| `Linux version` | correct `LINUX_IMAGE` |
| `probe ok` | QEMU `-device virt-llm`, kernel driver config |
| `INITRAMFS_OK` | initramfs built for the correct guest bits |
| QEMU timeout | increase `VIRT_LLM_BOOT_TIMEOUT`, inspect kernel panic |

## 6. DMA/Queue/CQ/IRQ Failure

Base gate should print:

```text
dma inference ok
dma copy ok
gemm ok
attention q16 ok
```

Check:

- submit queue base/head/tail;
- completion queue tail;
- descriptor status;
- queue error;
- DMA buffer address and length;
- INTx/MSI/MSI-X selection;
- riscv32/riscv64 structure layout.

## 7. Qwen Failure

Run:

```bash
VIRT_LLM_MODEL_PATH=/path/to/model.safetensors tools/virt_llm/run_virt_llm_qwen.sh
```

Common causes:

| Symptom | Action |
|---|---|
| missing `VIRT_LLM_MODEL_PATH` | set the model safetensors path |
| missing `qwen model load ok` | check model path and QEMU `model-path` property |
| token mismatch | check RoPE, RMSNorm epsilon, attention mask, LM-head logits position |
| slow runtime | expected for current FP32 host CPU backend |

## 8. Fresh Clone Failure

Run:

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

Common causes:

| Symptom | Action |
|---|---|
| GitHub clone timeout | script retries; use `QEMU_REPO`/`LINUX_REPO` on intranet |
| Qwen SKIP | `VIRT_LLM_MODEL_PATH` is unset; basic gate is still valid |
| fresh clone depends on `/home/qemu` | this is a bug; fix the script |
