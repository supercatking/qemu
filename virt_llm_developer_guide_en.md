# virt-llm Developer Guide

Last updated: 2026-06-08

## 1. Purpose

This guide is for engineers extending `virt-llm`. It describes the source tree,
common development workflows, debugging entry points, validation gates, and
commit rules. Read `virt_llm_platform_setup_en.md` first for platform setup and
`virt_llm_platform_architecture_en.md` first for the hardware/software model.

## 2. Source Tree Map

QEMU repository:

| Path | Purpose |
|---|---|
| `hw/misc/virt_llm.c` | QEMU PCIe device model, BARs, queues, DMA, backends, Qwen ops |
| `tools/virt_llm/common.sh` | Shared environment variables, path resolution, progress logging |
| `tools/virt_llm/build_qemu_virt_llm.sh` | Build riscv32/riscv64 QEMU targets |
| `tools/virt_llm/build_linux_6_12_riscv.sh` | Generic Linux 6.12 riscv32/riscv64 build entry |
| `tools/virt_llm/build_initramfs.sh` | Build test/console/qwen initramfs images |
| `tools/virt_llm/run_virt_llm_validation.sh` | Basic boot/probe/GEMM/attention gate |
| `tools/virt_llm/run_virt_llm_qwen.sh` | Qwen 8-token exact-match gate |
| `tools/virt_llm/reproduce_fresh_virt_llm_platform.sh` | Fresh-clone reproduction gate |
| `tools/virt_llm/model_reference.py` | Host golden/reference generator |
| `virt_llm*.md` | Plans, architecture docs, setup docs, multi-agent docs, developer docs |

Linux repository:

| Path | Purpose |
|---|---|
| `drivers/misc/virt_llm_pci.c` | Linux 6.12 guest driver |
| `tools/testing/selftests/virt_llm/virt-llm-test.c` | Freestanding basic test runtime |
| `tools/testing/selftests/virt_llm/virt-llm-console.c` | Manual console runtime |
| `tools/testing/selftests/virt_llm/virt-llm-qwen.c` | Qwen op graph runtime |

## 3. Standard Development Loop

```bash
cd /home/qemu/qemu
git status --short
tools/virt_llm/build_qemu_virt_llm.sh
VIRT_LLM_GUEST_BITS=32 tools/virt_llm/run_virt_llm_validation.sh
VIRT_LLM_GUEST_BITS=64 tools/virt_llm/run_virt_llm_validation.sh
```

If the change touches Qwen, tensor backends, model loading, guest runtime, or
reference generation:

```bash
VIRT_LLM_MODEL_PATH=/home/zyz/llmsim/models/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

If the change touches scripts, paths, dependencies, build behavior, or
reproduction docs:

```bash
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

## 4. Adding an Opcode

Opcode changes must update QEMU, Linux/runtime, reference data, validation, and
documentation.

Steps:

1. Define the opcode name, number, inputs, outputs, error semantics, and backend
   in the architecture docs.
2. Add the opcode constant in `hw/misc/virt_llm.c`.
3. Add the descriptor dispatch case.
4. Implement the backend helper.
5. Fill completion status/backend/error fields.
6. Submit the matching descriptor from Linux driver/runtime.
7. Add a known-answer test.
8. Update Chinese and English architecture/developer docs.
9. Run the base gate and the relevant feature gate.

Rules:

- opcode numbers must not collide;
- unknown opcodes must continue to return `DESC_UNSUPP` and opcode error;
- bad tensor, bad shape, and bad dtype errors must be distinct from unknown
  opcode.

## 5. Changing ABI

ABI includes:

- BAR register offsets;
- feature bits;
- descriptor fields;
- completion entry fields;
- queue status/error fields;
- tensor request layout;
- ioctl contract;
- success/failure markers.

ABI change rules:

1. Update `virt_llm_platform_architecture_zh.md` and
   `virt_llm_platform_architecture_en.md` first.
2. Ask the ABI/Docs Steward to confirm visible behavior.
3. Update QEMU and Linux together.
4. Preserve compatibility unless the milestone explicitly permits a breaking
   change.
5. Add or update negative tests.

## 6. DMA and Memory Debugging

The current data path for most ops is:

```text
guest DMA buffer -> pci_dma_read -> QEMU host heap -> backend compute -> pci_dma_write -> guest DMA buffer
```

Debug checklist:

- descriptor guest physical addresses;
- input/output buffer size;
- QEMU host heap copy bounds;
- CQ status/backend/error fields;
- whether runtime reads the output buffer or stale input buffer;
- structure layout consistency between riscv32 and riscv64.

## 7. Qwen Development Rules

Current Qwen correctness baseline:

```text
Prompt: What is the capital of France?
Output tokens: 785,6722,315,9625,374,12095,13,151645
```

Rules:

1. FP32 exact-match is the correctness baseline.
2. Tokenizer is not a hardware op yet.
3. `VIRT_LLM_MODEL_PATH` is the model input.
4. Any change to RoPE, RMSNorm, attention mask, GEMM, LM head, or argmax must
   run the Qwen gate.
5. If token output diverges, report expected/current token, step, and first
   divergent layer/op.

## 8. Logs and Failure Triage

Base logs:

```text
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv32-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-riscv64-linux-6.12.log
$QEMU_BUILD/virt-llm-artifacts/logs/virt-llm-qwen.log
```

Fresh clone logs:

```text
/tmp/virt-llm-platform-repro/summary.md
/tmp/virt-llm-platform-repro/validation-rv32.log
/tmp/virt-llm-platform-repro/validation-rv64.log
/tmp/virt-llm-platform-repro/qwen-validation.log
```

Common failures:

| Symptom | Check first |
|---|---|
| QEMU cannot find `virt-llm` | target build, device registration under `hw/misc` |
| Linux does not probe | `CONFIG_VIRT_LLM_PCI=y` |
| DMA checksum mismatch | descriptor address, buffer length, copy direction |
| no CQ completion | queue head/tail, doorbell, interrupt path |
| Qwen token divergence | RoPE, RMSNorm eps, attention mask, logits position |
| fresh clone failure | repo URL, toolchain, QEMU build deps, model path |

## 9. Pre-Commit Checks

```bash
cd /home/qemu/qemu
git status --short
git diff --check
```

Do not commit:

- build outputs;
- logs;
- model files;
- Python caches;
- temporary patches;
- existing untracked `build-riscv64-user/`.

## 10. Push Rules

QEMU:

```bash
git push supercatking HEAD:llmdev
```

Linux:

```bash
cd /home/qemu/linux-6.12
git push supercatking HEAD:llmdev-linux-6.12
```

After pushing, verify:

```bash
git ls-remote supercatking refs/heads/llmdev
git rev-parse HEAD
```

Use the matching Linux branch for Linux verification.
