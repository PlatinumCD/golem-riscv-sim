import json
import sys
import tempfile
from pathlib import Path


TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parents[1] / "support"))

from deployment_manifest import load_deployment_manifest


def write_manifest(directory, value):
    path = directory / "deployment-manifest.json"
    path.write_text(json.dumps(value))
    return path


def require_failure(directory, value, expected_message, active=(0, 2)):
    path = write_manifest(directory, value)
    try:
        load_deployment_manifest(
            path, network_size=4, expected_active_tiles=active
        )
    except RuntimeError as error:
        if expected_message not in str(error):
            raise AssertionError(
                f"unexpected diagnostic: {error}"
            ) from error
        return
    raise AssertionError("invalid deployment manifest was accepted")


valid = {
    "schema": "sculptor.deployment",
    "version": 2,
    "active_tile_ids": [0, 2],
    "synchronization": {
        "mode": "bulk_barrier",
        "semantic_epoch_count": 4,
    },
}

with tempfile.TemporaryDirectory(prefix="mittens-deployment-manifest-") as raw:
    directory = Path(raw)
    parsed = load_deployment_manifest(
        write_manifest(directory, valid),
        network_size=4,
        expected_active_tiles=[2, 0],
    )
    assert parsed == {
        "active_tile_ids": [0, 2],
        "synchronization_mode": "bulk_barrier",
        "epoch_count": 4,
    }

    missing = dict(valid)
    del missing["synchronization"]
    require_failure(
        directory,
        missing,
        "missing=['synchronization']",
    )
    require_failure(
        directory,
        valid,
        "active participants disagree",
        active=(0, 1),
    )
    invalid_epoch = dict(valid)
    invalid_epoch["synchronization"] = {
        "mode": "bulk_barrier",
        "semantic_epoch_count": 0,
    }
    require_failure(
        directory,
        invalid_epoch,
        "epoch_count must be a positive integer",
    )
    float_version = dict(valid)
    float_version["version"] = 2.0
    require_failure(
        directory,
        float_version,
        "invalid deployment manifest version",
    )
    oversized_epoch = dict(valid)
    oversized_epoch["synchronization"] = {
        "mode": "bulk_barrier",
        "semantic_epoch_count": 0x100000000,
    }
    require_failure(
        directory,
        oversized_epoch,
        "epoch_count exceeds the uint32 ABI range",
    )
    invalid_mode = dict(valid)
    invalid_mode["synchronization"] = {
        "mode": "guess_from_boundary_ids",
        "semantic_epoch_count": 4,
    }
    require_failure(
        directory,
        invalid_mode,
        "synchronization mode is invalid",
    )

print("deployment manifest validation: PASS")
