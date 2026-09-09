#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
import sys
import tempfile


PROJECT_ROOT = Path(__file__).resolve().parents[3]
SUPERVISOR = PROJECT_ROOT / "scripts" / "run-with-checkpoint-timeout.py"
CHECKPOINT_PAYLOAD = {
    "schema": "sculptor.compile-ready",
    "version": 1,
    "status": "COMPILE_READY",
}


def run_case(
    root: Path,
    name: str,
    *,
    work_timeout: float,
    finalization_timeout: float,
    program: str,
) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
    checkpoint = root / f"{name}-ready.json"
    status = root / f"{name}-status.json"
    result = subprocess.run(
        [
            sys.executable,
            str(SUPERVISOR),
            "--work-timeout",
            str(work_timeout),
            "--finalization-timeout",
            str(finalization_timeout),
            "--term-grace",
            "0.2",
            "--checkpoint",
            str(checkpoint),
            "--status",
            str(status),
            "--",
            sys.executable,
            "-c",
            program,
            str(checkpoint),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    payload = json.loads(status.read_text(encoding="utf-8"))
    assert payload["schema"] == "sculptor.checkpoint-timeout"
    assert payload["version"] == 1
    return result, payload


def main() -> None:
    encoded_checkpoint = json.dumps(CHECKPOINT_PAYLOAD)
    with tempfile.TemporaryDirectory(prefix="sculptor-checkpoint-timeout-") as raw:
        root = Path(raw)

        # The qualified work crosses its former aggregate deadline but receives
        # a fresh, bounded finalization interval after the atomic checkpoint.
        completed, status = run_case(
            root,
            "cross-work-deadline",
            work_timeout=0.2,
            finalization_timeout=1.0,
            program=(
                "import pathlib,sys,time; "
                "time.sleep(0.1); "
                f"pathlib.Path(sys.argv[1]).write_text({encoded_checkpoint!r}); "
                "time.sleep(0.25)"
            ),
        )
        assert completed.returncode == 0, completed.stderr
        assert status["outcome"] == "complete"
        assert status["checkpoint_observed"] is True
        assert float(status["wall_seconds"]) > 0.2

        work_timeout, status = run_case(
            root,
            "work-timeout",
            work_timeout=0.15,
            finalization_timeout=1.0,
            program="import time; time.sleep(1)",
        )
        assert work_timeout.returncode == 124
        assert status["outcome"] == "work_timeout"
        assert status["checkpoint_observed"] is False

        finalization_timeout, status = run_case(
            root,
            "finalization-timeout",
            work_timeout=1.0,
            finalization_timeout=0.15,
            program=(
                "import pathlib,sys,time; "
                f"pathlib.Path(sys.argv[1]).write_text({encoded_checkpoint!r}); "
                "time.sleep(1)"
            ),
        )
        assert finalization_timeout.returncode == 124
        assert status["outcome"] == "finalization_timeout"
        assert status["checkpoint_observed"] is True

        missing, status = run_case(
            root,
            "missing-checkpoint",
            work_timeout=1.0,
            finalization_timeout=1.0,
            program="pass",
        )
        assert missing.returncode == 1
        assert status["outcome"] == "checkpoint_missing"


if __name__ == "__main__":
    main()
