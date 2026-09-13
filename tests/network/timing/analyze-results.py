#!/usr/bin/env python3
"""Compare isolated wormhole measurements with the configured closed form."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


TICKS_PER_CYCLE = 1000
# With edge-aligned injection, one hop crosses three ten-cycle links
# and two three-cycle router head pipelines: 3*10 + 2*3 = 36.
# Each extra hop adds one link plus one router pipeline (10+3 = 13).
ONE_HOP_HEAD_CYCLES = 36
ADDITIONAL_HOP_CYCLES = 13


def coordinates(endpoint: int, width: int) -> tuple[int, int]:
    return endpoint % width, endpoint // width


def hops(source: int, destination: int, width: int) -> int:
    source_x, source_y = coordinates(source, width)
    destination_x, destination_y = coordinates(destination, width)
    return abs(source_x - destination_x) + abs(source_y - destination_y)


def stall_total(path: Path) -> tuple[int, int]:
    output_stalls = 0
    crossbar_stalls = 0
    with path.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source):
            value = int(row["Sum.u64"])
            if row["StatisticName"] == "output_credit_stall_cycles":
                output_stalls += value
            elif row["StatisticName"] == "switch_arbitration_stall_cycles":
                crossbar_stalls += value
    return output_stalls, crossbar_stalls


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: analyze-results.py TRIALS.tsv RESULTS.csv"
        )
    manifest_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    results: list[dict[str, object]] = []
    with manifest_path.open(encoding="utf-8", newline="") as source:
        trials = list(csv.DictReader(source, delimiter="\t"))

    for trial in trials:
        width = int(trial["width"])
        destination = int(trial["destination"])
        words = int(trial["payload_words"])
        source_count = int(trial["source_count"])
        raw_path = Path(trial["raw_path"])
        stats_path = Path(trial["stats_path"])
        with raw_path.open(encoding="utf-8", newline="") as source:
            receipts = list(csv.DictReader(source))
        if len(receipts) != source_count:
            raise RuntimeError(
                f"{raw_path}: expected {source_count} receipts, "
                f"found {len(receipts)}"
            )

        injections = {int(row["injection_tick"]) for row in receipts}
        simultaneous = len(injections) == 1
        measured = []
        for receipt in receipts:
            latency_ticks = int(receipt["latency_ticks"])
            delivery_latency_ticks = (
                int(receipt["delivery_tick"]) -
                int(receipt["injection_tick"])
            )
            if (
                latency_ticks % TICKS_PER_CYCLE or
                delivery_latency_ticks % TICKS_PER_CYCLE
            ):
                raise RuntimeError(
                    f"{raw_path}: a measured latency is not an integer "
                    "1 GHz cycle"
                )
            source_id = int(receipt["source"])
            measured.append(
                {
                    "source": source_id,
                    "hops": hops(source_id, destination, width),
                    "delivery_cycles": (
                        delivery_latency_ticks // TICKS_PER_CYCLE
                    ),
                    "completion_cycles": (
                        latency_ticks // TICKS_PER_CYCLE
                    ),
                    "injection_tick": int(receipt["injection_tick"]),
                    "delivery_tick": int(
                        receipt["delivery_tick"]
                    ),
                    "completion_tick": int(
                        receipt["completion_tick"]
                    ),
                }
            )
        measured.sort(
            key=lambda row: (
                row["completion_cycles"],
                row["source"],
            )
        )
        output_stalls, crossbar_stalls = stall_total(stats_path)

        for rank, measurement in enumerate(measured):
            # NIC callbacks expose completed packets, never a head-only arrival.
            packet_cycles = words
            base_head = (
                ONE_HOP_HEAD_CYCLES +
                ADDITIONAL_HOP_CYCLES *
                (measurement["hops"] - 1)
            )
            if trial["experiment"] == "contention":
                if measurement["hops"] != 1:
                    raise RuntimeError(
                        f"{raw_path}: contention source is not one hop away"
                    )
                predicted_head = base_head + rank * packet_cycles
            else:
                predicted_head = base_head
            predicted_completion = (
                predicted_head + packet_cycles - 1
            )
            head_error = (
                measurement["delivery_cycles"] - predicted_completion
            )
            absolute_error = (
                measurement["completion_cycles"] -
                predicted_completion
            )
            percentage_error = (
                100.0 * absolute_error / predicted_completion
                if predicted_completion else 0.0
            )
            passed = (
                absolute_error == 0
                and head_error == 0
                and measurement["hops"] == int(trial["expected_hops"])
                and (source_count == 1 or simultaneous)
            )
            results.append(
                {
                    "experiment": trial["experiment"],
                    "configuration": trial["configuration"],
                    "source": measurement["source"],
                    "destination": destination,
                    "payload_words": words,
                    "hops": measurement["hops"],
                    "source_count": source_count,
                    "arrival_rank": rank,
                    "predicted_delivery_cycles": predicted_completion,
                    "measured_delivery_cycles": measurement["delivery_cycles"],
                    "delivery_error_cycles": head_error,
                    "predicted_completion_cycles": predicted_completion,
                    "measured_completion_cycles": (
                        measurement["completion_cycles"]
                    ),
                    "absolute_error": absolute_error,
                    "percentage_error": f"{percentage_error:.6f}",
                    "simultaneous_injection": int(simultaneous),
                    "output_credit_stall_cycles": output_stalls,
                    "crossbar_stalls": crossbar_stalls,
                    "pass": int(passed),
                    "injection_tick": measurement["injection_tick"],
                    "delivery_tick": (
                        measurement["delivery_tick"]
                    ),
                    "completion_tick": measurement["completion_tick"],
                }
            )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fields = list(results[0])
    with output_path.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=fields)
        writer.writeheader()
        writer.writerows(results)

    print(
        "experiment configuration source words hops sources "
        "delivery predicted measured completion predicted measured error pass"
    )
    for row in results:
        print(
            f"{row['experiment']:12} "
            f"{row['configuration']:22} "
            f"{row['source']:>2} "
            f"{row['payload_words']:>5} "
            f"{row['hops']:>4} "
            f"{row['source_count']:>7} "
            f"{row['predicted_delivery_cycles']:>9} "
            f"{row['measured_delivery_cycles']:>8} "
            f"{row['predicted_completion_cycles']:>10} "
            f"{row['measured_completion_cycles']:>8} "
            f"{row['absolute_error']:>5} "
            f"{'PASS' if row['pass'] else 'FAIL'}"
        )

    failures = [row for row in results if not row["pass"]]
    print(
        f"network timing validation: "
        f"{len(results) - len(failures)}/{len(results)} passed"
    )
    print(output_path)
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
