#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path


WORDS_PER_ROUTE = 5 + 512
TICKS_PER_LINK_WORD = 1000


def numbered_directories(parent, prefix):
    directories = []
    for path in parent.glob(f"{prefix}*"):
        if not path.is_dir():
            continue
        try:
            number = int(path.name.removeprefix(prefix))
        except ValueError:
            continue
        directories.append((number, path))
    return sorted(directories)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("--mode", default="polling")
    args = parser.parse_args()

    rows = []
    mode_directory = args.results / args.mode
    for fanout, fanout_directory in numbered_directories(
        mode_directory, "fanout-"
    ):
        for quantum, trial in numbered_directories(
            fanout_directory, "quantum-"
        ):
            if not (trial / "report" / "summary.json").is_file():
                continue
            with (trial / "report" / "summary.json").open(
                "r", encoding="utf-8"
            ) as source:
                summary = json.load(source)
            routes = list(
                csv.DictReader(
                    (trial / "report" / "routes.csv").open(
                        "r", encoding="utf-8", newline=""
                    )
                )
            )
            task_rows = list(
                csv.DictReader(
                    (trial / "report" / "critical-path.csv").open(
                        "r", encoding="utf-8", newline=""
                    )
                )
            )
            source_finish = min(
                int(row["finish_tick"])
                for row in task_rows
                if int(row["task_id"]) == 11
            )
            first_injection = min(
                int(row["injection_start_tick"]) for row in routes
            )
            last_injection = max(
                int(row["injection_finish_tick"]) for row in routes
            )
            rows.append(
                {
                    "mode": args.mode,
                    "fanout": fanout,
                    "sync_quantum": quantum,
                    "simulated_time_ticks": summary["finish_tick"],
                    "source_finish_tick": source_finish,
                    "first_injection_tick": first_injection,
                    "last_injection_tick": last_injection,
                    "finish_to_first_injection_ticks":
                        first_injection - source_finish,
                    "finish_to_last_injection_ticks":
                        last_injection - source_finish,
                    "transmit_blocked_ticks":
                        summary["transmit_backpressure"][
                            "blocked_ticks"
                        ],
                    "transmit_blocked_events":
                        summary["transmit_backpressure"]["events"],
                    "transmit_retries":
                        summary["transmit_backpressure"]["retries"],
                    "maximum_queue_occupancy":
                        summary["transmit_backpressure"][
                            "maximum_queue_occupancy"
                        ],
                    "instructions":
                        summary["tile_totals"]["instructions"],
                    "cpu_cycles":
                        summary["tile_totals"]["cpu_cycles"],
                    "injected_words":
                        summary["network"]["injected_words"],
                    "word_hops":
                        summary["network"][
                            "directional_word_hops"
                        ],
                    "router_stalls":
                        summary["network"]["router_stalls"],
                    "analytical_serialization_ticks":
                        fanout
                        * WORDS_PER_ROUTE
                        * TICKS_PER_LINK_WORD,
                }
            )

    if not rows:
        raise SystemExit(
            f"no completed {args.mode} fan-out trials found"
        )
    output = args.results / f"{args.mode}-results.csv"
    with output.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.DictWriter(destination, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(output)


if __name__ == "__main__":
    main()
