#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[3]
VALIDATOR = PROJECT_ROOT / "scripts" / "validate-sculptor-materialization-audit.py"

FIELDS = {
    "schema_version": 1,
    "epoch_count": 3,
    "materialized_tensor_count": 4,
    "materialized_tensor_bytes": 16384,
    "materialized_producer_region_count": 3,
    "materialized_consumer_region_count": 5,
    "zero_contribution_consumer_region_count": 0,
    "materialized_output_dma_descriptor_count": 0,
    "materialized_input_dma_descriptor_count": 0,
    "materialized_main_transfer_count_logical": 12,
    "materialized_tail_transfer_count_logical": 2,
    "unowned_materialized_byte_count": 0,
    "multiply_owned_materialized_byte_count": 0,
    "read_before_produced_region_count": 0,
    "cross_epoch_direct_route_count": 0,
    "unclassified_boundary_count": 0,
    "maximum_live_global_ram_bytes": 8192,
}


def finalized_module(input_count: int, output_count: int, **changes: int) -> str:
    fields = {**FIELDS, **changes}
    fields["materialized_input_dma_descriptor_count"] = input_count
    fields["materialized_output_dma_descriptor_count"] = output_count
    audit = ", ".join(f"{name} = {value} : i64" for name, value in fields.items())
    directions = [0] * input_count + [1] * output_count
    descriptors = []
    segments = []
    for descriptor_id, direction in enumerate(directions):
        descriptors.append(
            "#sculptor.materialized_dma_descriptor<"
            f"id = {descriptor_id} : i64, operationId = {descriptor_id} : i64, "
            "epochId = 1 : i64, workUnitId = 0 : i64, tensorId = 0 : i64, "
            f"portNumber = {descriptor_id} : i64, loopId = 0 : i64, "
            f"direction = {direction} : i64, templateKind = 0 : i64, "
            "flags = 0 : i64, scratchpadRingBase = 0 : i64, "
            "scratchpadSlotStride = 4096 : i64, ringSlots = 1 : i64, "
            "iterationBegin = 0 : i64, iterationEnd = 1 : i64, "
            "iterationStep = 1 : i64, "
            f"segmentOffset = {descriptor_id} : i64, segmentCount = 1 : i64, "
            "bytesPerIteration = 4096 : i64>"
        )
        segments.append(
            "#sculptor.materialized_dma_segment<"
            f"id = {descriptor_id} : i64, descriptorId = {descriptor_id} : i64, "
            "relationId = 0 : i64, relationPieceOrdinal = 0 : i64, "
            f"globalResourceId = {descriptor_id} : i64, "
            "byteSize = 4096 : i64, repeatCount = 1 : i64, "
            "pieceCount = 1 : i64, globalByteOffset = 0 : i64, "
            "globalIterationStride = 0 : i64, scratchpadByteOffset = 0 : i64, "
            "globalRepeatStride = 0 : i64, scratchpadRepeatStride = 0 : i64, "
            "globalPieceStride = 0 : i64, scratchpadPieceStride = 0 : i64, "
            "scratchpadIterationStride = 0 : i64>"
        )
    return (
        "module attributes {"
        f"sculptor.materialization.audit = {{{audit}}}, "
        "sculptor.materialization.dma_descriptors = ["
        + ", ".join(descriptors)
        + "], sculptor.materialization.dma_segments = ["
        + ", ".join(segments)
        + "]} {}\n"
    )


def compact_pair_module() -> str:
    fields = {**FIELDS, "materialized_input_dma_descriptor_count": 2}
    audit = ", ".join(f"{name} = {value} : i64" for name, value in fields.items())
    descriptors = []
    segments = []
    for descriptor_id, flag in enumerate((16, 32)):
        descriptors.append(
            "#sculptor.materialized_dma_descriptor<"
            f"id = {descriptor_id} : i64, operationId = 7 : i64, "
            "epochId = 1 : i64, workUnitId = 3 : i64, tensorId = 5 : i64, "
            f"portNumber = {descriptor_id} : i64, loopId = 2 : i64, "
            "direction = 0 : i64, templateKind = 0 : i64, "
            f"flags = {flag} : i64, scratchpadRingBase = 0 : i64, "
            "scratchpadSlotStride = 4096 : i64, ringSlots = 1 : i64, "
            "iterationBegin = 0 : i64, iterationEnd = 1 : i64, "
            "iterationStep = 1 : i64, "
            f"segmentOffset = {descriptor_id} : i64, segmentCount = 1 : i64, "
            "bytesPerIteration = 4 : i64>"
        )
        segments.append(
            "#sculptor.materialized_dma_segment<"
            f"id = {descriptor_id} : i64, descriptorId = {descriptor_id} : i64, "
            "relationId = 11 : i64, relationPieceOrdinal = 0 : i64, "
            "globalResourceId = 13 : i64, byteSize = 4 : i64, "
            "repeatCount = 1 : i64, pieceCount = 1 : i64, "
            f"globalByteOffset = {descriptor_id * 4} : i64, "
            "globalIterationStride = 0 : i64, "
            f"scratchpadByteOffset = {descriptor_id * 4} : i64, "
            "globalRepeatStride = 0 : i64, scratchpadRepeatStride = 0 : i64, "
            "globalPieceStride = 0 : i64, scratchpadPieceStride = 0 : i64, "
            "scratchpadIterationStride = 0 : i64>"
        )
    return (
        "module attributes {"
        f"sculptor.materialization.audit = {{{audit}}}, "
        "sculptor.materialization.dma_descriptors = ["
        + ", ".join(descriptors)
        + "], sculptor.materialization.dma_segments = ["
        + ", ".join(segments)
        + "]} {}\n"
    )


def compact_run_module(byte_sizes: tuple[int, ...]) -> str:
    if not byte_sizes or len(byte_sizes) % 2 != 0:
        raise ValueError("compact run fixture requires a non-empty even size list")
    fields = {
        **FIELDS,
        "materialized_input_dma_descriptor_count": len(byte_sizes),
    }
    audit = ", ".join(f"{name} = {value} : i64" for name, value in fields.items())
    descriptors = []
    segments = []
    byte_offset = 0
    for descriptor_id, byte_size in enumerate(byte_sizes):
        flag = 16 if descriptor_id % 2 == 0 else 32
        descriptors.append(
            "#sculptor.materialized_dma_descriptor<"
            f"id = {descriptor_id} : i64, operationId = 7 : i64, "
            "epochId = 1 : i64, workUnitId = 3 : i64, tensorId = 5 : i64, "
            f"portNumber = {descriptor_id} : i64, loopId = 2 : i64, "
            "direction = 0 : i64, templateKind = 0 : i64, "
            f"flags = {flag} : i64, scratchpadRingBase = 0 : i64, "
            "scratchpadSlotStride = 4096 : i64, ringSlots = 1 : i64, "
            "iterationBegin = 0 : i64, iterationEnd = 1 : i64, "
            "iterationStep = 1 : i64, "
            f"segmentOffset = {descriptor_id} : i64, segmentCount = 1 : i64, "
            f"bytesPerIteration = {byte_size} : i64>"
        )
        segments.append(
            "#sculptor.materialized_dma_segment<"
            f"id = {descriptor_id} : i64, descriptorId = {descriptor_id} : i64, "
            "relationId = 11 : i64, relationPieceOrdinal = 0 : i64, "
            "globalResourceId = 13 : i64, "
            f"byteSize = {byte_size} : i64, repeatCount = 1 : i64, "
            "pieceCount = 1 : i64, "
            f"globalByteOffset = {byte_offset} : i64, "
            "globalIterationStride = 0 : i64, "
            f"scratchpadByteOffset = {byte_offset} : i64, "
            "globalRepeatStride = 0 : i64, scratchpadRepeatStride = 0 : i64, "
            "globalPieceStride = 0 : i64, scratchpadPieceStride = 0 : i64, "
            "scratchpadIterationStride = 0 : i64>"
        )
        byte_offset += byte_size
    return (
        "module attributes {"
        f"sculptor.materialization.audit = {{{audit}}}, "
        "sculptor.materialization.dma_descriptors = ["
        + ", ".join(descriptors)
        + "], sculptor.materialization.dma_segments = ["
        + ", ".join(segments)
        + "]} {}\n"
    )


def affine_segment_run_module() -> str:
    fields = {
        **FIELDS,
        "materialized_input_dma_descriptor_count": 1,
        "materialized_output_dma_descriptor_count": 1,
    }
    audit = ", ".join(f"{name} = {value} : i64" for name, value in fields.items())
    descriptors = [
        "#sculptor.materialized_dma_descriptor<"
        "id = 0 : i64, operationId = 7 : i64, epochId = 1 : i64, "
        "workUnitId = 3 : i64, tensorId = 5 : i64, portNumber = 0 : i64, "
        "loopId = 2 : i64, direction = 0 : i64, templateKind = 0 : i64, "
        "flags = 0 : i64, scratchpadRingBase = 0 : i64, "
        "scratchpadSlotStride = 8192 : i64, ringSlots = 1 : i64, "
        "iterationBegin = 0 : i64, iterationEnd = 1 : i64, "
        "iterationStep = 1 : i64, segmentOffset = 0 : i64, "
        "segmentCount = 1 : i64, bytesPerIteration = 4560 : i64>",
        "#sculptor.materialized_dma_descriptor<"
        "id = 1 : i64, operationId = 8 : i64, epochId = 1 : i64, "
        "workUnitId = 4 : i64, tensorId = 6 : i64, portNumber = 0 : i64, "
        "loopId = 3 : i64, direction = 1 : i64, templateKind = 0 : i64, "
        "flags = 0 : i64, scratchpadRingBase = 0 : i64, "
        "scratchpadSlotStride = 8192 : i64, ringSlots = 1 : i64, "
        "iterationBegin = 0 : i64, iterationEnd = 1 : i64, "
        "iterationStep = 1 : i64, segmentOffset = 1 : i64, "
        "segmentCount = 1 : i64, bytesPerIteration = 3072 : i64>",
    ]
    segments = [
        "#sculptor.materialized_dma_segment<"
        "id = 0 : i64, descriptorId = 0 : i64, relationId = 11 : i64, "
        "relationPieceOrdinal = 0 : i64, globalResourceId = 13 : i64, "
        "byteSize = 228 : i64, repeatCount = 1 : i64, pieceCount = 20 : i64, "
        "globalByteOffset = 0 : i64, globalIterationStride = 0 : i64, "
        "scratchpadByteOffset = 0 : i64, globalRepeatStride = 0 : i64, "
        "scratchpadRepeatStride = 0 : i64, globalPieceStride = 228 : i64, "
        "scratchpadPieceStride = 228 : i64, scratchpadIterationStride = 0 : i64>",
        "#sculptor.materialized_dma_segment<"
        "id = 1 : i64, descriptorId = 1 : i64, relationId = 12 : i64, "
        "relationPieceOrdinal = 0 : i64, globalResourceId = 14 : i64, "
        "byteSize = 512 : i64, repeatCount = 2 : i64, pieceCount = 3 : i64, "
        "globalByteOffset = 0 : i64, globalIterationStride = 0 : i64, "
        "scratchpadByteOffset = 0 : i64, globalRepeatStride = 1536 : i64, "
        "scratchpadRepeatStride = 1536 : i64, globalPieceStride = 512 : i64, "
        "scratchpadPieceStride = 512 : i64, scratchpadIterationStride = 0 : i64>",
    ]
    return (
        "module attributes {"
        f"sculptor.materialization.audit = {{{audit}}}, "
        "sculptor.materialization.dma_descriptors = ["
        + ", ".join(descriptors)
        + "], sculptor.materialization.dma_segments = ["
        + ", ".join(segments)
        + "]} {}\n"
    )


class MaterializationAuditValidatorTest(unittest.TestCase):
    def make_fixture(self, root: Path) -> tuple[Path, Path, Path, Path]:
        cores = root / "cores"
        cores.mkdir()
        active = root / "active-cores.txt"
        active.write_text("0\n1\n", encoding="utf-8")
        deployment = root / "deployment-manifest.json"
        deployment.write_text(
            json.dumps(
                {
                    "schema": "sculptor.deployment",
                    "version": 2,
                    "active_tile_ids": [0, 1],
                    "synchronization": {
                        "mode": "bulk_barrier",
                        "semantic_epoch_count": 3,
                    },
                }
            ),
            encoding="utf-8",
        )
        (cores / "core-0-finalized.mlir").write_text(
            finalized_module(1, 2), encoding="utf-8"
        )
        (cores / "core-1-finalized.mlir").write_text(
            finalized_module(2, 1), encoding="utf-8"
        )
        return cores, active, deployment, root / "audit.json"

    def run_validator(
        self, cores: Path, active: Path, deployment: Path, output: Path
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(VALIDATOR),
                "--core-directory",
                str(cores),
                "--active-core-manifest",
                str(active),
                "--deployment-manifest",
                str(deployment),
                "--network-size",
                "4",
                "--global-ram-bytes",
                "32768",
                "--output",
                str(output),
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_pass_aggregates_local_descriptors_and_hashes_sources(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cores, active, deployment, output = self.make_fixture(Path(temporary))
            result = self.run_validator(cores, active, deployment, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            certificate = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(certificate["status"], "PASS")
            self.assertEqual(certificate["active_tile_ids"], [0, 1])
            self.assertEqual(
                certificate["counters"]["zero_contribution_consumer_region_count"],
                0,
            )
            self.assertEqual(
                certificate["counters"]["materialized_input_dma_descriptor_count"],
                3,
            )
            self.assertEqual(
                certificate["counters"]["materialized_output_dma_descriptor_count"],
                3,
            )
            self.assertEqual(len(certificate["tiles"]), 2)
            self.assertTrue(all(len(tile["sha256"]) == 64 for tile in certificate["tiles"]))
            self.assertEqual(
                certificate["counters"]["remaining_input_logical_bytes"],
                3 * 4096,
            )
            self.assertEqual(
                certificate["counters"]["remaining_output_logical_transfer_count"],
                3,
            )
            self.assertEqual(
                certificate["counters"]["maximum_physical_segment_bytes"],
                4096,
            )
            self.assertEqual(
                certificate["counters"]["phase4_accounting_tile_count"], 0
            )
            self.assertEqual(
                certificate["physical_request_size_histograms"],
                {"input": {"4096": 3}, "output": {"4096": 3}},
            )
            self.assertEqual(
                certificate["tiles"][0]["physical_request_size_histograms"],
                {"input": {"4096": 1}, "output": {"4096": 2}},
            )

    def test_compact_pair_counts_two_descriptors_as_one_combined_request(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cores, active, deployment, output = self.make_fixture(Path(temporary))
            for core in (0, 1):
                (cores / f"core-{core}-finalized.mlir").write_text(
                    compact_pair_module(), encoding="utf-8"
                )
            result = self.run_validator(cores, active, deployment, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            certificate = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(
                certificate["physical_request_size_histograms"],
                {"input": {"8": 2}, "output": {}},
            )
            self.assertEqual(
                certificate["counters"]["materialized_input_dma_descriptor_count"],
                4,
            )
            self.assertEqual(
                certificate["counters"]["remaining_input_physical_request_count"],
                2,
            )
            self.assertEqual(
                certificate["counters"]["remaining_input_physical_byte_count"],
                16,
            )
            self.assertTrue(
                all(
                    tile["physical_request_size_histograms"]["input"] == {"8": 1}
                    for tile in certificate["tiles"]
                )
            )

    def test_maximal_run_audit_predicts_four_way_pack_and_honors_4k_limit(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cores, active, deployment, output = self.make_fixture(Path(temporary))
            for core in (0, 1):
                (cores / f"core-{core}-finalized.mlir").write_text(
                    compact_run_module((1000, 1000, 1000, 1000)),
                    encoding="utf-8",
                )
            result = self.run_validator(cores, active, deployment, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            audit = json.loads(output.read_text(encoding="utf-8"))[
                "maximal_input_dma_run_audit"
            ]
            self.assertEqual(audit["static_run_count"], 2)
            self.assertEqual(audit["static_descriptor_count"], 8)
            self.assertEqual(audit["maximum_run_descriptor_count"], 4)
            self.assertEqual(audit["maximum_run_bytes"], 4000)
            self.assertEqual(audit["dynamic_packed_request_count"], 2)
            self.assertEqual(audit["predicted_physical_request_count"], 2)
            self.assertEqual(audit["predicted_additional_request_reduction"], 2)
            self.assertEqual(audit["run_length_histogram"], {"4": 2})
            self.assertEqual(
                audit["candidate_request_size_histogram"], {"4000": 2}
            )

            for core in (0, 1):
                (cores / f"core-{core}-finalized.mlir").write_text(
                    compact_run_module((1024, 1024, 1024, 1025)),
                    encoding="utf-8",
                )
            second_output = Path(temporary) / "boundary-audit.json"
            result = self.run_validator(cores, active, deployment, second_output)
            self.assertEqual(result.returncode, 0, result.stderr)
            boundary = json.loads(second_output.read_text(encoding="utf-8"))[
                "maximal_input_dma_run_audit"
            ]
            self.assertEqual(boundary["maximum_run_descriptor_count"], 3)
            self.assertEqual(boundary["maximum_run_bytes"], 3072)
            self.assertEqual(boundary["predicted_additional_request_reduction"], 0)
            self.assertEqual(
                boundary["candidate_request_size_histogram"],
                {"1025": 2, "3072": 2},
            )

    def test_affine_segment_run_audit_packs_piece_and_repeat_axes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            cores, active, deployment, output = self.make_fixture(Path(temporary))
            for core in (0, 1):
                (cores / f"core-{core}-finalized.mlir").write_text(
                    affine_segment_run_module(), encoding="utf-8"
                )
            result = self.run_validator(cores, active, deployment, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            audit = json.loads(output.read_text(encoding="utf-8"))[
                "affine_segment_run_audit"
            ]
            self.assertEqual(audit["input"]["static_packable_segment_count"], 2)
            self.assertEqual(
                audit["input"]["static_piece_linear_segment_count"], 2
            )
            self.assertEqual(audit["input"]["dynamic_logical_request_count"], 40)
            self.assertEqual(audit["input"]["dynamic_packed_request_count"], 4)
            self.assertEqual(audit["input"]["predicted_physical_request_count"], 4)
            self.assertEqual(
                audit["input"]["predicted_additional_request_reduction"], 36
            )
            self.assertEqual(
                audit["input"]["candidate_request_size_histogram"],
                {"684": 2, "3876": 2},
            )
            self.assertEqual(audit["output"]["static_packable_segment_count"], 2)
            self.assertEqual(
                audit["output"]["static_full_linear_segment_count"], 2
            )
            self.assertEqual(audit["output"]["dynamic_logical_request_count"], 12)
            self.assertEqual(audit["output"]["dynamic_packed_request_count"], 2)
            self.assertEqual(audit["output"]["predicted_physical_request_count"], 2)
            self.assertEqual(
                audit["output"]["predicted_additional_request_reduction"], 10
            )
            self.assertEqual(
                audit["output"]["candidate_request_size_histogram"], {"3072": 2}
            )

    def test_failures_still_write_fail_closed_certificate(self) -> None:
        mutations = {
            "nonzero correctness counter": lambda cores, active, deployment: (
                cores / "core-1-finalized.mlir"
            ).write_text(
                finalized_module(2, 1, cross_epoch_direct_route_count=1),
                encoding="utf-8",
            ),
            "missing active core": lambda cores, active, deployment: (
                cores / "core-1-finalized.mlir"
            ).unlink(),
            "duplicate active core": lambda cores, active, deployment: active.write_text(
                "0\n1\n1\n", encoding="utf-8"
            ),
            "epoch mismatch": lambda cores, active, deployment: deployment.write_text(
                json.dumps(
                    {
                        "schema": "sculptor.deployment",
                        "version": 2,
                        "active_tile_ids": [0, 1],
                        "synchronization": {
                            "mode": "bulk_barrier",
                            "semantic_epoch_count": 2,
                        },
                    }
                ),
                encoding="utf-8",
            ),
            "descriptor count mismatch": lambda cores, active, deployment: (
                cores / "core-1-finalized.mlir"
            ).write_text(
                finalized_module(2, 1).replace(
                    "direction = 0 : i64", "direction = 1 : i64", 1
                ),
                encoding="utf-8",
            ),
            "global RAM overflow": lambda cores, active, deployment: (
                (cores / "core-0-finalized.mlir").write_text(
                    finalized_module(1, 2, maximum_live_global_ram_bytes=65536),
                    encoding="utf-8",
                ),
                (cores / "core-1-finalized.mlir").write_text(
                    finalized_module(2, 1, maximum_live_global_ram_bytes=65536),
                    encoding="utf-8",
                ),
            ),
            "physical segment wider than 4 KiB": lambda cores, active, deployment: (
                cores / "core-1-finalized.mlir"
            ).write_text(
                finalized_module(2, 1).replace(
                    "byteSize = 4096 : i64", "byteSize = 4097 : i64", 1
                ),
                encoding="utf-8",
            ),
            "segment byte total mismatch": lambda cores, active, deployment: (
                cores / "core-1-finalized.mlir"
            ).write_text(
                finalized_module(2, 1).replace(
                    "bytesPerIteration = 4096 : i64",
                    "bytesPerIteration = 4095 : i64",
                    1,
                ),
                encoding="utf-8",
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                cores, active, deployment, output = self.make_fixture(Path(temporary))
                mutate(cores, active, deployment)
                result = self.run_validator(cores, active, deployment, output)
                self.assertNotEqual(result.returncode, 0)
                certificate = json.loads(output.read_text(encoding="utf-8"))
                self.assertEqual(certificate["status"], "FAIL")
                self.assertTrue(certificate["errors"])


if __name__ == "__main__":
    unittest.main()
