"""Build the current register-interface QEMU from pinned sources and local overlays."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_OUTPUT = ROOT / "build/src/qemu"

# Dependency order matches build-scripts/prepare-qemu.sh. Some later-numbered
# patches establish context needed by earlier-numbered patches.
PATCH_ORDER = (1, 2, 4, 5, 13, 14, 15, 6, 7, 8, 9, 10, 11, 12,
               16, 17, 18, 19, 20, 21, 22)


def fingerprint(paths):
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in paths}


def copies():
    result = []
    for device in ("nic", "sync"):
        origin = HERE / "qemu"
        result += [(origin / f"mittens_{device}.c", f"hw/misc/mittens_{device}.c"),
                   (origin / f"mittens_{device}.h", f"include/hw/misc/mittens_{device}.h")]
    for name in ("NICTileBridge.h", "SyncTileBridge.h",
                 "MemoryMap.h", "FetchSegment.h"):
        include = HERE / "include/mittens"
        result.append((include / name,
                       f"include/mittens/{name}"))
    instructions = HERE / "qemu"
    result += [(instructions / "golem_analog_helper.c", "target/riscv/golem_analog_helper.c"),
               (instructions / "golem_lsq_helper.c", "target/riscv/golem_lsq_helper.c"),
               (instructions / "golem_slq_helper.c", "target/riscv/golem_slq_helper.c"),
               (instructions / "trans_golem_analog.c.inc",
                "target/riscv/insn_trans/trans_golem_analog.c.inc")]
    return result


def build(output=DEFAULT_OUTPUT, jobs=None):
    output = Path(output).resolve()
    if output in (ROOT, ROOT / "src", ROOT / "third_party", ROOT / "build", ROOT / "install"):
        raise ValueError("QEMU output must be a dedicated build directory")
    source = output / "source"
    binary_dir = output / "build"
    output.mkdir(parents=True, exist_ok=True)
    versions = ROOT / "src/config/build/versions.env"
    revision = re.search(r"^QEMU_COMMIT=([0-9a-f]+)$", versions.read_text(), re.M).group(1)
    patch_dir = HERE / "qemu/base-patches"
    patches = [next(patch_dir.glob(f"{number:04d}-*.patch")) for number in PATCH_ORDER]
    patches += sorted((HERE / "qemu").glob("*.patch"))
    base = {"revision": revision, "patches": fingerprint(patches), "preparation_version": 2}
    stamp = output / "source.json"
    owner = output / "source.owner"
    old = json.loads(stamp.read_text()) if stamp.exists() else None
    if old != base:
        if source.exists() and old is None and not owner.exists():
            raise RuntimeError(f"Refusing to replace an unowned source directory: {source}")
        if old is not None or owner.exists():
            shutil.rmtree(source, ignore_errors=True)
            shutil.rmtree(binary_dir, ignore_errors=True)
        owner.write_text("source_new QEMU generated source\n")
        source.mkdir()
        with (output / "prepare.log").open("w") as log:
            # An archive avoids modifying Git worktrees, submodule state, or
            # the prepared source used by the original src implementation.
            archive = subprocess.Popen(
                ["git", "-C", str(ROOT / "third_party/qemu"), "archive", revision],
                stdout=subprocess.PIPE, stderr=log)
            try:
                subprocess.run(["tar", "-x", "-C", str(source)],
                               stdin=archive.stdout, stdout=log, stderr=log, check=True)
            finally:
                archive.stdout.close()
                code = archive.wait()
            if code:
                raise RuntimeError(f"QEMU archive failed; see {output / 'prepare.log'}")
            for patch in patches:
                # These compatibility patches repair older prepared trees;
                # the current synchronization patch already supplies them.
                compatibility = {
                    13: ("hw/riscv/virt.c", "mittens_sync_create();"),
                    14: ("accel/tcg/tcg-accel-ops-icount.c",
                         "cpu_budget = mittens_sync_wait_for_grant(cpu_budget);"),
                    15: ("accel/tcg/tcg-accel-ops-icount.c",
                         "mittens_sync_begin_quantum(cpu->icount_budget);"),
                }
                check = compatibility.get(int(patch.name[:4]))
                if check and check[1] in (source / check[0]).read_text():
                    continue
                # Historical compatibility patches repeat some complete file
                # changes. Check each file independently before applying it.
                sections = re.split(r"(?=^diff --git )", patch.read_text(), flags=re.M)
                for section in filter(str.strip, sections):
                    already = subprocess.run(
                        ["patch", "-p1", "--force", "--dry-run", "--reverse"],
                        input=section, text=True, cwd=source,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    if already.returncode == 0:
                        continue
                    subprocess.run(["patch", "-p1", "--batch", "--forward"],
                                   input=section, text=True, cwd=source,
                                   stdout=log, stderr=log, check=True)
        stamp.write_text(json.dumps(base, indent=2) + "\n")
    overlay = copies()
    for original, relative in overlay:
        destination = source / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.read_bytes() != original.read_bytes():
            shutil.copyfile(original, destination)
    inputs = [Path(__file__), versions, *patches, *(p for p, _ in overlay)]
    before = fingerprint(inputs)
    binary_dir.mkdir(exist_ok=True)
    python = os.environ.get("QEMU_PYTHON", "/usr/bin/python3")
    if not (binary_dir / "build.ninja").exists():
        command = [str(source / "configure"), f"--python={python}",
                   f"--prefix={output / 'install'}", "--target-list=riscv64-softmmu",
                   "--disable-docs", "--disable-guest-agent", "--disable-plugins",
                   "--disable-slirp", "--disable-tools", "--disable-werror"]
        with (output / "configure.log").open("w") as log:
            subprocess.run(command, cwd=binary_dir, stdout=log, stderr=subprocess.STDOUT,
                           check=True)
    jobs = jobs or int(os.environ.get("JOBS", min(os.cpu_count() or 1, 16)))
    if jobs < 1:
        raise ValueError("Build jobs must be positive")
    command = ["ninja", "-C", str(binary_dir), "-j", str(jobs), "qemu-system-riscv64"]
    with (output / "build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    if before != fingerprint(inputs):
        raise RuntimeError("QEMU source changed during compilation; rebuild before validating")
    executable = output / "qemu-system-riscv64"
    if executable.is_symlink():
        executable.unlink()
    elif executable.exists():
        raise RuntimeError(f"Refusing to replace existing executable: {executable}")
    executable.symlink_to("build/qemu-system-riscv64")
    (output / "build.json").write_text(json.dumps(
        {"executable": str(executable), "revision": revision,
         "source_sha256": before, "command": command}, indent=2) + "\n")
    return executable


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--jobs", type=int)
    args = parser.parse_args()
    print(build(args.output, args.jobs), flush=True)
