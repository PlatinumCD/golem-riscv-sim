#!/usr/bin/env python3
import argparse
import hashlib
import json
import math
from pathlib import Path

import torch
from transformers import AutoTokenizer, GPT2LMHeadModel


EXPECTED_CONFIG = {
    "model_type": "gpt2",
    "n_layer": 12,
    "n_embd": 768,
    "n_head": 12,
    "n_inner": None,
    "n_positions": 1024,
    "vocab_size": 50257,
}
SEQUENCE_LENGTH = 128
PROMPT = "Once upon a time, a small computer learned to route tensors."


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model_directory", type=Path)
    args = parser.parse_args()

    source = json.loads(
        (args.model_directory / "source.json").read_text()
    )
    config = json.loads(
        (args.model_directory / "config.json").read_text()
    )
    actual_config = {
        name: config.get(name) for name in EXPECTED_CONFIG
    }
    if actual_config != EXPECTED_CONFIG:
        raise RuntimeError(
            f"unexpected compact GPT-2 configuration: {actual_config}"
        )

    tokenizer = AutoTokenizer.from_pretrained(
        args.model_directory,
        local_files_only=True,
    )
    tokenizer.pad_token = tokenizer.eos_token
    model = GPT2LMHeadModel.from_pretrained(
        args.model_directory,
        local_files_only=True,
        torch_dtype=torch.float32,
    ).eval()

    encoded = tokenizer(
        PROMPT,
        max_length=SEQUENCE_LENGTH,
        padding="max_length",
        truncation=True,
        return_tensors="pt",
    )
    with torch.inference_mode():
        logits = model(
            input_ids=encoded["input_ids"],
            attention_mask=encoded["attention_mask"],
            use_cache=False,
        ).logits

    expected_shape = (1, SEQUENCE_LENGTH, EXPECTED_CONFIG["vocab_size"])
    if tuple(logits.shape) != expected_shape:
        raise RuntimeError(
            f"unexpected logits shape {tuple(logits.shape)}; "
            f"expected {expected_shape}"
        )
    checksum = logits.double().sum().item()
    if not math.isfinite(checksum):
        raise RuntimeError("model produced a non-finite logits checksum")

    prompt_tokens = int(encoded["attention_mask"].sum().item())
    final_prompt_index = prompt_tokens - 1
    next_token = int(logits[0, final_prompt_index].argmax().item())
    parameter_count = sum(parameter.numel() for parameter in model.parameters())
    model_hash = sha256(args.model_directory / "model.safetensors")
    if model_hash != source["weights_sha256"]:
        raise RuntimeError(
            f"local weights SHA-256 {model_hash} does not match "
            f"the source manifest {source['weights_sha256']}"
        )

    print(f"Repository: {source['repository']}")
    print(f"Revision: {source['revision']}")
    print(f"Weights SHA-256: {model_hash}")
    print(
        "Architecture: "
        f"layers={model.config.n_layer} "
        f"hidden={model.config.n_embd} "
        f"heads={model.config.n_head} "
        f"inner={model.config.n_inner or 4 * model.config.n_embd} "
        f"context={model.config.n_positions} "
        f"vocabulary={model.config.vocab_size}"
    )
    print(f"Parameters: {parameter_count}")
    print(f"Prompt tokens: {prompt_tokens}")
    print(f"Input shape: {tuple(encoded['input_ids'].shape)}")
    print(f"Output shape: {tuple(logits.shape)}")
    print(f"Logits checksum: {checksum:.9f}")
    print(f"Next token ID: {next_token}")
    print(f"Next token text: {tokenizer.decode([next_token])!r}")
    print("Official GPT-2 tokenizer, weights, and eager forward pass: PASS")


if __name__ == "__main__":
    main()
