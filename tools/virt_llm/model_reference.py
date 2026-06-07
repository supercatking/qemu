#!/usr/bin/env python3
"""Generate model manifest and greedy-decode golden data for virt-llm."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any


DEFAULT_MODEL = os.environ.get(
    "VIRT_LLM_MODEL_ID",
    os.environ.get("VIRT_LLM_MODEL_PATH", "/home/zyz/llmsim/models/qwen2.5-0.5b-instruct"),
)
DEFAULT_ARTIFACT_PREFIX = "model"
DEFAULT_PROMPTS = ["What is the capital of France?"]


def require_deps() -> tuple[Any, Any, Any, Any]:
    missing: list[str] = []
    try:
        import torch  # type: ignore
    except Exception:
        torch = None
        missing.append("torch")
    try:
        from transformers import AutoConfig, AutoModelForCausalLM, AutoTokenizer  # type: ignore
    except Exception:
        AutoConfig = AutoModelForCausalLM = AutoTokenizer = None
        missing.append("transformers")

    if missing:
        print(
            "Missing Python dependencies: "
            + ", ".join(missing)
            + "\nInstall example:\n"
            + "  python3 -m venv ${VIRT_LLM_REF_VENV:-.venv-virt-llm-ref}\n"
            + "  ${VIRT_LLM_REF_VENV:-.venv-virt-llm-ref}/bin/pip install "
            + "--upgrade pip\n"
            + "  ${VIRT_LLM_REF_VENV:-.venv-virt-llm-ref}/bin/pip install "
            + "--index-url https://download.pytorch.org/whl/cpu torch\n"
            + "  ${VIRT_LLM_REF_VENV:-.venv-virt-llm-ref}/bin/pip install "
            + "transformers safetensors huggingface_hub",
            file=sys.stderr,
        )
        sys.exit(2)

    return torch, AutoConfig, AutoModelForCausalLM, AutoTokenizer


def pick_dtype(torch: Any, dtype: str) -> Any:
    if dtype == "float16":
        return torch.float16
    if dtype == "bfloat16":
        return torch.bfloat16
    return torch.float32


def render_prompt(tokenizer: Any, prompt: str) -> str:
    messages = [{"role": "user", "content": prompt}]
    if getattr(tokenizer, "chat_template", None):
        return tokenizer.apply_chat_template(
            messages,
            tokenize=False,
            add_generation_prompt=True,
        )
    return prompt


def config_to_manifest(model_id: str, config: Any) -> dict[str, Any]:
    data = config.to_dict()
    keys = [
        "model_type",
        "architectures",
        "vocab_size",
        "hidden_size",
        "intermediate_size",
        "num_hidden_layers",
        "num_attention_heads",
        "num_key_value_heads",
        "max_position_embeddings",
        "rms_norm_eps",
        "rope_theta",
        "rope_parameters",
        "hidden_act",
        "attention_dropout",
        "sliding_window",
        "use_sliding_window",
        "tie_word_embeddings",
        "torch_dtype",
        "dtype",
        "bos_token_id",
        "eos_token_id",
        "pad_token_id",
    ]
    manifest = {k: data.get(k) for k in keys if k in data}
    hidden_size = data.get("hidden_size")
    num_heads = data.get("num_attention_heads")
    if hidden_size and num_heads:
        manifest["head_dim"] = hidden_size // num_heads
    if manifest.get("rope_theta") is None:
        rope_parameters = data.get("rope_parameters") or {}
        manifest["rope_theta"] = rope_parameters.get("rope_theta")
    manifest["model_id"] = model_id
    manifest["reference_dtype"] = str(data.get("torch_dtype", data.get("dtype", "unknown")))
    return manifest


def token_text(tokenizer: Any, token_id: int) -> str:
    return tokenizer.decode([token_id], skip_special_tokens=False)


def tensor_checksum(torch: Any, tensor: Any) -> dict[str, Any]:
    t = tensor.detach().to(torch.float32).cpu()
    return {
        "shape": list(t.shape),
        "sum": float(t.sum().item()),
        "mean": float(t.mean().item()),
        "abs_sum": float(t.abs().sum().item()),
        "max": float(t.max().item()),
        "min": float(t.min().item()),
    }


def collect_top_logits(torch: Any, tokenizer: Any, logits: Any, top_k: int) -> list[dict[str, Any]]:
    k = min(top_k, int(logits.numel()))
    values, indices = torch.topk(logits.detach().to(torch.float32).cpu(), k=k)
    return [
        {
            "token_id": int(token_id),
            "logit": float(logit),
            "text": token_text(tokenizer, int(token_id)),
        }
        for token_id, logit in zip(indices.tolist(), values.tolist())
    ]


def write_manifest(out_dir: Path, prefix: str, manifest: dict[str, Any]) -> Path:
    manifest_path = out_dir / f"{prefix}_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    return manifest_path


def run_generation(
    torch: Any,
    tokenizer: Any,
    model: Any,
    prompt: str,
    max_new_tokens: int,
    device: str,
    top_k: int,
) -> dict[str, Any]:
    rendered = render_prompt(tokenizer, prompt)
    encoded = tokenizer(rendered, return_tensors="pt")
    encoded = {k: v.to(device) for k, v in encoded.items()}
    current_ids = encoded["input_ids"]
    attention_mask = encoded.get("attention_mask")
    step_top_logits: list[dict[str, Any]] = []
    first_logits = None

    with torch.no_grad():
        for step in range(max_new_tokens):
            model_inputs = {"input_ids": current_ids}
            if attention_mask is not None:
                model_inputs["attention_mask"] = attention_mask
            logits = model(**model_inputs).logits[:, -1, :]
            if first_logits is None:
                first_logits = logits
            selected_token_id = int(torch.argmax(logits[0]).detach().cpu().item())
            step_top_logits.append(
                {
                    "step": step,
                    "input_len": int(current_ids.shape[-1]),
                    "selected_token_id": selected_token_id,
                    "selected_token_text": token_text(tokenizer, selected_token_id),
                    "top_logits": collect_top_logits(torch, tokenizer, logits[0], top_k),
                }
            )

            next_token = torch.tensor(
                [[selected_token_id]],
                dtype=current_ids.dtype,
                device=current_ids.device,
            )
            current_ids = torch.cat([current_ids, next_token], dim=-1)
            if attention_mask is not None:
                next_attention = torch.ones(
                    (attention_mask.shape[0], 1),
                    dtype=attention_mask.dtype,
                    device=attention_mask.device,
                )
                attention_mask = torch.cat([attention_mask, next_attention], dim=-1)

    input_ids = encoded["input_ids"][0].detach().cpu().tolist()
    output_ids = current_ids[0].detach().cpu().tolist()
    new_token_ids = output_ids[len(input_ids):]
    expected_first_new_token = new_token_ids[0] if new_token_ids else None
    top_logits = step_top_logits[0]["top_logits"] if step_top_logits else []
    if first_logits is None:
        first_logits = torch.empty((1, 0), device=device)
    return {
        "prompt": prompt,
        "rendered_prompt": rendered,
        "input_ids": input_ids,
        "input_len": len(input_ids),
        "new_token_ids": new_token_ids,
        "expected_first_new_token": expected_first_new_token,
        "expected_first_new_token_text": (
            token_text(tokenizer, expected_first_new_token)
            if expected_first_new_token is not None
            else ""
        ),
        "output_ids": output_ids,
        "top_logits": top_logits,
        "step_top_logits": step_top_logits,
        "last_prompt_logits_checksum": tensor_checksum(torch, first_logits),
        "decoded_output": tokenizer.decode(output_ids, skip_special_tokens=False),
        "decoded_new_text": tokenizer.decode(new_token_ids, skip_special_tokens=False),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-id", default=DEFAULT_MODEL)
    parser.add_argument(
        "--out-dir",
        default=os.environ.get("VIRT_LLM_ARTIFACT_DIR", "./virt-llm-artifacts/qwen2.5-0.5b-instruct"),
    )
    parser.add_argument("--artifact-prefix", default=DEFAULT_ARTIFACT_PREFIX)
    parser.add_argument("--prompt", action="append", default=[])
    parser.add_argument("--max-new-tokens", type=int, default=1)
    parser.add_argument("--top-k", type=int, default=5)
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="cpu")
    parser.add_argument("--dtype", choices=["float32", "float16", "bfloat16"], default="float32")
    parser.add_argument(
        "--local-files-only",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Only read an already downloaded Hugging Face snapshot or local model path.",
    )
    parser.add_argument(
        "--manifest-only",
        action="store_true",
        help="Write config-derived manifest without loading model weights or generating tokens.",
    )
    args = parser.parse_args()

    torch, AutoConfig, AutoModelForCausalLM, AutoTokenizer = require_deps()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    device = "cuda" if args.device == "cuda" else "cpu"
    if args.device == "auto" and torch.cuda.is_available():
        device = "cuda"
    dtype = pick_dtype(torch, args.dtype)

    torch.manual_seed(0)
    if hasattr(torch, "set_num_threads"):
        torch.set_num_threads(int(os.environ.get("VIRT_LLM_REF_THREADS", "1")))
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(0)
    hf_load_args = {"local_files_only": args.local_files_only}
    config = AutoConfig.from_pretrained(args.model_id, **hf_load_args)
    prompts = args.prompt or DEFAULT_PROMPTS
    manifest = config_to_manifest(args.model_id, config)
    manifest.update(
        {
            "reference_device": device,
            "reference_load_dtype": args.dtype,
            "local_files_only": args.local_files_only,
            "max_new_tokens": args.max_new_tokens,
            "top_k": args.top_k,
            "prompts": prompts,
        }
    )

    manifest_path = write_manifest(out_dir, args.artifact_prefix, manifest)
    print(f"Wrote {manifest_path}")
    if args.manifest_only:
        return 0

    tokenizer = AutoTokenizer.from_pretrained(args.model_id, **hf_load_args)
    model = AutoModelForCausalLM.from_pretrained(
        args.model_id,
        torch_dtype=dtype,
        low_cpu_mem_usage=True,
        **hf_load_args,
    )
    model.to(device)
    model.eval()
    golden = {
        "model_id": args.model_id,
        "manifest": manifest,
        "max_new_tokens": args.max_new_tokens,
        "device": device,
        "dtype": args.dtype,
        "local_files_only": args.local_files_only,
        "generations": [
            run_generation(torch, tokenizer, model, prompt, args.max_new_tokens, device, args.top_k)
            for prompt in prompts
        ],
    }

    golden_path = out_dir / f"{args.artifact_prefix}_golden.json"
    golden_path.write_text(json.dumps(golden, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")

    print(f"Wrote {golden_path}")
    for item in golden["generations"]:
        print(f"PROMPT: {item['prompt']}")
        print(f"INPUT_IDS: {item['input_ids']}")
        print(f"NEW_TOKEN_IDS: {item['new_token_ids']}")
        print(f"EXPECTED_FIRST_NEW_TOKEN: {item['expected_first_new_token']}")
        print(f"TOP_LOGITS: {item['top_logits']}")
        if args.max_new_tokens > 1:
            print(f"STEP_TOP_LOGITS: {item['step_top_logits']}")
        print(f"NEW_TEXT: {item['decoded_new_text']!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
