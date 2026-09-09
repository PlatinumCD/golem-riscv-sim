#!/usr/bin/env python3
"""Write or verify a reproducible Sculptor compiler/run manifest."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any


NAME_PATTERN = re.compile(r"[A-Za-z][A-Za-z0-9_.-]*")
BUFFER_BYTES = 1024 * 1024


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(BUFFER_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write_text(path: Path, text: str) -> None:
    """Replace *path* only after the complete payload is durable.

    Run manifests fingerprint very large model artifacts and are the commit
    record for SST reuse.  A timeout or host interruption must therefore leave
    either the previous complete manifest or no manifest, never a truncated
    JSON file that looks like a completed commit.
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, raw_temporary = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    )
    temporary = Path(raw_temporary)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as destination:
            destination.write(text)
            destination.flush()
            os.fsync(destination.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
        directory_descriptor = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def fingerprint_path(path: Path) -> dict[str, Any]:
    requested = path.absolute()
    resolved = path.resolve(strict=True)
    if resolved.is_file():
        return {
            "path": str(requested),
            "resolved_path": str(resolved),
            "kind": "file",
            "bytes": resolved.stat().st_size,
            "sha256": sha256_file(resolved),
        }
    if not resolved.is_dir():
        raise ValueError(f"cannot fingerprint non-file path: {path}")

    digest = hashlib.sha256()
    file_count = 0
    total_bytes = 0
    for child in sorted(resolved.rglob("*")):
        if not child.is_file():
            continue
        relative = child.relative_to(resolved).as_posix()
        child_size = child.stat().st_size
        child_hash = sha256_file(child)
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(child_size).encode("ascii"))
        digest.update(b"\0")
        digest.update(child_hash.encode("ascii"))
        digest.update(b"\0")
        file_count += 1
        total_bytes += child_size
    return {
        "path": str(requested),
        "resolved_path": str(resolved),
        "kind": "directory",
        "file_count": file_count,
        "bytes": total_bytes,
        "sha256": digest.hexdigest(),
    }


def git_output(root: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        capture_output=True,
    )
    if result.returncode:
        raise ValueError(
            f"git {' '.join(arguments)} failed in {root}: "
            + result.stderr.decode("utf-8", errors="replace")
        )
    return result.stdout


def fingerprint_source_tree(
    root: Path, scopes: tuple[str, ...] = ()
) -> dict[str, Any]:
    resolved = root.resolve(strict=True)
    normalized_scopes: list[str] = []
    for scope in scopes:
        candidate = Path(scope)
        if candidate.is_absolute() or ".." in candidate.parts:
            raise ValueError(f"source scope must be relative to {root}: {scope}")
        normalized_scopes.append(candidate.as_posix())
    path_arguments = ["--", *normalized_scopes]
    head = git_output(resolved, "rev-parse", "HEAD").decode().strip()
    tracked_diff = git_output(
        resolved,
        "diff",
        "--binary",
        "--no-ext-diff",
        "HEAD",
        *path_arguments,
    )
    untracked_output = git_output(
        resolved,
        "ls-files",
        "--others",
        "--exclude-standard",
        "-z",
        *path_arguments,
    )
    untracked_paths = sorted(
        item.decode("utf-8", errors="surrogateescape")
        for item in untracked_output.split(b"\0")
        if item
    )
    untracked_digest = hashlib.sha256()
    untracked_bytes = 0
    for relative in untracked_paths:
        path = resolved / relative
        if path.is_symlink():
            data = os.readlink(path).encode("utf-8", errors="surrogateescape")
            size = len(data)
            digest = sha256_bytes(data)
        elif path.is_file():
            size = path.stat().st_size
            digest = sha256_file(path)
        else:
            continue
        untracked_digest.update(relative.encode("utf-8", errors="surrogateescape"))
        untracked_digest.update(b"\0")
        untracked_digest.update(str(size).encode("ascii"))
        untracked_digest.update(b"\0")
        untracked_digest.update(digest.encode("ascii"))
        untracked_digest.update(b"\0")
        untracked_bytes += size

    status = git_output(
        resolved,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
        *path_arguments,
    )
    state = {
        "path": str(resolved),
        "scopes": normalized_scopes,
        "head": head,
        "dirty": bool(status),
        "status_sha256": sha256_bytes(status),
        "tracked_diff_sha256": sha256_bytes(tracked_diff),
        "untracked_sha256": untracked_digest.hexdigest(),
        "untracked_file_count": len(untracked_paths),
        "untracked_bytes": untracked_bytes,
    }
    state["state_sha256"] = sha256_bytes(
        json.dumps(state, sort_keys=True, separators=(",", ":")).encode()
    )
    return state


def named_path(value: str) -> tuple[str, Path]:
    name, separator, raw_path = value.partition("=")
    if not separator or not NAME_PATTERN.fullmatch(name) or not raw_path:
        raise argparse.ArgumentTypeError("expected NAME=PATH")
    return name, Path(raw_path)


def named_value(value: str) -> tuple[str, str]:
    name, separator, raw_value = value.partition("=")
    if not separator or not NAME_PATTERN.fullmatch(name):
        raise argparse.ArgumentTypeError("expected NAME=VALUE")
    return name, raw_value


def unique_mapping(values: list[tuple[str, Any]], kind: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in values:
        if name in result:
            raise ValueError(f"duplicate {kind} name: {name}")
        result[name] = value
    return result


def build_manifest(args: argparse.Namespace) -> dict[str, Any]:
    if args.output is None:
        raise ValueError("--output is required when writing a manifest")
    required_text = {
        "run_id": args.run_id,
        "run_mode": args.run_mode,
        "model": args.model,
        "memory_backend": args.memory_backend,
        "command": args.command,
    }
    missing = [name for name, value in required_text.items() if value is None]
    if missing or args.architecture_manifest is None:
        missing_arguments = missing + (
            ["architecture_manifest"]
            if args.architecture_manifest is None
            else []
        )
        raise ValueError(
            "missing manifest arguments: " + ", ".join(missing_arguments)
        )

    sources = unique_mapping(args.source, "source")
    source_scopes: dict[str, list[str]] = {name: [] for name in sources}
    for name, scope in args.source_scope:
        if name not in sources:
            raise ValueError(f"source scope refers to unknown source: {name}")
        source_scopes[name].append(scope)
    tools = unique_mapping(args.tool, "tool")
    artifacts = unique_mapping(args.artifact, "artifact")
    parameters = unique_mapping(args.parameter, "parameter")
    environment: dict[str, str] = {}
    for prefix in args.environment_prefix:
        environment.update(
            sorted(
                (name, value)
                for name, value in os.environ.items()
                if name.startswith(prefix)
            )
        )

    architecture_path = args.architecture_manifest.resolve(strict=True)
    architecture = json.loads(architecture_path.read_text(encoding="utf-8"))
    if architecture.get("schema") != "golem.streaming-architecture":
        raise ValueError("invalid streaming architecture manifest")

    return {
        "schema": "golem.sculptor-run",
        "schema_version": 1,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "run": {
            "id": args.run_id,
            "mode": args.run_mode,
            "model": args.model,
            "memory_backend": args.memory_backend,
            "command": args.command,
        },
        "hardware": {
            "mesh_rows": args.mesh_rows,
            "mesh_columns": args.mesh_columns,
            "arrays_per_core": args.arrays_per_core,
            "array_rows": args.array_rows,
            "array_columns": args.array_columns,
        },
        "architecture_manifest": {
            **fingerprint_path(architecture_path),
            "architecture": architecture["architecture"],
        },
        "source_trees": {
            name: fingerprint_source_tree(
                path, tuple(source_scopes.get(name, ()))
            )
            for name, path in sources.items()
        },
        "tools": {name: fingerprint_path(path) for name, path in tools.items()},
        "artifacts": {
            name: fingerprint_path(path) for name, path in artifacts.items()
        },
        "parameters": dict(sorted(parameters.items())),
        "environment": dict(sorted(environment.items())),
    }


def verify_manifest(path: Path) -> list[str]:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != "golem.sculptor-run":
        return ["invalid run manifest schema"]
    failures: list[str] = []
    for section in ("tools", "artifacts"):
        records = manifest.get(section, {})
        if not isinstance(records, dict):
            failures.append(f"invalid {section} section")
            continue
        for name, expected in records.items():
            try:
                actual = fingerprint_path(Path(expected["path"]))
            except (OSError, ValueError, KeyError) as error:
                failures.append(f"{section}.{name}: {error}")
                continue
            if actual.get("sha256") != expected.get("sha256"):
                failures.append(
                    f"{section}.{name}: sha256 changed "
                    f"({expected.get('sha256')} -> {actual.get('sha256')})"
                )

    architecture = manifest.get("architecture_manifest")
    if not isinstance(architecture, dict):
        failures.append("invalid architecture_manifest section")
    else:
        try:
            actual = fingerprint_path(Path(architecture["path"]))
        except (OSError, ValueError, KeyError) as error:
            failures.append(f"architecture_manifest: {error}")
        else:
            if actual.get("sha256") != architecture.get("sha256"):
                failures.append("architecture_manifest: sha256 changed")

    sources = manifest.get("source_trees", {})
    if not isinstance(sources, dict):
        failures.append("invalid source_trees section")
    else:
        for name, expected in sources.items():
            try:
                actual = fingerprint_source_tree(
                    Path(expected["path"]), tuple(expected.get("scopes", ()))
                )
            except (OSError, ValueError, KeyError) as error:
                failures.append(f"source_trees.{name}: {error}")
                continue
            if actual.get("state_sha256") != expected.get("state_sha256"):
                failures.append(
                    f"source_trees.{name}: source state changed "
                    f"({expected.get('state_sha256')} -> {actual.get('state_sha256')})"
                )
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--architecture-manifest", type=Path)
    parser.add_argument("--run-id")
    parser.add_argument("--run-mode", choices=("compile", "simulation"))
    parser.add_argument("--model")
    parser.add_argument("--memory-backend")
    parser.add_argument("--command")
    parser.add_argument("--mesh-rows", type=int)
    parser.add_argument("--mesh-columns", type=int)
    parser.add_argument("--arrays-per-core", type=int)
    parser.add_argument("--array-rows", type=int)
    parser.add_argument("--array-columns", type=int)
    parser.add_argument("--source", type=named_path, action="append", default=[])
    parser.add_argument(
        "--source-scope", type=named_value, action="append", default=[]
    )
    parser.add_argument("--tool", type=named_path, action="append", default=[])
    parser.add_argument("--artifact", type=named_path, action="append", default=[])
    parser.add_argument("--parameter", type=named_value, action="append", default=[])
    parser.add_argument("--environment-prefix", action="append", default=[])
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.verify is not None:
            failures = verify_manifest(args.verify)
            if failures:
                for failure in failures:
                    print(f"run manifest mismatch: {failure}", file=sys.stderr)
                return 1
            print(f"run manifest verified: {args.verify}")
            return 0
        manifest = build_manifest(args)
        assert args.output is not None
        atomic_write_text(
            args.output,
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        )
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"run manifest error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
