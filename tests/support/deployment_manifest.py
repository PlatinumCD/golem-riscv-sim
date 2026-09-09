import json
from pathlib import Path


_TOP_LEVEL_FIELDS = {
    "schema", "version", "active_tile_ids", "synchronization"
}


def _describe_field_mismatch(actual, expected):
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    return f"missing={missing}, unexpected={unexpected}"


def load_deployment_manifest(path, *, network_size, expected_active_tiles):
    """Load the authoritative pre-launch deployment synchronization contract."""
    manifest_path = Path(path)
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(
            f"cannot read deployment manifest {manifest_path}: {error}"
        ) from error

    if not isinstance(manifest, dict):
        raise RuntimeError("deployment manifest must be a JSON object")
    actual_fields = set(manifest)
    if actual_fields != _TOP_LEVEL_FIELDS:
        raise RuntimeError(
            "deployment manifest fields are invalid: " +
            _describe_field_mismatch(actual_fields, _TOP_LEVEL_FIELDS)
        )
    if manifest["schema"] != "sculptor.deployment":
        raise RuntimeError("invalid deployment manifest schema")
    if not isinstance(manifest["version"], int) or isinstance(
        manifest["version"], bool
    ) or manifest["version"] != 2:
        raise RuntimeError("invalid deployment manifest version")
    if (
        isinstance(network_size, bool) or
        not isinstance(network_size, int) or
        network_size <= 0
    ):
        raise ValueError("network_size must be a positive integer")

    active_tiles = manifest["active_tile_ids"]
    if not isinstance(active_tiles, list) or not active_tiles:
        raise RuntimeError(
            "deployment manifest active_tile_ids must be a nonempty array"
        )
    if any(
        isinstance(tile, bool) or not isinstance(tile, int)
        for tile in active_tiles
    ):
        raise RuntimeError(
            "deployment manifest active_tile_ids must contain integers"
        )
    if len(set(active_tiles)) != len(active_tiles):
        raise RuntimeError(
            "deployment manifest active_tile_ids contains duplicates"
        )
    if any(tile < 0 or tile >= network_size for tile in active_tiles):
        raise RuntimeError(
            "deployment manifest active tile is outside the configured mesh"
        )

    expected = list(expected_active_tiles)
    if any(
        isinstance(tile, bool) or not isinstance(tile, int)
        for tile in expected
    ) or len(set(expected)) != len(expected):
        raise ValueError(
            "expected_active_tiles must contain unique integer tile IDs"
        )
    if set(active_tiles) != set(expected):
        raise RuntimeError(
            "deployment manifest active participants disagree with the "
            f"numeric active-core manifest: deployment={sorted(active_tiles)} "
            f"numeric={sorted(expected)}"
        )

    synchronization = manifest["synchronization"]
    if not isinstance(synchronization, dict):
        raise RuntimeError(
            "deployment manifest synchronization must be a JSON object"
        )
    synchronization_fields = {"mode", "semantic_epoch_count"}
    if set(synchronization) != synchronization_fields:
        raise RuntimeError(
            "deployment manifest synchronization fields are invalid: " +
            _describe_field_mismatch(
                set(synchronization), synchronization_fields
            )
        )
    mode = synchronization["mode"]
    if mode not in {"exact_dependencies", "bulk_barrier"}:
        raise RuntimeError(
            "deployment manifest synchronization mode is invalid"
        )
    epoch_count = synchronization["semantic_epoch_count"]
    if (
        isinstance(epoch_count, bool) or
        not isinstance(epoch_count, int) or
        epoch_count <= 0
    ):
        raise RuntimeError(
            "deployment manifest epoch_count must be a positive integer"
        )
    if epoch_count > 0xFFFFFFFF:
        raise RuntimeError(
            "deployment manifest epoch_count exceeds the uint32 ABI range"
        )

    return {
        "active_tile_ids": list(active_tiles),
        "synchronization_mode": mode,
        "epoch_count": epoch_count,
    }
