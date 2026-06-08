# virt-llm Documentation Index / 文档索引

Last updated: 2026-06-08

This file is the stable entry point for the `virt-llm` documentation set. The
project intentionally keeps Chinese and English documents side by side so the
platform can be reviewed, reproduced, and extended by both local and external
teams.

本文档是 `virt-llm` 文档集的统一入口。项目同时维护中文和英文文档，便于本地团队和外部团队理解、复现和继续开发。

## Core Platform Docs / 核心平台文档

| Topic | Chinese | English |
|---|---|---|
| Platform setup and reproduction | `virt_llm_platform_setup_zh.md` | `virt_llm_platform_setup_en.md` |
| Platform architecture | `virt_llm_platform_architecture_zh.md` | `virt_llm_platform_architecture_en.md` |
| Project plan and milestones | `virt_llm_project_plan_zh.md` | `virt_llm_project_plan_en.md` |
| Multi-agent continuous development | `virt_llm_multi_agent_development_zh.md` | `virt_llm_multi_agent_development_en.md` |
| Developer guide | `virt_llm_developer_guide_zh.md` | `virt_llm_developer_guide_en.md` |
| Tools reference | `virt_llm_tools_reference_zh.md` | `virt_llm_tools_reference_en.md` |
| Troubleshooting | `virt_llm_troubleshooting_zh.md` | `virt_llm_troubleshooting_en.md` |

## Existing Design Notes / 既有设计文档

These files record earlier design steps and should remain available for
traceability:

| File | Purpose |
|---|---|
| `virt_llm.md` | Initial minimal PCIe device idea and bring-up notes |
| `virt_llm_architecture.md` | Early architecture notes |
| `virt_llm_architecture_v2.md` | Queue/DMA/MSI/scalar dispatch architecture update |
| `virt_llm_queue_dma.md` | Queue, DMA buffer, interrupt, and command simulation design |
| `virt_llm_riscv_dispatch.md` | RISC-V scalar dispatcher design |
| `virt_llm_phase4_plan.md` | Phase 4 scalar/kernel ABI validation plan |
| `virt_llm_qwen_architecture.md` | Qwen bring-up architecture notes |
| `virt_llm_qwen_per_op_plan.md` | Per-op Qwen inference plan |
| `virt_llm_135m_inference_plan.md` | Earlier 135M model inference target plan |
| `virt_llm_next_steps.md` | Historical next-step notes |
| `virt-llm-v1.md` | V1.0 release overview, if present in the checkout |
| `virt_llm_current_status_report_zh.md` | Current Chinese status report |
| `virt_llm_architecture_report_zh.md` | Detailed Chinese architecture report |

## Recommended Reading Order / 推荐阅读顺序

For a new engineer:

1. `virt_llm_platform_setup_en.md` or `virt_llm_platform_setup_zh.md`
2. `virt_llm_platform_architecture_en.md` or `virt_llm_platform_architecture_zh.md`
3. `virt_llm_multi_agent_development_en.md` or `virt_llm_multi_agent_development_zh.md`
4. `virt_llm_project_plan_en.md` or `virt_llm_project_plan_zh.md`
5. `virt_llm_developer_guide_en.md` or `virt_llm_developer_guide_zh.md`
6. `virt_llm_tools_reference_en.md` or `virt_llm_tools_reference_zh.md`
7. `virt_llm_troubleshooting_en.md` or `virt_llm_troubleshooting_zh.md`
8. Qwen-specific design notes if working on inference correctness

对新加入开发者：

1. 先读平台搭建文档；
2. 再读平台架构文档；
3. 然后读多 agent 持续开发规范；
4. 最后读项目计划和 Qwen 专项文档。
5. 如果要改代码，再读开发者指南、工具参考和排障手册。

## Validation Entry Points / 验证入口

The main scripts live under `tools/virt_llm/`.

主要脚本位于 `tools/virt_llm/`。

```bash
# Build QEMU riscv32/riscv64 and validate Linux guests.
tools/virt_llm/rebuild_and_validate_virt_llm.sh

# Fresh-clone platform reproduction under /tmp.
REPORT_DIR=/tmp/virt-llm-platform-repro \
  tools/virt_llm/reproduce_fresh_virt_llm_platform.sh

# Optional Qwen exact-match gate.
VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors \
  tools/virt_llm/run_virt_llm_qwen.sh
```

## Documentation Maintenance Rules / 文档维护规则

- Any new public script must be listed in the setup docs.
- Any new hardware block, backend, opcode family, or ABI field must be reflected
  in the architecture docs.
- Any milestone change must update the project plan docs.
- Any multi-agent development change must update the multi-agent docs.
- Chinese and English docs should describe the same technical facts. Exact
  wording may differ, but commands, acceptance gates, and file paths must match.
- Developer-facing changes should update the developer guides.
- Script changes should update the tools reference.
- New common failures should update the troubleshooting docs.

- 新增公开脚本必须更新平台搭建文档。
- 新增硬件模块、backend、opcode 或 ABI 字段必须更新架构文档。
- milestone 变化必须更新项目计划文档。
- 多 agent 流程变化必须更新多 agent 开发文档。
- 中英文文档的技术事实必须一致，尤其是命令、验收 gate 和路径。
- 面向开发者的代码改动流程变化必须更新开发者指南。
- 脚本变化必须更新工具参考文档。
- 新增常见失败模式必须更新排障文档。
