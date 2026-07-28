#!/usr/bin/env python3
import argparse
import warnings
from pathlib import Path

import torch
from torch_mlir import fx

from model import EXPECTED, SAMPLE_INPUT, TwoLinearLayerApplication

warnings.filterwarnings(
    "ignore",
    message=r"`isinstance\(treespec, LeafSpec\)` is deprecated",
    category=FutureWarning,
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    model = TwoLinearLayerApplication().eval()
    eager_result = model(SAMPLE_INPUT)
    if not torch.equal(eager_result, EXPECTED):
        raise RuntimeError(f"unexpected eager result: {eager_result}")

    module = fx.export_and_import(
        model,
        SAMPLE_INPUT,
        output_type="linalg-on-tensors",
        func_name="forward",
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(str(module))

    print(f"PyTorch result: {eager_result.tolist()}")
    print(f"Torch-MLIR Linalg output: {args.output}")


if __name__ == "__main__":
    main()
