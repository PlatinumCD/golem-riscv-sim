#!/usr/bin/env python3
"""Export the pinned Sculptor model-family cases to tensor-level MLIR.

GPT-2 deliberately is not part of this catalogue.  Its dedicated studies have
different scale and scheduling requirements from these functional deployment
proofs.
"""

import argparse
import os
import subprocess
import sys
import warnings
from pathlib import Path

import torch
from torch_mlir import fx


warnings.filterwarnings(
    "ignore",
    message=r"`isinstance\(treespec, LeafSpec\)` is deprecated",
    category=FutureWarning,
)

UPSTREAM_TESTS = (
    Path(__file__).resolve().parents[3]
    / "third_party"
    / "sculptor-mlir"
    / "tests"
    / "python_tests"
)
sys.path.insert(0, str(UPSTREAM_TESTS))

from test_convolution import (  # noqa: E402
    Conv1DModel,
    Conv2DModel,
    Conv3DModel,
    GroupedConv2DModel,
)
from test_gru import GRUCellModel, GRUModel  # noqa: E402
from test_linear import LinearModel, TwoLinearModel  # noqa: E402
from test_lstm import LSTMCellModel, LSTMModel  # noqa: E402
from test_rnn import RNNCellModel, RNNModel  # noqa: E402
from test_transformer import TransformerBlockModel  # noqa: E402


def ones(*shape: int) -> torch.Tensor:
    return torch.ones(*shape)


CASES = {
    "linear_with_bias": (
        lambda: LinearModel(bias=True), lambda: (ones(1, 4),), 1024, 512, False
    ),
    "linear_without_bias": (
        lambda: LinearModel(bias=False), lambda: (ones(1, 4),), 1024, 512, False
    ),
    "two_linear_layers": (
        TwoLinearModel, lambda: (ones(1, 8),), 1024, 512, False
    ),
    "conv1d_with_bias": (
        lambda: Conv1DModel(bias=True), lambda: (ones(1, 1, 6),), 1024, 512, False
    ),
    "conv1d_without_bias": (
        lambda: Conv1DModel(bias=False), lambda: (ones(1, 1, 6),), 1024, 512, False
    ),
    "conv2d_with_bias": (
        lambda: Conv2DModel(bias=True), lambda: (ones(1, 1, 5, 5),), 1024, 512, False
    ),
    "conv2d_without_bias": (
        lambda: Conv2DModel(bias=False), lambda: (ones(1, 1, 5, 5),), 1024, 512, False
    ),
    "grouped_conv2d_with_bias": (
        lambda: GroupedConv2DModel(bias=True), lambda: (ones(1, 4, 5, 5),), 1024, 512, False
    ),
    "grouped_conv2d_without_bias": (
        lambda: GroupedConv2DModel(bias=False), lambda: (ones(1, 4, 5, 5),), 1024, 512, False
    ),
    "conv3d_with_bias": (
        lambda: Conv3DModel(bias=True), lambda: (ones(1, 1, 4, 4, 4),), 1024, 512, False
    ),
    "conv3d_without_bias": (
        lambda: Conv3DModel(bias=False), lambda: (ones(1, 1, 4, 4, 4),), 1024, 512, False
    ),
    "gru_cell_with_bias": (
        lambda: GRUCellModel(bias=True), lambda: (ones(1, 4), ones(1, 3)), 1024, 512, False
    ),
    "gru_cell_without_bias": (
        lambda: GRUCellModel(bias=False), lambda: (ones(1, 4), ones(1, 3)), 1024, 512, False
    ),
    "gru_with_bias": (
        lambda: GRUModel(bias=True), lambda: (ones(1, 3, 4), ones(2, 1, 3)), 1024, 512, False
    ),
    "gru_without_bias": (
        lambda: GRUModel(bias=False), lambda: (ones(1, 3, 4), ones(2, 1, 3)), 1024, 512, False
    ),
    "lstm_cell_with_bias": (
        lambda: LSTMCellModel(bias=True), lambda: (ones(1, 4), ones(1, 3), ones(1, 3)), 1024, 512, False
    ),
    "lstm_cell_without_bias": (
        lambda: LSTMCellModel(bias=False), lambda: (ones(1, 4), ones(1, 3), ones(1, 3)), 1024, 512, False
    ),
    "lstm_with_bias": (
        lambda: LSTMModel(bias=True), lambda: (ones(1, 3, 4), ones(2, 1, 3), ones(2, 1, 3)), 1024, 512, False
    ),
    "lstm_without_bias": (
        lambda: LSTMModel(bias=False), lambda: (ones(1, 3, 4), ones(2, 1, 3), ones(2, 1, 3)), 1024, 512, False
    ),
    "rnn_cell_with_bias": (
        lambda: RNNCellModel(bias=True), lambda: (ones(1, 4), ones(1, 3)), 1024, 512, False
    ),
    "rnn_cell_without_bias": (
        lambda: RNNCellModel(bias=False), lambda: (ones(1, 4), ones(1, 3)), 1024, 512, False
    ),
    "rnn_with_bias": (
        lambda: RNNModel(bias=True), lambda: (ones(1, 3, 4), ones(2, 1, 3)), 1024, 512, False
    ),
    "rnn_without_bias": (
        lambda: RNNModel(bias=False), lambda: (ones(1, 3, 4), ones(2, 1, 3)), 1024, 512, False
    ),
    "transformer_block_with_bias": (
        lambda: TransformerBlockModel(bias=True), lambda: (ones(1, 4, 384),), 1024, 512, True
    ),
    "transformer_block_without_bias": (
        lambda: TransformerBlockModel(bias=False), lambda: (ones(1, 4, 384),), 1024, 512, True
    ),
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", nargs="?", choices=sorted(CASES))
    parser.add_argument("output", nargs="?", type=Path)
    parser.add_argument("--print-hardware", action="store_true")
    parser.add_argument("--list", action="store_true")
    arguments = parser.parse_args()

    if arguments.list:
        print("\n".join(sorted(CASES)))
        return
    if arguments.case is None or arguments.output is None:
        parser.error("CASE and OUTPUT are required unless --list is used")
    factory, input_factory, array_rows, array_cols, external_linalg = CASES[arguments.case]
    if arguments.print_hardware:
        print(f"{array_rows} {array_cols}")
        return

    model = factory().eval()
    inputs = input_factory()
    with torch.no_grad():
        model(*inputs)
    exported = torch.export.export(model, inputs).run_decompositions()
    module = fx.export_and_import(
        exported,
        output_type="torch" if external_linalg else "linalg-on-tensors",
        func_name="forward",
    )
    text = str(module)
    if external_linalg:
        torch_mlir_opt = os.environ.get("TORCH_MLIR_OPT")
        if torch_mlir_opt is None:
            raise RuntimeError("TORCH_MLIR_OPT is required for transformer cases")
        result = subprocess.run(
            [torch_mlir_opt, "-", "--pass-pipeline=builtin.module(torch-backend-to-linalg-on-tensors-backend-pipeline)"],
            input=text, text=True, capture_output=True, check=False,
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        text = result.stdout
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(text)
    print(f"exported {arguments.case}: {len(inputs)} input tensor(s)")


if __name__ == "__main__":
    main()
