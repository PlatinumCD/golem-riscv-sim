#!/usr/bin/env python3
"""Run a command with separate work and post-checkpoint deadlines."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from typing import Any


CHECKPOINT_SCHEMA = "sculptor.compile-ready"
STATUS_SCHEMA = "sculptor.checkpoint-timeout"


def positive_seconds(value: str) -> float:
    try:
        seconds = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected a number of seconds") from error
    if not seconds > 0:
        raise argparse.ArgumentTypeError("seconds must be positive")
    return seconds


def atomic_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, raw_temporary = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    )
    temporary = Path(raw_temporary)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as destination:
            json.dump(payload, destination, indent=2, sort_keys=True)
            destination.write("\n")
            destination.flush()
            os.fsync(destination.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def write_status(path: Path, payload: dict[str, Any]) -> None:
    try:
        atomic_json(path, payload)
    except OSError as error:
        # Status reporting must never replace the supervised command's real
        # exit code (for example, when the command rejected an overlong output
        # path before creating its directory).
        print(f"cannot write checkpoint supervisor status: {error}", file=sys.stderr)


def valid_checkpoint(path: Path) -> bool:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return False
    return (
        isinstance(payload, dict)
        and payload.get("schema") == CHECKPOINT_SCHEMA
        and payload.get("version") == 1
        and payload.get("status") == "COMPILE_READY"
    )


def terminate_group(process: subprocess.Popen[Any], grace: float) -> int:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return process.wait()
    try:
        return process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        return process.wait()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-timeout", type=positive_seconds, required=True)
    parser.add_argument(
        "--finalization-timeout", type=positive_seconds, required=True
    )
    parser.add_argument("--term-grace", type=positive_seconds, default=10.0)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--status", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not args.command:
        parser.error("a command is required after --")
    return args


def main() -> int:
    args = parse_args()
    checkpoint = args.checkpoint.absolute()
    status_path = args.status.absolute()
    if checkpoint.exists() or checkpoint.is_symlink():
        print(f"checkpoint already exists before launch: {checkpoint}", file=sys.stderr)
        return 2

    started = time.monotonic()
    checkpoint_seen: float | None = None
    try:
        process = subprocess.Popen(args.command, start_new_session=True)
    except OSError as error:
        write_status(
            status_path,
            {
                "schema": STATUS_SCHEMA,
                "version": 1,
                "outcome": "launch_error",
                "error": str(error),
            },
        )
        print(f"cannot launch supervised command: {error}", file=sys.stderr)
        return 126

    outcome = "command_failed"
    exit_code = 1
    try:
        while True:
            now = time.monotonic()
            if checkpoint_seen is None and valid_checkpoint(checkpoint):
                checkpoint_seen = now
                print(
                    "compile-ready checkpoint observed; "
                    f"starting {args.finalization_timeout:g}s finalization deadline",
                    flush=True,
                )

            child_status = process.poll()
            if child_status is not None:
                exit_code = child_status
                if child_status == 0 and checkpoint_seen is not None:
                    outcome = "complete"
                elif child_status == 0:
                    outcome = "checkpoint_missing"
                    exit_code = 1
                break

            if checkpoint_seen is None:
                timed_out = now - started >= args.work_timeout
                timeout_outcome = "work_timeout"
            else:
                timed_out = now - checkpoint_seen >= args.finalization_timeout
                timeout_outcome = "finalization_timeout"
            if timed_out:
                outcome = timeout_outcome
                print(
                    f"checkpoint supervisor: {timeout_outcome}",
                    file=sys.stderr,
                    flush=True,
                )
                terminate_group(process, args.term_grace)
                exit_code = 124
                break
            time.sleep(0.05)
    except KeyboardInterrupt:
        outcome = "interrupted"
        terminate_group(process, args.term_grace)
        exit_code = 130

    ended = time.monotonic()
    write_status(
        status_path,
        {
            "schema": STATUS_SCHEMA,
            "version": 1,
            "outcome": outcome,
            "exit_code": exit_code,
            "checkpoint_observed": checkpoint_seen is not None,
            "work_timeout_seconds": args.work_timeout,
            "finalization_timeout_seconds": args.finalization_timeout,
            "wall_seconds": ended - started,
        },
    )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
