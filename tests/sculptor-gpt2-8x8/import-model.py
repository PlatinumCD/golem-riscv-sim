#!/usr/bin/env python3
import argparse
import warnings
from pathlib import Path

import torch
from torch import nn
from torch_mlir import fx
from transformers import GPT2LMHeadModel
from transformers.pytorch_utils import Conv1D


SEQUENCE_LENGTH = 128


class FlattenedLinear(nn.Module):
    """Express a GPT-2 Conv1D projection as a rank-2 nn.Linear."""

    def __init__(self, source):
        super().__init__()
        input_features, output_features = source.weight.shape
        self.input_features = input_features
        self.output_features = output_features
        self.linear = nn.Linear(
            input_features,
            output_features,
            bias=source.bias is not None,
        )
        with torch.no_grad():
            self.linear.weight.copy_(source.weight.transpose(0, 1))
            if source.bias is not None:
                self.linear.bias.copy_(source.bias)

    def forward(self, value):
        shape = value.shape
        flattened = value.reshape(-1, self.input_features)
        projected = self.linear(flattened)
        return projected.reshape(
            shape[0],
            shape[1],
            self.output_features,
        )


class CompilerGPT2(nn.Module):
    """A numerically equivalent GPT-2 wrapper with visible rank-2 projections."""

    def __init__(self, model):
        super().__init__()
        self.transformer = model.transformer
        self.lm_head = model.lm_head
        self.hidden_size = model.config.n_embd
        self.vocabulary_size = model.config.vocab_size

    def forward(self, input_ids):
        hidden = self.transformer(
            input_ids=input_ids,
            use_cache=False,
            return_dict=False,
        )[0]
        logits = self.lm_head(hidden.reshape(-1, self.hidden_size))
        return logits.reshape(
            1,
            SEQUENCE_LENGTH,
            self.vocabulary_size,
        )


def replace_conv1d_projections(module):
    replacement_count = 0
    for name, child in tuple(module.named_children()):
        if isinstance(child, Conv1D):
            setattr(module, name, FlattenedLinear(child))
            replacement_count += 1
        else:
            replacement_count += replace_conv1d_projections(child)
    return replacement_count


def freeze_attention_constants(model):
    """Keep causal masks in the compiled image instead of the entry ABI."""

    for block in model.transformer.h:
        attention = block.attn
        attention.bias = nn.Parameter(
            attention.bias.detach(),
            requires_grad=False,
        )
        attention.masked_bias = nn.Parameter(
            attention.masked_bias.detach(),
            requires_grad=False,
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model_directory", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    warnings.filterwarnings(
        "ignore",
        message=r"`isinstance\(treespec, LeafSpec\)` is deprecated",
        category=FutureWarning,
    )

    model = GPT2LMHeadModel.from_pretrained(
        args.model_directory,
        local_files_only=True,
        torch_dtype=torch.float32,
    ).eval()
    input_ids = (
        torch.arange(SEQUENCE_LENGTH, dtype=torch.int64) * 17 + 3
    ).remainder(model.config.vocab_size).reshape(1, SEQUENCE_LENGTH)

    with torch.inference_mode():
        reference = model(
            input_ids=input_ids,
            use_cache=False,
            return_dict=False,
        )[0]

    replacement_count = replace_conv1d_projections(model.transformer)
    expected_replacement_count = model.config.n_layer * 4
    if replacement_count != expected_replacement_count:
        raise RuntimeError(
            "expected to replace "
            f"{expected_replacement_count} GPT-2 Conv1D projections, "
            f"replaced {replacement_count}"
        )
    freeze_attention_constants(model)
    compiler_model = CompilerGPT2(model).eval()

    with torch.inference_mode():
        adapted = compiler_model(input_ids)
    maximum_error = (adapted - reference).abs().max().item()
    mean_error = (adapted - reference).abs().mean().item()
    if not torch.allclose(
        adapted,
        reference,
        rtol=1.0e-4,
        atol=3.0e-5,
    ):
        raise RuntimeError(
            "compiler adapter changed the GPT-2 result: "
            f"maximum error {maximum_error}"
        )

    module = fx.export_and_import(
        compiler_model,
        input_ids,
        output_type="linalg-on-tensors",
        func_name="forward",
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(str(module))

    print(f"Input shape: {tuple(input_ids.shape)}")
    print(f"Output shape: {tuple(adapted.shape)}")
    print(f"Adapted Conv1D projections: {replacement_count}")
    print(f"Maximum adapter error: {maximum_error:.9g}")
    print(f"Mean adapter error: {mean_error:.9g}")
    print(f"Torch-MLIR Linalg output: {args.output}")


if __name__ == "__main__":
    main()
