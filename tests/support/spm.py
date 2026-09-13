"""Connect executable-SPM tiles to one shared boot/data DMA controller."""
import sst


def attach_memory(tiles, *, capacity_bytes=32 * 1024**3, **parameters):
    """`tiles` maps physical tile IDs to components; all share the same RAM.

    Use this for standalone component fixtures. Mesh tests use build_mesh,
    which also creates the router links. Call only once per simulation.
    """
    if not tiles or any(not isinstance(i, int) or i < 0 for i in tiles):
        raise ValueError("provide nonnegative tile IDs and at least one tile")
    controller = sst.Component("global_ram", "mittens.globalRAMController")
    config = {
        "tile_count": max(tiles) + 1, "active_tiles": sorted(tiles),
        "capacity_bytes": capacity_bytes, "dependency_mode": "bulk_barrier",
        "per_tile_queue_depth": 8, "queue_depth": max(16, 8 * len(tiles)),
    }
    config.update(parameters)
    controller.addParams(config)
    for tile_id, tile in tiles.items():
        tile.addParams({"global_ram_bytes": capacity_bytes})
        link = sst.Link(f"tile{tile_id}.global_dma")
        link.connect((tile, "globalDMA", "1ns"),
                     (controller, f"dma{tile_id}", "1ns"))
        link.setNoCut()
    return controller
