# virt-llm Agent 操作手册

本文件是给 AI coding agent 使用的最高优先级操作指南，目标环境是受限
Linux x86 服务器：普通用户权限、不能使用 `sudo`、网络白名单、只能访问
GitHub 或少量已批准站点。agent 必须按本文完成 virt-llm 的代码获取、依赖
检查、编译、执行、验证和失败报告。

本文不替代仓库里的长文档；需要背景时再阅读：

- `virt_llm_docs_index.md`
- `virt_llm_platform_setup_en.md`
- `virt_llm_platform_setup_zh.md`
- `virt_llm_tools_reference_en.md`
- `virt_llm_troubleshooting_en.md`

## 1. 基本规则

- 不使用 `sudo`，不安装系统包，不修改 `/usr`、`/opt`、`/etc` 等系统目录。
- 不下载或提交模型文件，不提交 build 目录、日志大文件、临时 cpio、内核 Image。
- 不把 `/home/qemu`、`/home/zyz`、Windows 路径写成必需路径。
- 所有路径必须通过环境变量配置；默认只使用当前目录下的 `work/`。
- 网络只访问 GitHub。Qwen 模型不从 Hugging Face 下载，必须由本地已有文件通过
  `VIRT_LLM_MODEL_PATH` 指定。
- 如果网络白名单、缺少依赖或无管理员权限导致无法继续，必须输出失败报告，不能伪装成功。
- 修改代码前先运行 `git status --short`，不得 revert 其他人已有改动。

## 2. 仓库和分支

标准仓库：

```bash
QEMU_REPO=${QEMU_REPO:-https://github.com/supercatking/qemu.git}
QEMU_BRANCH=${QEMU_BRANCH:-llmdev}
LINUX_REPO=${LINUX_REPO:-https://github.com/supercatking/linux.git}
LINUX_BRANCH=${LINUX_BRANCH:-llmdev-linux-6.12}
```

不要切换到其他仓库或分支，除非任务明确要求。

## 3. 标准环境变量

在普通用户可写目录中执行：

```bash
export WORKDIR=${WORKDIR:-$PWD/work}
export QEMU_REPO=${QEMU_REPO:-https://github.com/supercatking/qemu.git}
export QEMU_BRANCH=${QEMU_BRANCH:-llmdev}
export LINUX_REPO=${LINUX_REPO:-https://github.com/supercatking/linux.git}
export LINUX_BRANCH=${LINUX_BRANCH:-llmdev-linux-6.12}

export QEMU_SRC=${QEMU_SRC:-$WORKDIR/qemu}
export LINUX_SRC=${LINUX_SRC:-$WORKDIR/linux}
export QEMU_BUILD=${QEMU_BUILD:-$QEMU_SRC/build}
export LINUX_BUILD=${LINUX_BUILD:-$WORKDIR/linux-build-rv${VIRT_LLM_GUEST_BITS:-32}}
export CROSS_COMPILE=${CROSS_COMPILE:-riscv64-linux-gnu-}
export JOBS=${JOBS:-$(nproc)}
export REPORT_DIR=${REPORT_DIR:-$WORKDIR/reports}
```

Qwen exact-match 验证只在模型文件已经存在时运行：

```bash
export VIRT_LLM_MODEL_PATH=/path/to/qwen2.5-0.5b-instruct/model.safetensors
```

如果 `VIRT_LLM_MODEL_PATH` 未设置或文件不存在，必须明确报告：

```text
Qwen validation skipped: VIRT_LLM_MODEL_PATH is unset or missing
```

## 4. 依赖预检查

先检查依赖，不要尝试安装：

```bash
mkdir -p "$WORKDIR" "$REPORT_DIR"

missing=0
for tool in git python3 make ninja pkg-config "${CROSS_COMPILE}gcc"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "MISSING_DEPENDENCY: $tool"
    missing=1
  fi
done

if [ "$missing" -ne 0 ]; then
  echo "Dependency preflight failed. Stop and write a failure report."
  exit 1
fi
```

如果 QEMU 或 Linux 编译后续报告缺少 `glib`、`pixman`、`openssl`、`flex`、`bison`、
`bc` 等系统依赖，同样停止并写失败报告；不要使用 `sudo` 修复。

## 5. Clone 代码

```bash
mkdir -p "$WORKDIR"

if [ ! -d "$QEMU_SRC/.git" ]; then
  git clone --branch "$QEMU_BRANCH" --depth 1 "$QEMU_REPO" "$QEMU_SRC"
fi

if [ ! -d "$LINUX_SRC/.git" ]; then
  git clone --branch "$LINUX_BRANCH" --depth 1 "$LINUX_REPO" "$LINUX_SRC"
fi

cd "$QEMU_SRC"
git status --short
git rev-parse HEAD

cd "$LINUX_SRC"
git status --short
git rev-parse HEAD
```

如果 GitHub 访问失败，不要改用未知镜像。报告网络白名单阻塞，并记录失败命令。

## 6. 编译 QEMU

```bash
cd "$QEMU_SRC"
QEMU_SRC="$QEMU_SRC" \
QEMU_BUILD="$QEMU_BUILD" \
JOBS="$JOBS" \
tools/virt_llm/build_qemu_virt_llm.sh
```

期望生成：

```text
$QEMU_BUILD/qemu-system-riscv32
$QEMU_BUILD/qemu-system-riscv64
```

## 7. 编译 Linux 6.12

rv32：

```bash
cd "$QEMU_SRC"
VIRT_LLM_GUEST_BITS=32 \
LINUX_SRC="$LINUX_SRC" \
LINUX_BUILD="$WORKDIR/linux-build-rv32" \
CROSS_COMPILE="$CROSS_COMPILE" \
JOBS="$JOBS" \
tools/virt_llm/build_linux_6_12_riscv.sh
```

rv64：

```bash
cd "$QEMU_SRC"
VIRT_LLM_GUEST_BITS=64 \
LINUX_SRC="$LINUX_SRC" \
LINUX_BUILD="$WORKDIR/linux-build-rv64" \
CROSS_COMPILE="$CROSS_COMPILE" \
JOBS="$JOBS" \
tools/virt_llm/build_linux_6_12_riscv.sh
```

期望生成：

```text
$WORKDIR/linux-build-rv32/arch/riscv/boot/Image
$WORKDIR/linux-build-rv64/arch/riscv/boot/Image
```

## 8. 基础验证

rv32：

```bash
cd "$QEMU_SRC"
VIRT_LLM_GUEST_BITS=32 \
QEMU_SRC="$QEMU_SRC" \
QEMU_BUILD="$QEMU_BUILD" \
LINUX_SRC="$LINUX_SRC" \
LINUX_BUILD="$WORKDIR/linux-build-rv32" \
REPORT_DIR="$REPORT_DIR" \
tools/virt_llm/run_virt_llm_validation.sh
```

rv64：

```bash
cd "$QEMU_SRC"
VIRT_LLM_GUEST_BITS=64 \
QEMU_SRC="$QEMU_SRC" \
QEMU_BUILD="$QEMU_BUILD" \
LINUX_SRC="$LINUX_SRC" \
LINUX_BUILD="$WORKDIR/linux-build-rv64" \
REPORT_DIR="$REPORT_DIR" \
tools/virt_llm/run_virt_llm_validation.sh
```

必须看到以下成功 marker：

```text
gemm ok
attention q16 ok
INITRAMFS_OK
```

如果缺失任意 marker，基础验证失败。

## 9. Fresh clone 复现验证

该脚本会在 `/tmp` 或 `REPORT_DIR` 指定位置重新 clone、编译和验证，用于证明没有依赖
当前工作树的隐藏状态：

```bash
cd "$QEMU_SRC"
REPORT_DIR="$REPORT_DIR/fresh-repro" \
QEMU_REPO="$QEMU_REPO" \
QEMU_BRANCH="$QEMU_BRANCH" \
LINUX_REPO="$LINUX_REPO" \
LINUX_BRANCH="$LINUX_BRANCH" \
CROSS_COMPILE="$CROSS_COMPILE" \
tools/virt_llm/reproduce_fresh_virt_llm_platform.sh
```

完成后读取：

```text
$REPORT_DIR/fresh-repro/summary.md
```

summary 必须显示 basic gate PASS。若 `VIRT_LLM_MODEL_PATH` 未设置，Qwen gate 可以 SKIP，
但必须明确说明原因。

## 10. 可选 Qwen exact-match 验证

只有当模型文件存在时运行：

```bash
if [ -n "${VIRT_LLM_MODEL_PATH:-}" ] && [ -f "$VIRT_LLM_MODEL_PATH" ]; then
  cd "$QEMU_SRC"
  VIRT_LLM_MODEL_PATH="$VIRT_LLM_MODEL_PATH" \
  VIRT_LLM_GUEST_BITS=32 \
  QEMU_SRC="$QEMU_SRC" \
  QEMU_BUILD="$QEMU_BUILD" \
  LINUX_SRC="$LINUX_SRC" \
  LINUX_BUILD="$WORKDIR/linux-build-rv32" \
  tools/virt_llm/run_virt_llm_qwen.sh
else
  echo "Qwen validation skipped: VIRT_LLM_MODEL_PATH is unset or missing"
fi
```

有模型时必须看到：

```text
qwen model load ok
qwen full layers ok
QWEN_INFER_OK
```

当前 8-token baseline 输出应包含：

```text
output_tokens=785,6722,315,9625,374,12095,13,151645
```

## 11. 失败报告格式

任何失败都必须写入 `$REPORT_DIR/failure-report.md`。最小格式：

```markdown
# virt-llm Failure Report

## Host
- date:
- hostname:
- uname:
- current user:
- pwd:

## Environment
- WORKDIR:
- QEMU_REPO:
- QEMU_BRANCH:
- LINUX_REPO:
- LINUX_BRANCH:
- QEMU_SRC:
- LINUX_SRC:
- QEMU_BUILD:
- LINUX_BUILD:
- CROSS_COMPILE:
- VIRT_LLM_MODEL_PATH:

## Commits
- QEMU commit:
- Linux commit:

## Missing Dependencies
- ...

## Commands Run
- ...

## Logs
- ...

## First Failure
- failed command:
- first missing marker:
- first relevant error line:

## Blocker Classification
- network whitelist:
- no admin permission:
- missing dependency:
- build failure:
- validation failure:
- qwen skipped or failed:
```

失败报告必须包含第一处失败证据，而不是只写“运行失败”。

## 12. 修改和提交规则

- 修改前运行 `git status --short`。
- 只提交源代码、脚本、文档和必要小型 fixture。
- 不提交：
  - `build/`
  - `build-riscv64-user/`
  - Linux build 目录
  - `.cpio`
  - `Image`
  - `*.log`
  - Qwen 模型文件
- 提交前运行：

```bash
cd "$QEMU_SRC"
git diff --check
```

- 如果改动影响 Linux driver 或 UAPI，必须同步修改 Linux 仓库并分别提交。
- 如果只改 QEMU 文档，例如本文件，Linux 仓库不需要提交。

## 13. 验收结论格式

完成后输出：

```text
RESULT: PASS or FAIL
QEMU_COMMIT: <sha>
LINUX_COMMIT: <sha>
BASIC_RV32: PASS/FAIL
BASIC_RV64: PASS/FAIL
FRESH_REPRO: PASS/FAIL
QWEN_EXACT_MATCH: PASS/FAIL/SKIP
LOG_DIR: <path>
FAILURE_REPORT: <path or none>
```

只有基础验证和 fresh clone basic gate 通过，才能报告项目可复现。只有设置了有效
`VIRT_LLM_MODEL_PATH` 且看到 `QWEN_INFER_OK`，才能报告 Qwen exact-match 通过。
