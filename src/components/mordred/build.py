"""Build the pinned Mordred SST library without installing or registering it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def build(output, core=None):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    provenance = json.loads((HERE / "UPSTREAM.json").read_text())
    for name, digest in provenance["sha256"].items():
        path = HERE / "upstream" / name
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"Vendored Mordred differs from its pinned manifest: {name}")
    core = Path(core or os.environ.get("SST_CORE_ROOT", ROOT / "install/sst-core")).resolve()
    config = core / "bin/sst-config"
    version = subprocess.check_output([str(core / "bin/sst"), "--version"], text=True).strip()
    match = re.search(r"Version \((\d+)\.", version)
    if not match or int(match[1]) < 15:
        raise RuntimeError(f"Mordred requires SST >= 15; found {version}")
    compiler = shlex.split(subprocess.check_output([str(config), "--CXX"], text=True))
    flags = shlex.split(subprocess.check_output([str(config), "--ELEMENT_CXXFLAGS"], text=True))
    upstream_source = HERE / "upstream/src"
    # Retain the pinned upstream bytes. Observation hooks are applied only to
    # build-local copies and remain inactive without the profiling environment.
    source = output / "mordred-source"
    source.mkdir(exist_ok=True)
    for path in upstream_source.iterdir():
        if path.is_file():
            shutil.copyfile(path, source / path.name)
    patch = HERE / "patches/cycle-profile.patch"
    subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
                   cwd=source, check=True, capture_output=True)
    # Same raw-link source set as upstream CMake with PHYS_CHANNEL=OFF.
    # Optional Prydwen transport is deliberately not enabled.
    sources = sorted(p for p in source.glob("*.cc")
                     if p.name not in ("MordredNicPC.cc", "RtrPortControlPC.cc"))
    inputs = [Path(__file__).resolve(), HERE / "UPSTREAM.json", patch, HERE.parent / "cycleProfile.h"]
    inputs += sorted(p for p in upstream_source.iterdir() if p.suffix in (".h", ".cc"))
    inputs += sorted(p for p in source.iterdir() if p.suffix in (".h", ".cc"))
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    library = output / "libmordred.so"
    command = compiler + flags + ["-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-shared", f"-DSST_MAJOR_VERSION={match[1]}", f"-I{HERE.parent}", *map(str, sources), "-o", str(library)]
    with (output / "mordred-build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
    if before != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}:
        raise RuntimeError("Mordred sources changed during compilation; rebuild before validating")
    info = dict(sst=str(core / "bin/sst"), sst_version=version, plugin=str(output),
                library=str(library), library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
                upstream_revision=provenance["revision"],
                observation_patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                physical_channel_enabled=False, command=command, source_sha256=before)
    (output / "mordred-build.json").write_text(json.dumps(info, indent=2) + "\n")
    return info


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/source_new-mordred")
    args = parser.parse_args()
    print(build(args.output)["library"])
