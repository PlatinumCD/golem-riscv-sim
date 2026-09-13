#!/usr/bin/env python3

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path


OP_SET = 1
OP_LOAD = 2
OP_COMPUTE = 3
OP_STORE = 4
WORDS_PER_BEAT = 8
ROWS = 9
COLS = 9
COMPUTE_CYCLES = 16
SST_TICKS_PER_ANALOG_CYCLE = 10000


@dataclass
class Request:
    ticket: int
    operation: int
    array_id: int
    state: str = "queued"
    remaining: int = 0


def read_trace(path):
    with path.open(newline="") as stream:
        return [
            {
                **row,
                "ticket": int(row["ticket"]),
                "operation": int(row["operation"]),
                "array_id": int(row["array_id"]),
                "device_cycle": int(row["device_cycle"]),
                "event_tick": int(row["event_tick"]),
            }
            for row in csv.DictReader(stream)
        ]


def read_summary(path):
    with path.open(newline="") as stream:
        return {
            row["metric"]: int(row["value"])
            for row in csv.DictReader(stream)
        }


class ReferenceDevice:
    def __init__(self, array_count, arrivals):
        self.array_count = array_count
        self.arrivals = arrivals
        self.arrival_index = 0
        self.cycle = 0
        self.next_link_array = 0
        self.queues = [[] for _ in range(array_count)]
        self.events = []
        self.link_beats = 0

    def emit(self, request, phase):
        self.events.append(
            (
                request.ticket,
                request.operation,
                request.array_id,
                phase,
                self.cycle,
            )
        )

    def start_request(self, request):
        if request.operation in (OP_SET, OP_LOAD):
            request.state = "input"
            words = ROWS * COLS if request.operation == OP_SET else COLS
            request.remaining = (words + WORDS_PER_BEAT - 1) // WORDS_PER_BEAT
            self.emit(request, "input-transfer-start")
        elif request.operation == OP_COMPUTE:
            request.state = "compute"
            request.remaining = COMPUTE_CYCLES
            self.emit(request, "compute-start")
        elif request.operation == OP_STORE:
            request.state = "output"
            request.remaining = (ROWS + WORDS_PER_BEAT - 1) // WORDS_PER_BEAT
            self.emit(request, "output-transfer-start")
        else:
            raise AssertionError(f"unsupported operation {request.operation}")

    def start_ready(self):
        for queue in self.queues:
            if queue and queue[0].state == "queued":
                self.start_request(queue[0])

    def submit_arrivals(self):
        while self.arrival_index < len(self.arrivals):
            arrival = self.arrivals[self.arrival_index]
            if arrival["device_cycle"] != self.cycle:
                break
            request = Request(
                arrival["ticket"],
                arrival["operation"],
                arrival["array_id"],
            )
            if not 0 <= request.array_id < self.array_count:
                raise AssertionError(f"invalid array {request.array_id}")
            self.emit(request, "submitted")
            self.queues[request.array_id].append(request)
            self.start_ready()
            self.arrival_index += 1

    def active_requests(self):
        active = []
        identities = set()
        for queue in self.queues:
            if not queue:
                continue
            request = queue[0]
            if request.state in ("queued", "complete"):
                continue
            identity = id(request)
            if identity not in identities:
                identities.add(identity)
                active.append(request)
        return active

    def finish(self, request, finish_phase):
        self.emit(request, finish_phase)
        request.state = "complete"
        self.emit(request, "complete")
        queue = self.queues[request.array_id]
        if not queue or queue[0] is not request:
            raise AssertionError("reference queue order was violated")
        queue.pop(0)

    def tick(self):
        active = self.active_requests()
        if not active:
            raise AssertionError("reference device cannot advance while idle")

        self.cycle += 1

        for request in active:
            if request.state != "compute":
                continue
            request.remaining -= 1
            if request.remaining == 0:
                self.finish(request, "compute-finish")

        candidates = [
            request
            for request in active
            if request.state in ("input", "output")
        ]
        if candidates:
            winner = min(
                candidates,
                key=lambda request: (
                    (request.array_id + self.array_count - self.next_link_array)
                    % self.array_count,
                    request.ticket,
                ),
            )
            winner.remaining -= 1
            self.link_beats += 1
            self.next_link_array = (winner.array_id + 1) % self.array_count
            if winner.remaining == 0:
                phase = (
                    "input-transfer-finish"
                    if winner.state == "input"
                    else "output-transfer-finish"
                )
                self.finish(winner, phase)

        self.start_ready()

    def run(self):
        while self.arrival_index < len(self.arrivals) or any(self.queues):
            self.submit_arrivals()
            if any(self.queues):
                self.tick()
                continue
            if self.arrival_index < len(self.arrivals):
                next_cycle = self.arrivals[self.arrival_index]["device_cycle"]
                if next_cycle != self.cycle:
                    raise AssertionError(
                        "device cycle advanced while the reference device "
                        f"was idle: expected {self.cycle}, observed {next_cycle}"
                    )
        return self.events


def phase_event(rows, ticket, phase):
    matches = [
        row for row in rows
        if row["ticket"] == ticket and row["phase"] == phase
    ]
    if len(matches) != 1:
        raise AssertionError(
            f"ticket {ticket} has {len(matches)} '{phase}' events"
        )
    return matches[0]


def overlap(intervals):
    total = 0
    for index, first in enumerate(intervals):
        for second in intervals[index + 1:]:
            if first[0] == second[0]:
                continue
            total += max(
                0,
                min(first[2], second[2]) - max(first[1], second[1]),
            )
    return total


def validate_case(name, array_count, trace_path, summary_path):
    rows = read_trace(trace_path)
    summary = read_summary(summary_path)
    arrivals = [row for row in rows if row["phase"] == "submitted"]
    expected_commands = 4 * array_count
    if len(arrivals) != expected_commands:
        raise AssertionError(
            f"{name}: expected {expected_commands} commands, "
            f"observed {len(arrivals)}"
        )

    reference = ReferenceDevice(array_count, arrivals)
    expected = reference.run()
    observed = [
        (
            row["ticket"],
            row["operation"],
            row["array_id"],
            row["phase"],
            row["device_cycle"],
        )
        for row in rows
    ]
    if observed != expected:
        mismatch = next(
            (
                (index, wanted, got)
                for index, (wanted, got) in enumerate(zip(expected, observed))
                if wanted != got
            ),
            None,
        )
        if mismatch is None:
            mismatch = (
                min(len(expected), len(observed)),
                f"{len(expected)} total events",
                f"{len(observed)} total events",
            )
        raise AssertionError(
            f"{name}: analog phase trace differs from reference at "
            f"event {mismatch[0]}: expected {mismatch[1]}, "
            f"observed {mismatch[2]}"
        )

    if summary["analog_active_cycles"] != reference.cycle:
        raise AssertionError(
            f"{name}: expected {reference.cycle} active cycles, "
            f"observed {summary['analog_active_cycles']}"
        )
    if summary["analog_link_beats"] != reference.link_beats:
        raise AssertionError(
            f"{name}: expected {reference.link_beats} link beats, "
            f"observed {summary['analog_link_beats']}"
        )

    phase_pairs = {
        OP_SET: ("input-transfer-start", "input-transfer-finish"),
        OP_LOAD: ("input-transfer-start", "input-transfer-finish"),
        OP_COMPUTE: ("compute-start", "compute-finish"),
        OP_STORE: ("output-transfer-start", "output-transfer-finish"),
    }
    transfer_intervals = []
    compute_intervals = []
    for arrival in arrivals:
        start_phase, finish_phase = phase_pairs[arrival["operation"]]
        start = phase_event(rows, arrival["ticket"], start_phase)
        finish = phase_event(rows, arrival["ticket"], finish_phase)
        device_duration = finish["device_cycle"] - start["device_cycle"]
        sst_duration = finish["event_tick"] - start["event_tick"]
        # Commands can arrive between analog clock edges. The controller maps
        # absolute ticks to floor(clock cycles), so do the same at both ends;
        # multiplying a duration incorrectly includes the arrival's phase.
        elapsed_edges = (finish["event_tick"] // SST_TICKS_PER_ANALOG_CYCLE -
                         start["event_tick"] // SST_TICKS_PER_ANALOG_CYCLE)
        if elapsed_edges != device_duration:
            raise AssertionError(
                f"{name}: ticket {arrival['ticket']} advanced "
                f"{device_duration} device cycles but {sst_duration} SST ticks"
            )
        interval = (
            arrival["array_id"],
            start["device_cycle"],
            finish["device_cycle"],
        )
        if arrival["operation"] == OP_COMPUTE:
            if device_duration != COMPUTE_CYCLES:
                raise AssertionError(
                    f"{name}: compute ticket {arrival['ticket']} took "
                    f"{device_duration}, expected {COMPUTE_CYCLES} cycles"
                )
            compute_intervals.append(interval)
        else:
            transfer_intervals.append(interval)

    transfer_overlap = overlap(transfer_intervals)
    compute_overlap = overlap(compute_intervals)
    if array_count == 2:
        if transfer_overlap == 0:
            raise AssertionError(
                f"{name}: the two arrays never contended for the shared link"
            )
        if compute_overlap == 0:
            raise AssertionError(
                f"{name}: independent array computation did not overlap"
            )

    return {
        "case": name,
        "arrays": array_count,
        "commands": expected_commands,
        "expected_active_cycles": reference.cycle,
        "measured_active_cycles": summary["analog_active_cycles"],
        "expected_link_beats": reference.link_beats,
        "measured_link_beats": summary["analog_link_beats"],
        "cross_array_transfer_overlap_cycles": transfer_overlap,
        "cross_array_compute_overlap_cycles": compute_overlap,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "cases",
        nargs="+",
        help="NAME:ARRAY_COUNT:TRACE_CSV:SUMMARY_CSV",
    )
    arguments = parser.parse_args()

    results = []
    for specification in arguments.cases:
        name, count, trace, summary = specification.split(":", 3)
        results.append(
            validate_case(
                name,
                int(count),
                Path(trace),
                Path(summary),
            )
        )

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    with arguments.output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)

    print("case    arrays  active cycles  link beats  transfer overlap  compute overlap")
    for result in results:
        print(
            f"{result['case']:<7} "
            f"{result['arrays']:>6} "
            f"{result['measured_active_cycles']:>14} "
            f"{result['measured_link_beats']:>11} "
            f"{result['cross_array_transfer_overlap_cycles']:>17} "
            f"{result['cross_array_compute_overlap_cycles']:>16}"
        )


if __name__ == "__main__":
    main()
