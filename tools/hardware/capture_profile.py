"""Validated host capture configuration for deployment runs on this host.

Keep this opt-in at the launcher level: it does not change hardware-test defaults.
The QEMU and SST element builds must implement the same bridge protocol.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INSTALL = ROOT / "install/fetch-segments"
ELEMENT_LIBRARY = ROOT / "install/host-capture-fast/sst-elements/lib/sst-elements-library"
CAPTURE_WORKERS = 16


def binaries(install=INSTALL, element_library=ELEMENT_LIBRARY):
    paths = {
        "sst": Path(install).resolve() / "sst-core/bin/sst",
        "qemu": Path(install).resolve() / "qemu/bin/qemu-system-riscv64",
        "element": Path(element_library).resolve() / "libmittens.so",
    }
    for path in paths.values():
        if not path.is_file():
            raise FileNotFoundError(f"Required simulator build is missing: {path}; no fallback permitted")
    return paths
