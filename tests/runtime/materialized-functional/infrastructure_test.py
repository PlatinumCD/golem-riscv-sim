#!/usr/bin/env python3
"""Fast negative tests for the M4 artifact/log validator."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from case_config import load_cases
from validate_generated_case import validate as validate_generated_case


TEST_DIR = Path(__file__).resolve().parent

EXPECTED_SOURCE_CONTRACTS = {
    "pointwise": (
        "func.func @forward(%arg0: tensor<2500xf32>) -> tensor<2500xf32>",
        'kContract{"pointwise", 1, 1, {10000, 0}, {10000}}',
    ),
    "fork": (
        "func.func @forward(%arg0: tensor<2500xf32>) -> tensor<2500xf32>",
        'kContract{"fork", 1, 1, {10000, 0}, {10000}}',
    ),
    "pool": (
        "func.func @forward(%arg0: tensor<1x2x52x64xf32>) "
        "-> tensor<1x2x26x32xf32>",
        'kContract{"pool", 1, 1, {26624, 0}, {6656}}',
    ),
    "concat": (
        "func.func @forward(%arg0: tensor<2500xf32>, "
        "%arg1: tensor<2500xf32>) -> tensor<5000xf32>",
        'kContract{"concat", 2, 1, {10000, 10000}, {20000}}',
    ),
    "reduction": (
        "func.func @forward(%arg0: tensor<1x4x32x32xf32>) "
        "-> tensor<1x4x31x31xf32>",
        'kContract{"reduction", 1, 1, {16384, 0}, {15376}}',
    ),
    "layout-conversion": (
        "func.func @forward(%input: tensor<129x8xf32>) "
        "-> tensor<129x8xf32>",
        'kContract{"layout-conversion", 1, 1, {4128, 0}, {4128}}',
    ),
}


def normalized_source(path: Path) -> str:
    return " ".join(path.read_text(encoding="utf-8").split())


def validate_source_contracts(cases: dict[str, dict[str, object]]) -> None:
    if set(cases) != set(EXPECTED_SOURCE_CONTRACTS):
        raise AssertionError("fixture and source-contract case sets disagree")
    oracle = normalized_source(TEST_DIR / "case_oracle.cpp")
    for name, case in cases.items():
        signature, contract = EXPECTED_SOURCE_CONTRACTS[name]
        fixture = (TEST_DIR / "fixtures" / f"{name}.mlir").read_text(
            encoding="utf-8"
        )
        normalized_fixture = " ".join(fixture.split())
        if signature not in normalized_fixture or contract not in oracle:
            raise AssertionError(f"{name} fixture and external contract disagree")
        layers = re.findall(
            r'sculptor\.semantic\.layer_kind = "([^"]+)"', fixture
        )
        if tuple(layers) != case["required_semantic_layers"]:
            raise AssertionError(f"{name} semantic layer contract disagrees")
        layer_ids = re.findall(
            r"sculptor\.semantic\.layer_id = (\d+) : i64", fixture
        )
        if len(layer_ids) != len(layers) or len(set(layer_ids)) != len(layer_ids):
            raise AssertionError(f"{name} semantic layer IDs are not unique")


def run_validator(directory: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            str(TEST_DIR / "validate_run.py"),
            "--case",
            "pointwise",
            "--log",
            str(directory / "simulation.log"),
            "--statistics",
            str(directory / "statistics.csv"),
            "--active-cores",
            str(directory / "active-cores.txt"),
        ],
        text=True,
        capture_output=True,
        check=False,
    )


def write_valid(directory: Path) -> None:
    directory.joinpath("active-cores.txt").write_text("0\n2\n", encoding="utf-8")
    lines = []
    for tile in (0, 2):
        validation_requests = 0
        validation_bytes = 0
        physical_requests = 3 + validation_requests
        physical_bytes = 8224 + validation_bytes
        lines.extend(
            [
                "MATERIALIZED_FUNCTIONAL_ACCOUNT case=pointwise "
                f"tile={tile} iterations=2 full_requests=2 tail_requests=1 "
                "runtime_requests=3 runtime_bytes=8224 "
                f"validation_requests={validation_requests} "
                f"validation_bytes={validation_bytes}",
                f"MATERIALIZED_FUNCTIONAL_PASS case=pointwise tile={tile}",
                f"MITTENS_SCRATCHPAD_PROFILE tile={tile} enabled=1 "
                f"capacity_bytes=2097152 cpu_requests=0 "
                f"dma_transfers={physical_requests} dma_bytes={physical_bytes}",
            ]
        )
    directory.joinpath("simulation.log").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )
    directory.joinpath("statistics.csv").write_text(
        "ComponentName,StatisticName,StatisticSubId,StatisticType,SimTime,"
        "Rank,Sum.u64,SumSQ.u64,Count.u64,Min.u64,Max.u64\n"
        "global_ram,requests,,Accumulator,1,0,6,36,6,1,1\n"
        "global_ram,bytes,,Accumulator,1,0,16448,1,6,32,4096\n",
        encoding="utf-8",
    )


def write_valid_generated_case(directory: Path) -> dict[str, Path]:
    formed = directory / "formed.mlir"
    formed.write_text(
        """module attributes {
  sculptor.materialization.audit = {
    cross_epoch_direct_route_count = 0 : i64,
    materialized_main_transfer_count_logical = 1 : i64,
    materialized_tail_transfer_count_logical = 1 : i64
  },
  sculptor.materialization.boundaries = [{boundary_id = 7 : i64}],
  sculptor.materialization.epoch_count = 3 : i64,
  sculptor.sharding.layout_audit = {
    explicit_layout_conversion_count = 0 : i64,
    failure_class_counts = {}
  }
} {
  func.func @forward() attributes {
    sculptor.materialization.operation_epochs = [
      {epoch_id = 1 : i64, executable = true},
      {epoch_id = 2 : i64, executable = true}
    ]
  } {
    \"test.layer\"() {sculptor.semantic.layer_kind = \"pointwise_add\"} : () -> ()
    \"test.layer\"() {sculptor.semantic.layer_kind = \"pointwise_multiply\"} : () -> ()
    return
  }
}
""",
        encoding="utf-8",
    )
    active = directory / "active-cores.txt"
    active.write_text("0\n", encoding="utf-8")
    deployment = directory / "deployment-manifest.json"
    deployment.write_text(
        json.dumps(
            {
                "schema": "sculptor.deployment",
                "version": 2,
                "active_tile_ids": [0],
                "synchronization": {
                    "mode": "bulk_barrier",
                    "semantic_epoch_count": 3,
                },
            }
        ),
        encoding="utf-8",
    )
    extracted = directory / "cores"
    extracted.mkdir()
    extracted.joinpath("core-0-extracted.mlir").write_text(
        "module {}\n", encoding="utf-8"
    )
    return {
        "formed": formed,
        "active_manifest": active,
        "deployment_manifest": deployment,
        "extracted_directory": extracted,
    }


def main() -> None:
    runner = (TEST_DIR / "run-case.sh").read_text(encoding="utf-8")
    assert "timeout --foreground --signal=TERM --kill-after=2s 20s" in runner
    assert "timeout --foreground --signal=TERM --kill-after=5s 120s" in runner
    assert runner.count("</dev/null") >= 2
    assert 'pipeline_status=("${PIPESTATUS[@]}")' in runner
    suite_runner = (TEST_DIR / "run-test.sh").read_text(encoding="utf-8")
    assert "timeout --foreground --signal=TERM --kill-after=10s 300s" in suite_runner
    assert "parametric-noc/run-test.sh\" </dev/null" in suite_runner
    assert "functional-abi-main.o" in runner
    assert "MATERIALIZED_FUNCTIONAL_PREFLIGHT" in runner
    preflight = (TEST_DIR / "preflight.cpp").read_text(encoding="utf-8")
    assert "accounting.full_requests == 0" in preflight
    assert "accounting.tail_requests == 0" in preflight
    simulation = (TEST_DIR / "simulation.py").read_text(encoding="utf-8")
    assert '"memory_init_batching": True' in simulation
    assert "initialization_barrier={" in simulation
    assert '"release_cycles": 1' in simulation

    cases = load_cases(TEST_DIR / "cases.json")
    assert list(cases) == [
        "pointwise",
        "fork",
        "pool",
        "concat",
        "reduction",
        "layout-conversion",
    ]
    validate_source_contracts(cases)
    malformed = json.loads((TEST_DIR / "cases.json").read_text(encoding="utf-8"))
    malformed["cases"][0]["id"] = True
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        bad_matrix = directory / "bad-cases.json"
        bad_matrix.write_text(json.dumps(malformed), encoding="utf-8")
        try:
            load_cases(bad_matrix)
        except ValueError:
            pass
        else:
            raise AssertionError("boolean case ID was accepted")

        missing_structure = json.loads(
            (TEST_DIR / "cases.json").read_text(encoding="utf-8")
        )
        del missing_structure["cases"][0]["minimum_epoch_count"]
        bad_matrix.write_text(json.dumps(missing_structure), encoding="utf-8")
        try:
            load_cases(bad_matrix)
        except ValueError:
            pass
        else:
            raise AssertionError("case without structural requirements was accepted")

        generated = write_valid_generated_case(directory)

        def expect_structure_rejected(label: str, **overrides: Path) -> None:
            arguments = dict(generated)
            arguments.update(overrides)
            try:
                validate_generated_case(
                    case_name="pointwise",
                    matrix=TEST_DIR / "cases.json",
                    **arguments,
                )
            except ValueError:
                return
            raise AssertionError(f"invalid compiler structure was accepted: {label}")

        structure = validate_generated_case(
            case_name="pointwise",
            matrix=TEST_DIR / "cases.json",
            **generated,
        )
        assert structure["epoch_count"] == 3
        original_formed = generated["formed"].read_text(encoding="utf-8")
        generated["formed"].write_text(
            original_formed.replace(
                "materialized_main_transfer_count_logical = 1",
                "materialized_main_transfer_count_logical = 0",
            ),
            encoding="utf-8",
        )
        expect_structure_rejected("zero full-transfer count")
        generated["formed"].write_text(
            original_formed.replace(
                "materialized_tail_transfer_count_logical = 1",
                "materialized_tail_transfer_count_logical = 0",
            ),
            encoding="utf-8",
        )
        expect_structure_rejected("zero tail-transfer count")
        generated["formed"].write_text(
            original_formed.replace(
                "sculptor.materialization.epoch_count = 3",
                "sculptor.materialization.epoch_count = 2",
            ),
            encoding="utf-8",
        )
        expect_structure_rejected("shallow materialization schedule")
        generated["formed"].write_text(
            original_formed.replace("executable = true", "executable = false", 1),
            encoding="utf-8",
        )
        expect_structure_rejected("erased executable operation")
        generated["formed"].write_text(
            original_formed.replace(
                "sculptor.materialization.boundaries = "
                "[{boundary_id = 7 : i64}]",
                "sculptor.materialization.boundaries = []",
            ),
            encoding="utf-8",
        )
        expect_structure_rejected("erased materialized boundary")
        generated["formed"].write_text(
            original_formed.replace("pointwise_multiply", "erased_multiply"),
            encoding="utf-8",
        )
        expect_structure_rejected("erased semantic layer")
        generated["formed"].write_text(original_formed, encoding="utf-8")

        layout_matrix = json.loads(
            (TEST_DIR / "cases.json").read_text(encoding="utf-8")
        )
        layout_matrix["cases"][0][
            "minimum_explicit_layout_conversion_count"
        ] = 1
        required_layout_matrix = directory / "required-layout-cases.json"
        required_layout_matrix.write_text(
            json.dumps(layout_matrix), encoding="utf-8"
        )
        try:
            validate_generated_case(
                case_name="pointwise",
                matrix=required_layout_matrix,
                **generated,
            )
        except ValueError:
            pass
        else:
            raise AssertionError("missing explicit layout conversion was accepted")

        original_deployment = generated["deployment_manifest"].read_text(
            encoding="utf-8"
        )
        deployment_payload = json.loads(original_deployment)
        deployment_payload["synchronization"][
            "semantic_epoch_count"
        ] = 4
        generated["deployment_manifest"].write_text(
            json.dumps(deployment_payload), encoding="utf-8"
        )
        expect_structure_rejected("deployment epoch mismatch")
        generated["deployment_manifest"].write_text(
            original_deployment, encoding="utf-8"
        )

        generated["extracted_directory"].joinpath(
            "core-0-extracted.mlir"
        ).write_text("#sculptor.tile_routine_route<bad>\n", encoding="utf-8")
        expect_structure_rejected("outlined direct route")

        write_valid(directory)
        valid = run_validator(directory)
        assert valid.returncode == 0, valid.stderr

        log = directory / "simulation.log"
        original = log.read_text(encoding="utf-8")
        log.write_text(
            original.replace(
                "MITTENS_SCRATCHPAD_PROFILE tile=2",
                "MITTENS_SCRATCHPAD_PROFILE_MISSING tile=2",
            ),
            encoding="utf-8",
        )
        incomplete = run_validator(directory)
        assert incomplete.returncode != 0

        write_valid(directory)
        statistics = directory / "statistics.csv"
        statistics.write_text(
            statistics.read_text(encoding="utf-8").replace(
                "global_ram,bytes,,Accumulator,1,0,16448,",
                "global_ram,bytes,,Accumulator,1,0,16447,",
            ),
            encoding="utf-8",
        )
        mismatch = run_validator(directory)
        assert mismatch.returncode != 0

        write_valid(directory)
        log.write_text(
            log.read_text(encoding="utf-8").replace(
                "MATERIALIZED_FUNCTIONAL_PASS case=pointwise tile=2",
                "MATERIALIZED_FUNCTIONAL_ERROR case=pointwise tile=2 "
                "stage=runtime-accounting",
            ),
            encoding="utf-8",
        )
        output_mismatch = run_validator(directory)
        assert output_mismatch.returncode != 0

    print("materialized functional infrastructure: PASS")


if __name__ == "__main__":
    main()
