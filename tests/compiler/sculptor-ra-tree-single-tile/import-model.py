#!/usr/bin/env python3
"""Export a deterministic one-layer PyTorch model to Tensor-level MLIR."""

import argparse
from pathlib import Path
import warnings

import torch
from torch_mlir import fx


warnings.filterwarnings(
    "ignore",
    message=r"`isinstance\(treespec, LeafSpec\)` is deprecated",
    category=FutureWarning,
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()

    model = torch.nn.Linear(4, 3, bias=True).eval()
    with torch.no_grad():
        model.weight.fill_(1.0)
        model.bias.fill_(0.5)
    sample = torch.ones(1, 4)
    expected = model(sample)
    if not torch.equal(expected, torch.full((1, 3), 4.5)):
        raise RuntimeError(f"unexpected PyTorch result: {expected}")

    exported = torch.export.export(model, (sample,))
    module = fx.export_and_import(
        exported,
        output_type="linalg-on-tensors",
        func_name="forward",
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(str(module))
    print(f"PyTorch result: {expected.tolist()}")


if __name__ == "__main__":
    main()
