#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path

from huggingface_hub import HfApi, snapshot_download


REQUIRED_FILES = (
    "config.json",
    "generation_config.json",
    "merges.txt",
    "model.safetensors",
    "tokenizer.json",
    "tokenizer_config.json",
    "vocab.json",
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--weights-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    info = HfApi().model_info(
        repo_id=args.repository,
        revision=args.revision,
    )
    if info.sha != args.revision:
        raise RuntimeError(
            f"resolved model revision {info.sha} does not match "
            f"the pinned revision {args.revision}"
        )

    args.output.mkdir(parents=True, exist_ok=True)
    snapshot_download(
        repo_id=args.repository,
        revision=args.revision,
        local_dir=args.output,
        allow_patterns=list(REQUIRED_FILES),
    )

    missing = [
        name for name in REQUIRED_FILES if not (args.output / name).is_file()
    ]
    if missing:
        raise RuntimeError(f"downloaded model is missing files: {missing}")

    actual_weights_sha256 = sha256(args.output / "model.safetensors")
    if actual_weights_sha256 != args.weights_sha256:
        raise RuntimeError(
            f"downloaded weights SHA-256 {actual_weights_sha256} does not "
            f"match the pinned digest {args.weights_sha256}"
        )

    source = {
        "repository": args.repository,
        "revision": info.sha,
        "weights_sha256": actual_weights_sha256,
        "files": list(REQUIRED_FILES),
    }
    (args.output / "source.json").write_text(
        json.dumps(source, indent=2) + "\n"
    )
    print(f"Downloaded pinned GPT-2 checkpoint: {args.repository}@{info.sha}")
    print(f"Weights SHA-256: {actual_weights_sha256}")
    print(f"Local model directory: {args.output}")


if __name__ == "__main__":
    main()
