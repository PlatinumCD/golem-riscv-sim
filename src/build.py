"""Build the current SST components, with optional correctness-test fixtures."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def load_build_info(path, *, with_tests=False, extra_sources=()):
    """Reject missing, stale or incomplete builds before starting a simulation."""
    info = json.loads(Path(path).read_text())
    for name, digest in info["source_sha256"].items():
        source = Path(name)
        if not source.is_file() or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"Build input changed: {source}; rebuild first")
    expected = [Path(p).resolve() for p in extra_sources]
    if with_tests:
        expected.append(HERE / "tests/driver.cc")
    for source in expected:
        if str(source) not in info["command"]:
            raise RuntimeError(f"Build does not include required fixture: {source}")
    for name, digest in info.get("artifact_sha256", {}).items():
        artifact = Path(name)
        if not artifact.is_file() or hashlib.sha256(artifact.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"Built artifact changed: {artifact}")
    if not (Path(info["plugin"]) / "libtilecomponents.so").is_file():
        raise RuntimeError("Component library is missing; rebuild first")
    return info


def build(output, with_tests=False, extra_sources=()):
    output = Path(output).resolve()
    if output in (ROOT, ROOT / "build", ROOT / "install", Path("/")) or output.is_relative_to(HERE):
        raise ValueError("Build output must be a dedicated directory outside the source tree")
    output.mkdir(parents=True, exist_ok=True)
    core = Path(os.environ.get("SST_CORE_ROOT", ROOT / "install/sst-core"))
    elements = Path(os.environ.get("SST_ELEMENTS_ROOT", ROOT / "install/sst-elements"))
    upstream = ROOT / "third_party/sst-elements/src/sst/elements/memHierarchy"
    spec = importlib.util.spec_from_file_location("prepare_controller", HERE / "components/scratchpad/prepare_controller.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.prepare(upstream, output / "generated")
    spec = importlib.util.spec_from_file_location("prepare_connections", HERE / "components/scratchpad/prepare_connections.py")
    connections = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(connections)
    routing_source, routing_inputs = connections.connections(output / "profiled-connections")
    config = core / "bin/sst-config"
    compiler = shlex.split(subprocess.check_output([str(config), "--CXX"], text=True))
    flags = shlex.split(subprocess.check_output([str(config), "--ELEMENT_CXXFLAGS"], text=True))
    library = elements / "lib/sst-elements-library"
    sources = [output / "generated/scratchpad.cc", routing_source, HERE / "components/scratchpad/bankedBackend.cc",
               HERE / "components/scratchpad/exactConvertor.cc", HERE / "components/analog-arrays/analogArrays.cc"]
    cpu = HERE / "components/riscv-qemu"
    sources.append(HERE / "components/mordred/spmEndpoint.cc")
    sources += [cpu / name for name in ("riscvQemu.cc", "loadStoreQueue.cc", "analogCommandQueue.cc", "instructionCache.cc", "qemuProcess.cc",
                "sharedSyncMemoryBridge.cc", "scratchpadBootImage.cc")]
    if with_tests:
        sources.append(HERE / "tests/driver.cc")
    extra_sources = [Path(p).resolve() for p in extra_sources]
    sources.extend(extra_sources)
    # Compiled artifacts depend on implementation and build inputs. Guest
    # programs and Python test scripts are fingerprinted by the test runner;
    # changing one must not invalidate an otherwise identical shared library.
    inputs = sorted(p for p in (HERE / "components").rglob("*") if p.suffix in (".h", ".cc"))
    inputs += [Path(__file__), HERE / "components/scratchpad/prepare_controller.py",
               HERE / "components/mordred/build.py", HERE / "components/mordred/UPSTREAM.json",
               upstream / "scratchpad.h", upstream / "scratchpad.cc", *sources]
    inputs += [header for source in extra_sources for header in source.parent.rglob("*.h")]
    inputs += sorted((HERE / "components/mordred/patches").glob("*.patch"))
    inputs += [HERE / "components/scratchpad/prepare_connections.py", *routing_inputs,
               routing_source.with_suffix(".h")]
    before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    command = compiler + flags + ["-O2", "-Wall", "-Wextra", "-shared", "-pthread", f"-I{elements / 'include'}",
        f"-I{cpu}", f"-I{HERE / 'components/scratchpad'}",
        f"-I{upstream}", f"-I{ROOT / 'third_party/sst-elements/src'}",
        f"-I{core / 'include/sst/core'}", *map(str, sources),
        f"-L{library}", f"-Wl,-rpath,{library}", "-lmemHierarchy", "-o", str(output / "libtilecomponents.so")]
    with (output / "build.log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
    if before != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}:
        raise RuntimeError("Source changed during compilation; rebuild before validating")
    spec = importlib.util.spec_from_file_location("build_mordred", HERE / "components/mordred/build.py")
    mordred = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mordred)
    network = mordred.build(output, core=core)
    info = dict(sst=str(core / "bin/sst"), library=str(library), plugin=str(output), command=command,
                source_sha256=before, mordred=network,
                artifact_sha256={str(output / name): hashlib.sha256((output / name).read_bytes()).hexdigest()
                                 for name in ("libtilecomponents.so", "libmordred.so")})
    (output / "build.json").write_text(json.dumps(info, indent=2) + "\n")
    return info


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/src/components")
    args = parser.parse_args()
    build(args.output)
    print(args.output.resolve() / "libtilecomponents.so")
