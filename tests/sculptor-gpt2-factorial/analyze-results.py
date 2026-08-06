#!/usr/bin/env python3
"""Analyze the complete GPT-2 2^5 compiler factorial experiment."""

from __future__ import annotations

import csv
import itertools
import math
import sys
from pathlib import Path
from statistics import fmean
from typing import Any, Iterable


FACTORS = (
    "boundary_regret",
    "compact_region",
    "spatial_link_pressure",
    "balanced_reductions",
    "distributed_matmul",
)
FACTOR_LABELS = {
    "boundary_regret": "Boundary regret",
    "compact_region": "Compact region",
    "spatial_link_pressure": "Spatial link pressure",
    "balanced_reductions": "Balanced reductions",
    "distributed_matmul": "Distributed matmul",
}
TOKEN_COUNTS = (4, 8, 16, 32)


def number(row: dict[str, str], field: str) -> float:
    value = row.get(field, "")
    if value == "":
        return math.nan
    return float(value)


def mean_runtime(rows: Iterable[dict[str, str]]) -> float:
    values = [number(row, "simulated_time_ns") for row in rows]
    finite = [value for value in values if math.isfinite(value)]
    return fmean(finite) if finite else math.nan


def percent_change(enabled: float, disabled: float) -> float:
    if not math.isfinite(enabled) or not disabled:
        return math.nan
    return 100.0 * (enabled / disabled - 1.0)


def pearson(rows: list[dict[str, str]], x_field: str, y_field: str) -> float:
    pairs = [
        (number(row, x_field), number(row, y_field))
        for row in rows
    ]
    pairs = [
        (x_value, y_value)
        for x_value, y_value in pairs
        if math.isfinite(x_value) and math.isfinite(y_value)
    ]
    if len(pairs) < 2:
        return math.nan
    x_mean = fmean(value[0] for value in pairs)
    y_mean = fmean(value[1] for value in pairs)
    numerator = sum(
        (x_value - x_mean) * (y_value - y_mean)
        for x_value, y_value in pairs
    )
    x_energy = sum((value[0] - x_mean) ** 2 for value in pairs)
    y_energy = sum((value[1] - y_mean) ** 2 for value in pairs)
    denominator = math.sqrt(x_energy * y_energy)
    return numerator / denominator if denominator else math.nan


def fmt(value: float, digits: int = 3) -> str:
    if not math.isfinite(value):
        return "n/a"
    return f"{value:.{digits}f}"


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def complete_token_rows(
    passed: list[dict[str, str]], tokens: int
) -> list[dict[str, str]]:
    rows = [row for row in passed if int(row["tokens"]) == tokens]
    return rows if len(rows) == 32 else []


def main_effects(
    token_rows: dict[int, list[dict[str, str]]]
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for tokens, rows in token_rows.items():
        for factor in FACTORS:
            disabled = mean_runtime(
                row for row in rows if int(row[factor]) == 0
            )
            enabled = mean_runtime(
                row for row in rows if int(row[factor]) == 1
            )
            output.append(
                {
                    "tokens": tokens,
                    "factor": factor,
                    "disabled_mean_ms": disabled / 1.0e6,
                    "enabled_mean_ms": enabled / 1.0e6,
                    "effect_ms": (enabled - disabled) / 1.0e6,
                    "effect_percent": percent_change(enabled, disabled),
                }
            )
    for factor in FACTORS:
        per_token = [
            row
            for row in output
            if row["factor"] == factor and isinstance(row["tokens"], int)
        ]
        output.append(
            {
                "tokens": "all-token-mean",
                "factor": factor,
                "disabled_mean_ms": "",
                "enabled_mean_ms": "",
                "effect_ms": "",
                "effect_percent": fmean(
                    float(row["effect_percent"]) for row in per_token
                ),
            }
        )
    return output


def interaction_rows(
    token_rows: dict[int, list[dict[str, str]]]
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for tokens, rows in token_rows.items():
        grand_mean = mean_runtime(rows)
        for first, second in itertools.combinations(FACTORS, 2):
            cells = {
                (first_value, second_value): mean_runtime(
                    row
                    for row in rows
                    if int(row[first]) == first_value
                    and int(row[second]) == second_value
                )
                for first_value in (0, 1)
                for second_value in (0, 1)
            }
            difference_in_differences = (
                cells[(1, 1)]
                - cells[(1, 0)]
                - cells[(0, 1)]
                + cells[(0, 0)]
            )
            output.append(
                {
                    "tokens": tokens,
                    "factor_a": first,
                    "factor_b": second,
                    "mean_00_ms": cells[(0, 0)] / 1.0e6,
                    "mean_10_ms": cells[(1, 0)] / 1.0e6,
                    "mean_01_ms": cells[(0, 1)] / 1.0e6,
                    "mean_11_ms": cells[(1, 1)] / 1.0e6,
                    "difference_in_differences_ms": (
                        difference_in_differences / 1.0e6
                    ),
                    "factorial_interaction_effect_ms": (
                        difference_in_differences / 2.0e6
                    ),
                    "interaction_percent_of_token_mean": (
                        100.0 * difference_in_differences / grand_mean
                    ),
                }
            )
    for first, second in itertools.combinations(FACTORS, 2):
        per_token = [
            row
            for row in output
            if row["factor_a"] == first and row["factor_b"] == second
        ]
        output.append(
            {
                "tokens": "all-token-mean",
                "factor_a": first,
                "factor_b": second,
                "mean_00_ms": "",
                "mean_10_ms": "",
                "mean_01_ms": "",
                "mean_11_ms": "",
                "difference_in_differences_ms": "",
                "factorial_interaction_effect_ms": "",
                "interaction_percent_of_token_mean": fmean(
                    float(row["interaction_percent_of_token_mean"])
                    for row in per_token
                ),
            }
        )
    return output


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    temporary.replace(path)


def best_and_worst(
    rows: list[dict[str, str]], tokens: int
) -> tuple[dict[str, str], dict[str, str]]:
    token_rows = [row for row in rows if int(row["tokens"]) == tokens]
    ranked = sorted(token_rows, key=lambda row: number(row, "simulated_time_ns"))
    return ranked[0], ranked[-1]


def write_incomplete_report(
    report_path: Path, passed: list[dict[str, str]]
) -> None:
    counts = {
        tokens: sum(int(row["tokens"]) == tokens for row in passed)
        for tokens in TOKEN_COUNTS
    }
    lines = [
        "# GPT-2 compiler-factorial analysis",
        "",
        "The experiment is incomplete. Factorial effects are not reported ",
        "because an unbalanced subset would confound the factors.",
        "",
        "| Tokens | Passed | Required |",
        "|---:|---:|---:|",
    ]
    lines.extend(
        f"| {tokens} | {counts[tokens]} | 32 |" for tokens in TOKEN_COUNTS
    )
    lines.extend(
        (
            "",
            f"Total coverage: **{len(passed)}/128** deployments.",
            "",
            "Transfer cost remains the fixed base score in every trial.",
        )
    )
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_complete_report(
    report_path: Path,
    passed: list[dict[str, str]],
    effects: list[dict[str, Any]],
    interactions: list[dict[str, Any]],
) -> None:
    aggregate_effects = [
        row for row in effects if row["tokens"] == "all-token-mean"
    ]
    aggregate_effects.sort(key=lambda row: float(row["effect_percent"]))
    aggregate_interactions = [
        row
        for row in interactions
        if row["tokens"] == "all-token-mean"
    ]
    aggregate_interactions.sort(
        key=lambda row: abs(float(row["interaction_percent_of_token_mean"])),
        reverse=True,
    )

    lines = [
        "# GPT-2 compiler-factorial analysis",
        "",
        "This is a complete 2^5 factorial study at four token lengths. The ",
        "128 deployments use the same analog hardware and timing model. ",
        "Transfer cost is the fixed base scheduler score, not a variable.",
        "",
        "A negative runtime effect is beneficial. It means that enabling the ",
        "factor reduced simulated time after averaging over every combination ",
        "of the other four factors.",
        "",
        "## Main effects",
        "",
        "| Rank | Compiler factor | Mean runtime change |",
        "|---:|---|---:|",
    ]
    for ordinal, row in enumerate(aggregate_effects, 1):
        lines.append(
            f"| {ordinal} | {FACTOR_LABELS[row['factor']]} | "
            f"{fmt(float(row['effect_percent']), 2)}% |"
        )

    lines.extend(
        (
            "",
            "The percentage is the arithmetic mean of the four token-specific ",
            "effects. This prevents the 32-token trials from dominating the ",
            "aggregate.",
            "",
            "## Best and worst deployments",
            "",
            "| Tokens | Best configuration | Best (ms) | Worst configuration | Worst (ms) |",
            "|---:|---|---:|---|---:|",
        )
    )
    for tokens in TOKEN_COUNTS:
        best, worst = best_and_worst(passed, tokens)
        lines.append(
            f"| {tokens} | `{best['configuration']}` | "
            f"{number(best, 'simulated_time_ns') / 1.0e6:.3f} | "
            f"`{worst['configuration']}` | "
            f"{number(worst, 'simulated_time_ns') / 1.0e6:.3f} |"
        )

    lines.extend(
        (
            "",
            "## Strongest pairwise interactions",
            "",
            "A negative interaction means that the pair works better together ",
            "than the sum of its isolated runtime effects. A positive value ",
            "means that one factor weakens the other.",
            "",
            "| Rank | Factor pair | Difference-in-differences |",
            "|---:|---|---:|",
        )
    )
    for ordinal, row in enumerate(aggregate_interactions[:10], 1):
        lines.append(
            f"| {ordinal} | {FACTOR_LABELS[row['factor_a']]} × "
            f"{FACTOR_LABELS[row['factor_b']]} | "
            f"{fmt(float(row['interaction_percent_of_token_mean']), 2)}% |"
        )

    correlations = (
        ("Retired instructions", "instructions"),
        ("Network words", "network_words"),
        ("Network word-hops", "network_word_hops"),
        ("Compiler-predicted makespan", "predicted_makespan_ns"),
    )
    lines.extend(
        (
            "",
            "## Runtime correlations",
            "",
            "These correlations are descriptive. The factorial effects above ",
            "provide the controlled comparisons.",
            "",
            "| Metric | Pearson r with simulated runtime |",
            "|---|---:|",
        )
    )
    for label, field in correlations:
        lines.append(
            f"| {label} | {fmt(pearson(passed, field, 'simulated_time_ns'))} |"
        )

    lines.extend(
        (
            "",
            "## Interpretation constraints",
            "",
            "- This study measures compiler placement and graph-rewrite effects.",
            "- Distributed matmul uses the current copy-based partition ABI.",
            "- The native memory backend does not model cache or DRAM latency.",
            "- All trials use a 1 GHz dual-issue CPU and a 100 ns analog MVM.",
            "- Main effects are controlled averages, not single-run anecdotes.",
            "",
            "See `main-effects.csv` and `pairwise-interactions.csv` for every ",
            "token-specific value.",
        )
    )
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: analyze-results.py RESULTS_CSV OUTPUT_DIR")
    results_path = Path(sys.argv[1]).resolve()
    output_dir = Path(sys.argv[2]).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    rows = read_rows(results_path)
    passed = [
        row
        for row in rows
        if row.get("status") == "pass"
        and math.isfinite(number(row, "simulated_time_ns"))
    ]
    token_rows = {
        tokens: complete_token_rows(passed, tokens) for tokens in TOKEN_COUNTS
    }
    report_path = output_dir / "analysis.md"
    if any(not token_rows[tokens] for tokens in TOKEN_COUNTS):
        write_incomplete_report(report_path, passed)
        print(f"factorial analysis deferred: {len(passed)}/128 trials passed")
        print(f"factorial coverage report: {report_path}")
        return 0

    effects = main_effects(token_rows)
    interactions = interaction_rows(token_rows)
    write_csv(output_dir / "main-effects.csv", effects)
    write_csv(output_dir / "pairwise-interactions.csv", interactions)
    write_complete_report(report_path, passed, effects, interactions)
    print("factorial analysis: COMPLETE")
    print(f"factorial report: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
