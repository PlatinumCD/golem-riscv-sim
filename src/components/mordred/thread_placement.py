"""Keep each tile's shared-memory components on one SST worker."""
from collections.abc import Sequence
import os


def partition_tiles(x_dim, y_dim, threads):
    """Recursively split spatial rectangles; IDs are row-major physical tiles."""
    for name, value in (("x_dim", x_dim), ("y_dim", y_dim), ("threads", threads)):
        if type(value) is not int or value < 1:
            raise ValueError(f"{name} must be a positive integer")
    if threads > x_dim * y_dim:
        raise ValueError("SST thread count must not exceed the number of tiles")
    rectangles = [(0, 0, x_dim, y_dim)]
    while len(rectangles) < threads:
        index = max(range(len(rectangles)), key=lambda i: rectangles[i][2] * rectangles[i][3])
        x, y, width, height = rectangles.pop(index)
        if width >= height and width > 1:
            cut = width // 2
            rectangles.extend(((x, y, cut, height), (x + cut, y, width - cut, height)))
        else:
            cut = height // 2
            rectangles.extend(((x, y, width, cut), (x, y + cut, width, height - cut)))
    placement = [0] * (x_dim * y_dim)
    for worker, (x, y, width, height) in enumerate(sorted(rectangles, key=lambda r: (r[1], r[0]))):
        for row in range(y, y + height):
            for column in range(x, x + width):
                placement[row * x_dim + column] = worker
    return tuple(placement)


def resolve_threads(sst, mesh, tile_threads=None):
    """Validate before component construction or backing-file mutation.

    Configuration-only test doubles can omit SST's execution-count API; they
    then describe the ordinary single-thread graph.
    """
    ranks = getattr(sst, "getMPIRankCount", lambda: 1)()
    if ranks != 1:
        raise ValueError("shared-SPM tile meshes require one SST MPI rank")
    threads = getattr(sst, "getThreadCount", lambda: 1)()
    automatic = partition_tiles(mesh.x_dim, mesh.y_dim, threads)
    if threads > 1 and "TILE_COMPONENT_TRACE_START_TASK" in os.environ:
        raise ValueError("threaded tile meshes require full-lifetime observation scope")
    if tile_threads is None:
        return threads, automatic
    if (not isinstance(tile_threads, Sequence) or isinstance(tile_threads, (str, bytes))
            or len(tile_threads) != mesh.endpoint_count
            or any(type(worker) is not int or not 0 <= worker < threads for worker in tile_threads)):
        raise ValueError("tile_threads must contain one valid SST worker ID per row-major tile")
    return threads, tuple(tile_threads)


def place_tiles(sst, tiles, routers, workers, name):
    """Only inter-router links cross workers; subcomponents inherit placement."""
    sst.setProgramOption("partitioner", "sst.self")
    for identity, (tile, router, worker) in enumerate(zip(tiles, routers, workers)):
        connections = sst.findComponentByName(f"{name}.tile{identity}.spm_connections")
        if connections is None:
            raise ValueError(f"missing SPM connections for tile {identity}")
        components = [tile[key] for key in
                      ("cpu", "arrays", "scratchpad", "router_spm", "dram_controller")
                      if tile.get(key) is not None]
        for component in (*components, connections, router):
            component.setRank(0, worker)
        # The CPU rejects threaded configurations that omit explicit placement.
        tile["cpu"].addParams(dict(sst_tile_thread=worker))
