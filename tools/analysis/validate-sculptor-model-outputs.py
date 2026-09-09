#!/usr/bin/env python3
"""Compare simulator output fingerprints with a deterministic host reference."""

from __future__ import annotations

import argparse
import json
import re
import struct
from pathlib import Path


OUTPUT_PREFIX = "SCULPTOR_MODEL_OUTPUT "
FIELD = re.compile(r"([a-zA-Z0-9_]+)=([0-9]+)")
REQUIRED_FIELDS = frozenset({"tile", "index", "bytes", "fingerprint"})


def bits_to_float(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def parse_fields(record: str) -> dict[str, int]:
    return {name: int(value) for name, value in FIELD.findall(record)}


def parse_observed(log: Path) -> dict[int, dict[str, int]]:
    outputs: dict[int, dict[str, int]] = {}
    for line in log.read_text(errors="replace").replace("\r", "").splitlines():
        if OUTPUT_PREFIX not in line:
            continue
        if not line.startswith(OUTPUT_PREFIX) or line.count(OUTPUT_PREFIX) != 1:
            raise RuntimeError(f"corrupt model output record: {line}")
        record = line
        fields = parse_fields(record)
        if not REQUIRED_FIELDS <= fields.keys():
            raise RuntimeError(f"malformed model output record: {record}")
        index = fields["index"]
        if index in outputs:
            raise RuntimeError(f"duplicate simulator output index: {index}")
        outputs[index] = fields
    return outputs


def validate(expected_path: Path, log: Path) -> dict[str, object]:
    expected_document = json.loads(expected_path.read_text())
    expected_outputs = {
        int(record["index"]): record for record in expected_document["outputs"]
    }
    observed_outputs = parse_observed(log)
    if set(observed_outputs) != set(expected_outputs):
        raise RuntimeError(
            "simulator output indices differ from reference: "
            f"expected {sorted(expected_outputs)}, observed {sorted(observed_outputs)}"
        )

    records: list[dict[str, object]] = []
    for index in sorted(expected_outputs):
        expected = expected_outputs[index]
        observed = observed_outputs[index]
        if observed.get("bytes") != expected["bytes"]:
            raise RuntimeError(
                f"output {index} byte count differs: expected {expected['bytes']}, "
                f"observed {observed.get('bytes')}"
            )
        exact = observed.get("fingerprint") == expected["fingerprint"]
        tolerance = (
            "tolerance_fingerprint" in expected
            and observed.get("tolerance_fingerprint")
            == expected["tolerance_fingerprint"]
        )
        if not exact and not tolerance:
            sample_differences = []
            for sample_index, expected_bits in enumerate(expected["sample_bits"]):
                key = f"sample{sample_index}_bits"
                if key not in observed:
                    break
                expected_value = bits_to_float(expected_bits)
                observed_value = bits_to_float(observed[key])
                sample_differences.append(
                    {
                        "index": sample_index,
                        "expected": expected_value,
                        "observed": observed_value,
                        "absolute_error": abs(expected_value - observed_value),
                    }
                )
            raise RuntimeError(
                f"output {index} differs from the PyTorch reference; "
                f"sample differences: {sample_differences}"
            )
        records.append(
            {
                "index": index,
                "bytes": expected["bytes"],
                "comparison": "exact" if exact else "float32-tolerance-fingerprint",
            }
        )
    return {"status": "PASS", "outputs": records}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", type=Path, required=True)
    parser.add_argument("--simulation-log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    try:
        report = validate(args.expected, args.simulation_log)
    except Exception as error:
        report = {"status": "FAIL", "error": str(error)}
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        raise SystemExit(str(error)) from error
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(
        f"validated {len(report['outputs'])} simulator output(s) against PyTorch"
    )


if __name__ == "__main__":
    main()
