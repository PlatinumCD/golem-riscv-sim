#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
import sys
import tempfile


PROJECT_ROOT = Path(__file__).resolve().parents[3]
WRITER = PROJECT_ROOT / "scripts" / "write-sculptor-run-manifest.py"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="sculptor-run-manifest-") as raw:
        directory = Path(raw)
        architecture = directory / "architecture.json"
        architecture.write_text(
            json.dumps(
                {
                    "schema": "golem.streaming-architecture",
                    "schema_version": 1,
                    "architecture": {
                        "version": 1,
                        "fixed_shard_bytes": 4096,
                        "scratchpad_bytes": 2097152,
                        "global_ram_bytes": 34359738368,
                        "max_in_flight": 2,
                        "noc_word_bytes": 4,
                    },
                }
            ),
            encoding="utf-8",
        )
        tool = directory / "tool"
        tool.write_bytes(b"tool-v1")
        artifact = directory / "artifact.mlir"
        artifact.write_text("module {}\n", encoding="utf-8")
        manifest = directory / "run-manifest.json"

        subprocess.run(
            [
                sys.executable,
                str(WRITER),
                "--output",
                str(manifest),
                "--architecture-manifest",
                str(architecture),
                "--run-id",
                "test-run",
                "--run-mode",
                "compile",
                "--model",
                "fixture",
                "--memory-backend",
                "streaming",
                "--command",
                "fixture --compile-only",
                "--mesh-rows",
                "1",
                "--mesh-columns",
                "1",
                "--arrays-per-core",
                "1",
                "--array-rows",
                "1024",
                "--array-columns",
                "512",
                "--source",
                f"repository={PROJECT_ROOT}",
                "--source-scope",
                "repository=scripts",
                "--source-scope",
                "repository=tests/validation/run-manifest",
                "--tool",
                f"compiler={tool}",
                "--artifact",
                f"deployment={artifact}",
                "--parameter",
                "fixed_shard_bytes=4096",
            ],
            check=True,
        )
        subprocess.run(
            [sys.executable, str(WRITER), "--verify", str(manifest)],
            check=True,
        )
        assert not list(directory.glob(f".{manifest.name}.*.tmp"))

        payload = json.loads(manifest.read_text(encoding="utf-8"))
        assert payload["run"]["mode"] == "compile"
        assert payload["hardware"]["array_rows"] == 1024
        assert payload["architecture_manifest"]["architecture"][
            "fixed_shard_bytes"
        ] == 4096
        assert payload["source_trees"]["repository"]["head"]
        assert payload["tools"]["compiler"]["sha256"]
        assert payload["artifacts"]["deployment"]["sha256"]

        artifact.write_text("module { func.func @changed() }\n", encoding="utf-8")
        rejected = subprocess.run(
            [sys.executable, str(WRITER), "--verify", str(manifest)],
            check=False,
            capture_output=True,
            text=True,
        )
        assert rejected.returncode == 1
        assert "artifacts.deployment: sha256 changed" in rejected.stderr


if __name__ == "__main__":
    main()
