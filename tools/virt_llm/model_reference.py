#!/usr/bin/env python3
"""Generate model manifest and greedy-decode golden data for virt-llm."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


DEFAULT_MODEL = "HuggingFaceTB/SmolLM-135M-Instruct"
DEFAULT_ARTIFACT_PREFIX = "model"
DEFAULT_PROMPTS = [
    "What is the capital of France?",
    "Write one short sentence about RISC-V.",
]


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
            + "  python3 -m venv /home/qemu/virt-llm-ref-venv\n"
            + "  /home/qemu/virt-llm-ref-venv/bin/pip install "
            + "torch transformers safetensors huggingface_hub",
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
        "tie_word_embeddings",
        "torch_dtype",
        "bos_token_id",
        "eos_token_id",
        "pad_token_id",
    ]
    manifest = {k: data.get(k) for k in keys if k in data}
    manifest["model_id"] = model_id
    manifest["reference_dtype"] = str(data.get("torch_dtype", "unknown"))
    return manifest


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
) -> dict[str, Any]:
    rendered = render_prompt(tokenizer, prompt)
    encoded = tokenizer(rendered, return_tensors="pt")
    encoded = {k: v.to(device) for k, v in encoded.items()}

    with torch.no_grad():
        generated = model.generate(
            **encoded,
            do_sample=False,
            temperature=None,
            top_p=None,
            top_k=None,
            max_new_tokens=max_new_tokens,
            pad_token_id=tokenizer.eos_token_id,
        )

    input_ids = encoded["input_ids"][0].detach().cpu().tolist()
    output_ids = generated[0].detach().cpu().tolist()
    new_token_ids = output_ids[len(input_ids):]
    return {
        "prompt": prompt,
        "rendered_prompt": rendered,
        "input_ids": input_ids,
        "new_token_ids": new_token_ids,
        "output_ids": output_ids,
        "decoded_output": tokenizer.decode(output_ids, skip_special_tokens=False),
        "decoded_new_text": tokenizer.decode(new_token_ids, skip_special_tokens=False),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model-id", default=DEFAULT_MODEL)
    parser.add_argument("--out-dir", default="/home/qemu/virt-llm-artifacts/smollm135")
    parser.add_argument("--artifact-prefix", default=DEFAULT_ARTIFACT_PREFIX)
    parser.add_argument("--prompt", action="append", default=[])
    parser.add_argument("--max-new-tokens", type=int, default=8)
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="cpu")
    parser.add_argument("--dtype", choices=["float32", "float16", "bfloat16"], default="float32")
    parser.add_argument(
        "--local-files-only",
        action="store_true",
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
    hf_load_args = {"local_files_only": args.local_files_only}
    config = AutoConfig.from_pretrained(args.model_id, **hf_load_args)
    prompts = args.prompt or DEFAULT_PROMPTS
    manifest = config_to_manifest(args.model_id, config)
    manifest.update(
        {
            "reference_device": device,
            "reference_load_dtype": args.dtype,
            "max_new_tokens": args.max_new_tokens,
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
        "max_new_tokens": args.max_new_tokens,
        "device": device,
        "dtype": args.dtype,
        "generations": [
            run_generation(torch, tokenizer, model, prompt, args.max_new_tokens, device)
            for prompt in prompts
        ],
    }

    golden_path = out_dir / f"{args.artifact_prefix}_golden.json"
    golden_path.write_text(json.dumps(golden, indent=2, sort_keys=True) + "\n",
                           encoding="utf-8")

    print(f"Wrote {golden_path}")
    for item in golden["generations"]:
        print(f"PROMPT: {item['prompt']}")
        print(f"NEW_TOKEN_IDS: {item['new_token_ids']}")
        print(f"NEW_TEXT: {item['decoded_new_text']!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
