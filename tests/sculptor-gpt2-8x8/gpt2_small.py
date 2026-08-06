#!/usr/bin/env python3
"""Export a GPT-2 small-like decoder-only Transformer fixture to Torch-MLIR.

This is a compiler fixture, not a faithful pretrained GPT-2 implementation.
GPT-2 small uses 12 transformer layers, hidden size 768, and 12 attention
heads. The fixture accepts hidden states directly, uses a causal self-attention
mask, and keeps sequence length small by default so full-shape compiler tests
remain practical.

Use --profile tiny for a fast smoke-test shape.
"""

import argparse
import os
import sys
import warnings
from dataclasses import dataclass

import torch


warnings.filterwarnings(
    "ignore",
    message=r"`isinstance\(treespec, LeafSpec\)` is deprecated.*",
    category=FutureWarning,
)
warnings.filterwarnings(
    "ignore",
    message=r"enable_nested_tensor is True, but self.use_nested_tensor is False.*",
    category=UserWarning,
)

try:
    torch.backends.mha.set_fastpath_enabled(False)
except AttributeError:
    pass


@dataclass(frozen=True)
class GPT2FixtureConfig:
    batch_size: int
    seq_len: int
    hidden_size: int
    num_heads: int
    num_layers: int
    intermediate_size: int


TINY_CONFIG = GPT2FixtureConfig(
    batch_size=1,
    seq_len=3,
    hidden_size=4,
    num_heads=2,
    num_layers=2,
    intermediate_size=16,
)

GPT2_CONFIG = GPT2FixtureConfig(
    batch_size=1,
    seq_len=4,
    hidden_size=768,
    num_heads=12,
    num_layers=12,
    intermediate_size=3072,
)


class GPT2LikeTransformer(torch.nn.Module):
    def __init__(self, config: GPT2FixtureConfig):
        super().__init__()
        self.config = config
        layer = torch.nn.TransformerEncoderLayer(
            d_model=config.hidden_size,
            nhead=config.num_heads,
            dim_feedforward=config.intermediate_size,
            dropout=0.0,
            activation="gelu",
            batch_first=True,
            norm_first=True,
            bias=True,
        )
        self.blocks = torch.nn.TransformerEncoder(
            layer,
            num_layers=config.num_layers,
            norm=torch.nn.LayerNorm(config.hidden_size),
        )
        causal_mask = torch.triu(
            torch.ones(config.seq_len, config.seq_len, dtype=torch.bool),
            diagonal=1,
        )
        self.causal_mask = causal_mask
        self._initialize_deterministically()

    def _initialize_deterministically(self):
        with torch.no_grad():
            for index, (name, parameter) in enumerate(self.named_parameters()):
                if "norm" in name and name.endswith("weight"):
                    parameter.fill_(1.0)
                    continue
                if "norm" in name and name.endswith("bias"):
                    parameter.zero_()
                    continue

                scale = (
                    1000.0 + 10.0 * index
                    if parameter.ndim == 1
                    else 100.0 + 10.0 * index
                )
                parameter.fill_((index + 1) / scale)

    def forward(self, hidden_states):
        return self.blocks(
            hidden_states,
            mask=self.causal_mask,
            is_causal=True,
        )


def make_inputs(config: GPT2FixtureConfig):
    hidden_states = (
        torch.arange(
            1,
            config.batch_size * config.seq_len * config.hidden_size + 1,
            dtype=torch.float32,
        ).reshape(config.batch_size, config.seq_len, config.hidden_size)
        / 100.0
    )
    return (hidden_states,)


def emit_mlir(model, inputs):
    from torch_mlir import fx

    exported = torch.export.export(model, inputs)
    exported = exported.run_decompositions()
    module = fx.export_and_import(
        exported,
        output_type="torch",
        func_name="forward",
    )
    print(module)
    sys.stdout.flush()
    os._exit(0)


def build_config(args):
    config = TINY_CONFIG if args.profile == "tiny" else GPT2_CONFIG
    return GPT2FixtureConfig(
        batch_size=args.batch_size or config.batch_size,
        seq_len=args.seq_len or config.seq_len,
        hidden_size=args.hidden_size or config.hidden_size,
        num_heads=args.num_heads or config.num_heads,
        num_layers=args.num_layers or config.num_layers,
        intermediate_size=args.intermediate_size or config.intermediate_size,
    )


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("run", "mlir"), default="run")
    parser.add_argument(
        "--profile",
        choices=("gpt2-small", "tiny"),
        default="gpt2-small",
    )
    parser.add_argument("--batch-size", type=int)
    parser.add_argument("--seq-len", type=int)
    parser.add_argument("--hidden-size", type=int)
    parser.add_argument("--num-heads", type=int)
    parser.add_argument("--num-layers", type=int)
    parser.add_argument("--intermediate-size", type=int)
    return parser.parse_args()


def main():
    args = parse_args()
    config = build_config(args)

    if config.hidden_size % config.num_heads != 0:
        raise ValueError("hidden_size must be divisible by num_heads")

    torch.manual_seed(0)
    model = GPT2LikeTransformer(config).eval()
    inputs = make_inputs(config)

    if args.mode == "mlir":
        emit_mlir(model, inputs)

    with torch.no_grad():
        output = model(*inputs)
    print(output)


if __name__ == "__main__":
    main()
