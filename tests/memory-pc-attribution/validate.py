#!/usr/bin/env python3

import csv
import sys
from pathlib import Path


def main():
    sites_path = Path(sys.argv[1])
    with sites_path.open("r", encoding="utf-8", newline="") as source:
        rows = [
            row
            for row in csv.DictReader(source)
            if row["task_id"] == "11"
        ]

    expected_callers = {
        "(anonymous namespace)::memoryAttributionSiteA()",
        "(anonymous namespace)::memoryAttributionSiteB()",
    }
    shared = "(anonymous namespace)::memoryAttributionSharedSite(unsigned long volatile*)"
    attributed = [
        row
        for row in rows
        if row["source_function"] == shared
    ]
    found_callers = {row["caller_function"] for row in attributed}
    if found_callers != expected_callers:
        raise RuntimeError(
            f"expected separate callers {sorted(expected_callers)}, "
            f"found {sorted(found_callers)}"
        )

    pcs = {int(row["guest_pc"]) for row in attributed}
    return_addresses = {
        row["caller_function"]: int(row["guest_ra"])
        for row in attributed
    }
    if len(pcs) != 1 or 0 in pcs:
        raise RuntimeError(f"expected one shared memory PC, found {pcs}")
    if len(set(return_addresses.values())) != 2 or 0 in return_addresses.values():
        raise RuntimeError(
            f"callers do not have distinct return addresses: {return_addresses}"
        )

    for function, return_address in sorted(return_addresses.items()):
        matching = [
            row
            for row in attributed
            if row["caller_function"] == function
        ]
        requests = sum(int(row["requests"]) for row in matching)
        stalls = sum(int(row["memory_stall_ticks"]) for row in matching)
        if requests == 0 or stalls == 0:
            raise RuntimeError(f"memory site has no timed request: {function}")
        print(
            f"{function}: shared_pc=0x{next(iter(pcs)):x}, "
            f"return_address=0x{return_address:x}, requests={requests}, "
            f"stall_ticks={stalls}"
        )


if __name__ == "__main__":
    main()
