from decimal import Decimal, InvalidOperation
import re
import os
from pathlib import Path

import sst


EAST_PORT = 0
WEST_PORT = 1
SOUTH_PORT = 2
NORTH_PORT = 3
LOCAL_PORT = 4
ROUTER_PORTS = 5

LINK_LATENCY = "10ns"
WORD_BYTES = 4
WORD_BITS = WORD_BYTES * 8
MESH_ROUTER_BACKENDS = ("mittens",)

_FREQUENCY_PATTERN = re.compile(
    r"^([0-9]+(?:\.[0-9]+)?)\s*(Hz|kHz|MHz|GHz)$",
    re.IGNORECASE,
)
_FREQUENCY_SCALES = {
    "hz": Decimal(1),
    "khz": Decimal(1_000),
    "mhz": Decimal(1_000_000),
    "ghz": Decimal(1_000_000_000),
}


def tile_id(x, y, width):
    return y * width + x


def _mesh_link_configuration(width_bits, clock, cell_words, buffer_cells):
    if (isinstance(width_bits, bool) or
            not isinstance(width_bits, int) or
            width_bits <= 0 or
            width_bits % WORD_BITS != 0):
        raise ValueError(
            "mesh link width must be a positive multiple of 32 bits"
        )
    if not isinstance(clock, str):
        raise ValueError("mesh link clock must be an SST frequency string")

    match = _FREQUENCY_PATTERN.fullmatch(clock.strip())
    if match is None:
        raise ValueError(
            "mesh link clock must use Hz, kHz, MHz, or GHz"
        )
    try:
        frequency_hz = (
            Decimal(match.group(1)) *
            _FREQUENCY_SCALES[match.group(2).lower()]
        )
    except InvalidOperation as error:
        raise ValueError("invalid mesh link clock") from error
    if frequency_hz <= 0:
        raise ValueError("mesh link clock must be positive")

    bytes_per_second = frequency_hz * Decimal(width_bits) / Decimal(8)
    if bytes_per_second != bytes_per_second.to_integral_value():
        raise ValueError(
            "mesh width and clock must produce an integral byte rate"
        )

    link_bytes = width_bits // 8
    useful_cell_bytes = cell_words * WORD_BYTES
    flit_bytes = (
        (useful_cell_bytes + link_bytes - 1) // link_bytes
    ) * link_bytes
    return (
        f"{int(bytes_per_second)}B/s",
        f"{flit_bytes}B",
        f"{flit_bytes * buffer_cells}B",
    )


def _make_wormhole_router(x, y, width, height, clock,
                          link_width_bits, input_buffer_flits,
                          pipeline_cycles, tx_streams, rx_streams=1):
    router_id = tile_id(x, y, width)
    router = sst.Component(
        f"router_{x}_{y}",
        "mittens.wormholeRouter",
    )
    router.addParams(
        {
            "id": router_id,
            "mesh_width": width,
            "mesh_height": height,
            "clock": clock,
            "link_width_bits": link_width_bits,
            "input_buffer_flits": input_buffer_flits,
            "pipeline_cycles": pipeline_cycles,
            "tx_streams": tx_streams,
            "rx_streams": rx_streams,
        }
    )
    return router


def _connect_routers(name, first, first_port, second, second_port,
                     latency=LINK_LATENCY):
    link = sst.Link(name)
    link.connect(
        (first, f"port{first_port}", latency),
        (second, f"port{second_port}", latency),
    )


def _attach_tile(router, node_id, image, qemu_path, network_size,
                 mesh_width, mesh_height, verbosity, tile_params,
                 buffer_size, link_bandwidth, mesh_link_width_bits,
                 mesh_link_clock, network_packet_words, memory_backend,
                 mesh_router_backend,
                 wormhole_input_buffer_flits,
                 wormhole_injection_buffer_flits,
                 mesh_link_latency):
    tile = sst.Component(f"tile{node_id}", "mittens.tile")
    params = {
        "tile_id": node_id,
        "network_size": network_size,
        "mesh_width": mesh_width,
        "mesh_height": mesh_height,
        "qemu_path": qemu_path,
        "elf": image,
        "memory": tile_params.get(
            "qemu_control_memory", tile_params.get("memory", "16M")
        ),
        "launch_mode": "managed",
        "serial_output_directory": tile_params.get(
            "serial_output_directory", ""
        ),
        "cpu_clock": "1GHz",
        "verbose": verbosity,
    }
    params.update(
        {
            key: value for key, value in tile_params.items()
            if key not in ("qemu_control_memory", "memory")
        }
    )
    # Keep endpoint completion timing identical to the physical mesh links.
    params["mesh_link_width_bits"] = mesh_link_width_bits
    params["mesh_link_clock"] = mesh_link_clock
    params["network_packet_words"] = network_packet_words
    params["network_tail_delivery"] = (
        mesh_router_backend == "mittens"
    )
    params["memory_backend"] = memory_backend
    tile.addParams(params)

    network = tile.setSubComponent(
        "networkIF",
        "mittens.wormholeNIC",
    )
    network.addParams(
        {
            "endpoint_id": node_id,
            "network_size": network_size,
            "clock": mesh_link_clock,
            "link_width_bits": mesh_link_width_bits,
            "router_buffer_flits":
                wormhole_input_buffer_flits,
            "injection_buffer_flits":
                wormhole_injection_buffer_flits,
            "mesh_width": mesh_width,
            "mesh_height": mesh_height,
            "tx_streams": tile_params.get("tx_dma_streams", 1),
            "rx_streams": tile_params.get("rx_dma_streams", 1),
        }
    )
    network_port = "router_port0"
    if os.environ.get("MITTENS_NIC_RECEIVE_TRACE_DIR"):
        network.addParam("receive_flit_trace_path", str(Path(os.environ["MITTENS_NIC_RECEIVE_TRACE_DIR"]) / f"nic-{node_id}-flits.csv"))

    local_lanes = (max(tile_params.get("tx_dma_streams", 1), tile_params.get("rx_dma_streams", 1)))
    for lane in range(local_lanes):
        local_network_port = (f"router_port{lane}")
        local_router_port = (f"port{LOCAL_PORT + lane}")
        link = sst.Link(f"tile{node_id}_local_link_{lane}")
        link.connect(
            (network, local_network_port, mesh_link_latency),
            (router, local_router_port, mesh_link_latency),
        )
        link.setNoCut()
    return tile


def build_mesh(*, width, height, qemu_path, images, statistics_path,
               verbosity=2, tile_params=None, network_cell_words=1,
               network_buffer_cells=16, mesh_link_width_bits=32,
               mesh_link_clock="1GHz", network_packet_words=None,
               mesh_link_latency=LINK_LATENCY,
               mesh_router_backend="mittens",
               wormhole_input_buffer_flits=32,
               wormhole_injection_buffer_flits=64,
               wormhole_pipeline_cycles=3,
               memory_backend="streaming",
               memory_hierarchy=None, active_tiles=None,
               global_memory=None, initialization_barrier=None,
               epoch_barrier=None,
               partition_global_dma=False, sst_work_partition=None):
    """Build a mesh whose physical links carry fixed 32-bit words.

    ``mesh_link_width_bits`` controls how many of those words a link can move
    per ``mesh_link_clock`` cycle. Timing cells are padded to a complete
    physical beat; neither setting changes the guest-visible NIC word size.
    ``active_tiles`` limits QEMU-backed endpoints; every physical router and
    cardinal link remains available for transit traffic.
    """
    network_size = width * height
    if memory_backend != "streaming":
        raise ValueError(
            "only executable SPM with streaming DMA is supported"
        )
    if mesh_router_backend not in MESH_ROUTER_BACKENDS:
        raise ValueError(
            "the supported mesh router is mittens"
        )
    if not isinstance(partition_global_dma, bool):
        raise ValueError("partition_global_dma must be boolean")
    if not isinstance(mesh_link_latency, str) or not mesh_link_latency:
        raise ValueError("mesh_link_latency must be an SST time string")
    if (
        wormhole_input_buffer_flits <= 0 or
        wormhole_injection_buffer_flits <= 0 or
        wormhole_pipeline_cycles <= 0
    ):
        raise ValueError(
            "wormhole router buffer and pipeline values must be positive"
        )
    if memory_hierarchy is not None:
        raise ValueError("scratchpad boot does not use an L1/L2 data hierarchy")
    if len(images) != network_size:
        raise ValueError(
            f"mesh requires {network_size} images, received {len(images)}"
        )
    if active_tiles is None:
        active_tile_set = set(range(network_size))
    else:
        active_tile_list = list(active_tiles)
        if not active_tile_list:
            raise ValueError("active_tiles must contain at least one tile ID")
        if any(
            isinstance(tile, bool) or not isinstance(tile, int)
            for tile in active_tile_list
        ):
            raise ValueError("active tile IDs must be integers")
        active_tile_set = set(active_tile_list)
        if len(active_tile_set) != len(active_tile_list):
            raise ValueError("active tile IDs must not contain duplicates")
        if any(tile < 0 or tile >= network_size for tile in active_tile_set):
            raise ValueError("active tile IDs must be within the mesh")
    work_partition_threads = None
    work_partition_controller_thread = None
    if sst_work_partition is not None:
        if memory_backend != "streaming":
            raise ValueError(
                "SST workload partitioning currently requires streaming memory"
            )
        if not isinstance(sst_work_partition, dict):
            raise ValueError("SST workload partition must be an object")
        if sst_work_partition.get("schema") != (
            "golem.sculptor-sst-work-partition"
        ) or sst_work_partition.get("schema_version") != 1:
            raise ValueError("invalid SST workload partition schema")
        if sst_work_partition.get("mesh") != {
            "rows": height,
            "columns": width,
        }:
            raise ValueError("SST workload partition mesh mismatch")
        if sst_work_partition.get("thread_count") != sst.getThreadCount():
            raise ValueError("SST workload partition thread-count mismatch")
        if sst_work_partition.get("active_tile_ids") != sorted(
            active_tile_set
        ):
            raise ValueError("SST workload partition active-tile mismatch")
        work_partition_threads = sst_work_partition.get("mesh_threads")
        if (
            not isinstance(work_partition_threads, list)
            or len(work_partition_threads) != network_size
            or any(
                isinstance(thread, bool)
                or not isinstance(thread, int)
                or thread < 0
                or thread >= sst.getThreadCount()
                for thread in work_partition_threads
            )
        ):
            raise ValueError("SST workload partition has invalid mesh threads")
        work_partition_controller_thread = sst_work_partition.get(
            "controller_thread"
        )
        if (
            isinstance(work_partition_controller_thread, bool)
            or not isinstance(work_partition_controller_thread, int)
            or work_partition_controller_thread < 0
            or work_partition_controller_thread >= sst.getThreadCount()
        ):
            raise ValueError(
                "SST workload partition has an invalid controller thread"
            )
    if tile_params is None:
        tile_params = {}
    else:
        tile_params = dict(tile_params)
    tile_params.setdefault("scratchpad_boot", True)
    if tile_params["scratchpad_boot"] is not True or tile_params.get("scratchpad_enabled", True) is not True:
        raise ValueError("executable SPM cannot be disabled")
    if tile_params["scratchpad_boot"]:
        if memory_backend != "streaming":
            raise ValueError("scratchpad boot requires streaming memory, without an L1/L2 data hierarchy")
        tile_params.setdefault("scratchpad_enabled", True)
        tile_params.setdefault("scratchpad_bytes", 256 * 1024)
    rx_streams = tile_params.get("rx_dma_streams", 1)
    if isinstance(rx_streams, bool) or rx_streams not in (1, 2, 4):
        raise ValueError("rx_dma_streams must be 1, 2, or 4")
    if rx_streams > 1 and mesh_router_backend != "mittens":
        raise ValueError("multiple RX streams require the mittens router and NIC")
    streaming_memory = memory_backend == "streaming"
    if initialization_barrier is not None or tile_params.get("memory_init_barrier_tiles", 0):
        raise ValueError("SPM boot uses timed DMA, not an initialization barrier")
    epoch_configuration = None
    if epoch_barrier is not None:
        if not streaming_memory:
            raise ValueError(
                "epoch_barrier requires memory_backend='streaming'"
            )
        if not isinstance(epoch_barrier, dict):
            raise ValueError("epoch_barrier must be a configuration mapping")
        unsupported = set(epoch_barrier) - {
            "epoch_count", "clock", "release_cycles", "stop_after_releases",
            "verbose"
        }
        if unsupported:
            raise ValueError(
                "unsupported epoch_barrier settings: " +
                ", ".join(sorted(unsupported))
            )
        epoch_count = epoch_barrier.get("epoch_count")
        if (
            isinstance(epoch_count, bool) or
            not isinstance(epoch_count, int) or
            epoch_count <= 0
        ):
            raise ValueError(
                "epoch_barrier epoch_count must be a positive integer"
            )
        if epoch_count > 0xFFFFFFFF:
            raise ValueError(
                "epoch_barrier epoch_count exceeds the uint32 ABI range"
            )
        release_cycles = epoch_barrier.get("release_cycles", 1)
        if (
            isinstance(release_cycles, bool) or
            not isinstance(release_cycles, int) or
            release_cycles <= 0
        ):
            raise ValueError(
                "epoch_barrier release_cycles must be a positive integer"
            )
        stop_after_releases = epoch_barrier.get("stop_after_releases", 0)
        if (
            isinstance(stop_after_releases, bool) or
            not isinstance(stop_after_releases, int) or
            stop_after_releases < 0 or
            stop_after_releases > epoch_count
        ):
            raise ValueError(
                "epoch_barrier stop_after_releases must be between zero and "
                "epoch_count"
            )
        epoch_configuration = {
            "tile_count": network_size,
            "active_tiles": sorted(active_tile_set),
            "epoch_count": epoch_count,
            "clock": epoch_barrier.get("clock", "1GHz"),
            "release_cycles": release_cycles,
            "stop_after_releases": stop_after_releases,
            "verbose": epoch_barrier.get("verbose", verbosity),
        }
        configured_epochs = tile_params.get("epoch_barrier_epochs")
        if configured_epochs not in (None, epoch_count):
            raise ValueError(
                "tile epoch_barrier_epochs disagrees with the controller"
            )
        tile_params["epoch_barrier_epochs"] = epoch_count
    elif tile_params.get("epoch_barrier_epochs", 0) != 0:
        raise ValueError(
            "tile epoch_barrier_epochs requires an epoch_barrier controller"
        )
    if streaming_memory:
        # A DMA request link has no reverse admission/credit message: once a
        # tile submits an event, the controller must be able to retain it.
        # The QEMU endpoint admits at most eight outstanding DMA jobs, so
        # dimension the controller for that exact finite endpoint contract.
        # A smaller per-tile window or fixed total depth rejects legal bursts.
        global_configuration = {
            "capacity_bytes": 32 * 1024 ** 3,
            "clock": "1GHz",
            "channels": 1,
            "queue_depth": 16,
            "per_tile_queue_depth": 8,
            "setup_cycles": 8,
            "bytes_per_cycle": 32,
            "burst_bytes": 64,
            "fixed_latency_cycles": 2,
        }
        explicit_queue_depth = (
            global_memory is not None and "queue_depth" in global_memory
        )
        if global_memory is not None:
            global_configuration.update(global_memory)
        global_configuration.setdefault("dependency_mode", "bulk_barrier")
        if not explicit_queue_depth:
            global_configuration["queue_depth"] = max(
                global_configuration["queue_depth"],
                len(active_tile_set) *
                global_configuration["per_tile_queue_depth"],
            )
        tile_params.setdefault("scratchpad_enabled", True)
        tile_params.setdefault("scratchpad_bytes", 256 * 1024)
        tile_params["global_ram_bytes"] = global_configuration[
            "capacity_bytes"
        ]
    elif global_memory is not None:
        raise ValueError(
            "global_memory requires memory_backend='streaming'"
        )
    if network_cell_words <= 0 or network_buffer_cells <= 0:
        raise ValueError(
            "network cell and buffer counts must both be positive"
        )
    buffer_words = network_cell_words * network_buffer_cells
    if network_packet_words is None:
        network_packet_words = buffer_words
    if network_packet_words < 7:
        raise ValueError(
            "network_packet_words must be at least seven words"
        )
    if network_packet_words > buffer_words:
        raise ValueError(
            "network_packet_words must not exceed the network buffer capacity"
        )
    if (
        mesh_router_backend == "mittens" and
        network_packet_words > wormhole_input_buffer_flits
    ):
        raise ValueError(
            "network_packet_words must not exceed the wormhole input buffer"
        )
    if (
        mesh_router_backend == "mittens" and
        network_packet_words > wormhole_injection_buffer_flits
    ):
        raise ValueError(
            "network_packet_words must not exceed the wormhole injection buffer"
        )
    link_bandwidth, flit_size, buffer_size = _mesh_link_configuration(
        mesh_link_width_bits,
        mesh_link_clock,
        network_cell_words,
        network_buffer_cells,
    )
    global_controller = None
    if streaming_memory:
        global_controller = sst.Component(
            "global_ram", "mittens.globalRAMController"
        )
        controller_params = dict(global_configuration)
        controller_params["tile_count"] = network_size
        controller_params["active_tiles"] = sorted(active_tile_set)
        if tile_params.get("profile_mode", "off") != "off":
            controller_params["profile_output_directory"] = (
                tile_params.get("profile_output_directory", "")
            )
            controller_params["progress_snapshot_interval_ms"] = (
                tile_params.get("progress_snapshot_interval_ms", 10000)
            )
        global_controller.addParams(controller_params)
        if work_partition_threads is not None:
            global_controller.setRank(0, work_partition_controller_thread)
    epoch_controller = None
    if epoch_configuration is not None:
        epoch_controller = sst.Component(
            "epoch_barrier", "mittens.epochBarrierController"
        )
        epoch_controller.addParams(epoch_configuration)
        if work_partition_threads is not None:
            epoch_controller.setRank(0, work_partition_controller_thread)
    routers = {
        (x, y): _make_wormhole_router(
            x,
            y,
            width,
            height,
            mesh_link_clock,
            mesh_link_width_bits,
            wormhole_input_buffer_flits,
            wormhole_pipeline_cycles,
            tile_params.get("tx_dma_streams", 1),
            tile_params.get("rx_dma_streams", 1),
        )
        for y in range(height)
        for x in range(width)
    }
    if work_partition_threads is not None:
        for (x, y), router in routers.items():
            router.setRank(0, work_partition_threads[tile_id(x, y, width)])

    for y in range(height):
        for x in range(width):
            router = routers[x, y]

            if x + 1 < width:
                _connect_routers(
                    f"horizontal_{x}_{y}_to_{x + 1}_{y}",
                    router,
                    EAST_PORT,
                    routers[x + 1, y],
                    WEST_PORT,
                    mesh_link_latency,
                )

            if y + 1 < height:
                _connect_routers(
                    f"vertical_{x}_{y}_to_{x}_{y + 1}",
                    router,
                    SOUTH_PORT,
                    routers[x, y + 1],
                    NORTH_PORT,
                    mesh_link_latency,
                )

            node_id = tile_id(x, y, width)
            # Keep the physical router in the mesh, but do not launch a QEMU
            # guest for an idle tile.  This preserves routing while avoiding
            # one large guest-memory allocation for every physical tile.
            if node_id not in active_tile_set:
                continue
            tile = _attach_tile(
                router,
                node_id,
                images[node_id],
                qemu_path,
                network_size,
                width,
                height,
                verbosity,
                tile_params,
                buffer_size,
                link_bandwidth,
                mesh_link_width_bits,
                mesh_link_clock,
                network_packet_words,
                memory_backend,
                mesh_router_backend,
                wormhole_input_buffer_flits,
                wormhole_injection_buffer_flits,
                mesh_link_latency,
            )
            if work_partition_threads is not None:
                tile.setRank(0, work_partition_threads[node_id])
            if global_controller is not None:
                dma_link = sst.Link(f"tile{node_id}.global_dma")
                dma_link.connect(
                    (tile, "globalDMA", "1ns"),
                    (global_controller, f"dma{node_id}", "1ns"),
                )
                # Controller affinity is the single-thread default. A
                # multithreaded simulation may deliberately expose these
                # links as partition cuts; otherwise the no-cut star
                # transitively collapses every active tile and the controller
                # into one indivisible SST partition.
                if not partition_global_dma:
                    dma_link.setNoCut()
            if epoch_controller is not None:
                barrier_link = sst.Link(f"tile{node_id}.epoch_barrier")
                barrier_link.connect(
                    (tile, "epochBarrier", "1ns"),
                    (epoch_controller, f"barrier{node_id}", "1ns"),
                )

    sst.setStatisticLoadLevel(1)
    sst.setStatisticOutput(
        "sst.statOutputCSV",
        {"filepath": statistics_path, "separator": ","},
    )
    sst.enableAllStatisticsForAllComponents(
        {"type": "sst.AccumulatorStatistic", "rate": "0ns"}
    )

    return routers
