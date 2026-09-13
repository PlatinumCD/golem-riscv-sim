#!/usr/bin/env python3
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
minimum_misses = int(sys.argv[2]) if len(sys.argv) > 2 else 0
minimum_invalidations = int(sys.argv[3]) if len(sys.argv) > 3 else 0
expected_unretired = int(sys.argv[4]) if len(sys.argv) > 4 else 0
match = re.search(
    r"INSTRUCTION_CACHE tile=0 accesses=\d+ hits=\d+ misses=\d+ "
    r"fill_bytes=\d+ stall_cycles=\d+ invalidations=\d+ unretired_fetches=\d+", text
)
if not match:
    raise SystemExit("missing or malformed exact INSTRUCTION_CACHE stdout line")
fields = {k: int(v) for k, v in re.findall(
    r"(accesses|hits|misses|fill_bytes|stall_cycles|invalidations|unretired_fetches)=(\d+)", match.group()
)}
if fields["hits"] + fields["misses"] != fields["accesses"]:
    raise SystemExit("I-cache hit/miss accounting is inconsistent")
if fields["misses"] < minimum_misses:
    raise SystemExit(f"expected at least {minimum_misses} I-cache misses")
if fields["invalidations"] < minimum_invalidations:
    raise SystemExit(f"expected at least {minimum_invalidations} I-cache invalidations")
if fields["unretired_fetches"] != expected_unretired:
    raise SystemExit(f"expected {expected_unretired} unretired fetches, got {fields['unretired_fetches']}")
print(match.group())
