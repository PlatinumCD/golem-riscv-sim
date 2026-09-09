#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "validate_sst_partition",
    ROOT / "scripts" / "validate-sculptor-sst-partition.py",
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def partition(*groups: list[str]) -> str:
    lines = []
    component_id = 0
    for thread, names in enumerate(groups):
        if not names:
            continue
        lines.append(f"Rank: 0.{thread} Component List:")
        for name in names:
            lines.append(f"   {name} (ID={component_id})")
            component_id += 1
            lines.append("      -> type      test.component")
    return "\n".join(lines) + "\n"


class SSTPartitionValidationTest(unittest.TestCase):
    def test_balanced_partition_passes(self) -> None:
        result = MODULE.validate_partition(
            partition(["tile0", "router_0_0"], ["tile1", "global_ram"]),
            [0, 1],
            2,
            True,
        )
        self.assertEqual(result["occupied_active_tile_threads"], 2)
        self.assertEqual(result["tile_threads"], {"0": 0, "1": 1})

    def test_empty_requested_thread_fails(self) -> None:
        with self.assertRaisesRegex(ValueError, "thread 1 owns no active tile"):
            MODULE.validate_partition(
                partition(["tile0", "tile1"], []), [0, 1], 2, True
            )

    def test_duplicate_tile_fails(self) -> None:
        with self.assertRaisesRegex(ValueError, "repeats active tile 0"):
            MODULE.validate_partition(
                partition(["tile0"], ["tile0"]), [0], 2, False
            )

    def test_unknown_thread_fails(self) -> None:
        with self.assertRaisesRegex(ValueError, "unexpected thread 1"):
            MODULE.validate_partition(
                partition([], ["tile0"]), [0], 1, False
            )


if __name__ == "__main__":
    unittest.main()
