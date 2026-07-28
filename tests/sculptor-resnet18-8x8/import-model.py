#!/usr/bin/env python3
import argparse
import warnings
from pathlib import Path

from torch_mlir import fx

from model import ResNet18Application, SAMPLE_INPUT


warnings.filterwarnings(
    "ignore",
    message=r"`isinstance\(treespec, LeafSpec\)` is deprecated",
    category=FutureWarning,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    model = ResNet18Application().eval()
    eager_result = model(SAMPLE_INPUT)
    module = fx.export_and_import(
        model,
        SAMPLE_INPUT,
        output_type="linalg-on-tensors",
        func_name="forward",
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(str(module))
    print(f"PyTorch output shape: {tuple(eager_result.shape)}")
    print(f"PyTorch output sum: {eager_result.sum().item():.9f}")
    print(f"PyTorch top-1 index: {eager_result.argmax(dim=1).item()}")
    print(f"Torch-MLIR Linalg output: {args.output}")


if __name__ == "__main__":
    main()
