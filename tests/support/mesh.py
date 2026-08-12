from decimal import Decimal, InvalidOperation
import re

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
MEMORY_BACKENDS = ("native", "memhierarchy")
MESH_ROUTER_BACKENDS = ("merlin", "mittens")
GUEST_RAM_BASE = 0x80000000
DEFAULT_MEMORY_HIERARCHY = {
    "topology": "private_l1",
    "l1_size": "32KiB",
    "l1_associativity": 4,
    "cache_line_size": 64,
    "l1_access_latency_cycles": 2,
    "l1_max_requests_per_cycle": 1,
    "l1_banks": 1,
    "l1_clock": "1GHz",
    "cpu_l1_latency": "1ns",
    "l1_memory_latency": "1ns",
    "lower_memory_clock": "1GHz",
    "lower_memory_access_time": "50ns",
    "memory_network_bandwidth": "64GB/s",
    "memory_network_buffer_size": "2KiB",
    "memory_network_flit_size": "64B",
    "memory_network_latency": "1ns",
    "l2_banks": 2,
    "l2_slice_size": "512KiB",
    "l2_associativity": 8,
    "l2_access_latency_cycles": 10,
    "l2_clock": "1GHz",
    "directory_entries": 32768,
}

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
_MEMORY_SIZE_PATTERN = re.compile(
    r"^([1-9][0-9]*)\s*([KMGT]?)(i?)[Bb]?$",
    re.IGNORECASE,
)
_MEMORY_SIZE_SCALES = {
    "": 1,
    "k": 1024,
    "m": 1024 ** 2,
    "g": 1024 ** 3,
    "t": 1024 ** 4,
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


def _make_router(x, y, width, height, flit_size, buffer_size,
                 link_bandwidth):
    router_id = tile_id(x, y, width)
    router = sst.Component(f"router_{x}_{y}", "merlin.hr_router")
    router.addParams(
        {
            "id": router_id,
            "num_ports": ROUTER_PORTS,
            "flit_size": flit_size,
            "xbar_bw": link_bandwidth,
            "link_bw": link_bandwidth,
            "input_buf_size": buffer_size,
            "output_buf_size": buffer_size,
        }
    )

    topology = router.setSubComponent("topology", "merlin.mesh")
    topology.addParams(
        {
            "shape": f"{width}x{height}",
            "width": "1x1",
            "local_ports": 1,
        }
    )
    return router


def _make_wormhole_router(x, y, width, height, clock,
                          link_width_bits, input_buffer_flits,
                          pipeline_cycles):
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


def _memory_size_bytes(value):
    if isinstance(value, bool):
        raise ValueError("tile memory size must not be boolean")
    if isinstance(value, int):
        if value <= 0:
            raise ValueError("tile memory size must be positive")
        return value
    if not isinstance(value, str):
        raise ValueError("tile memory size must be an integer or SST size")

    match = _MEMORY_SIZE_PATTERN.fullmatch(value.strip())
    if match is None:
        raise ValueError(f"unsupported tile memory size: {value}")
    return (
        int(match.group(1)) *
        _MEMORY_SIZE_SCALES[match.group(2).lower()]
    )


def _memory_hierarchy_configuration(overrides):
    configuration = dict(DEFAULT_MEMORY_HIERARCHY)
    if overrides is not None:
        configuration.update(overrides)
    if configuration["topology"] not in ("private_l1", "shared_l2"):
        raise ValueError(
            "memory hierarchy topology must be private_l1 or shared_l2"
        )
    if configuration["cache_line_size"] <= 0:
        raise ValueError("cache line size must be positive")
    if configuration["l1_max_requests_per_cycle"] <= 0:
        raise ValueError("L1 requests per cycle must be positive")
    if configuration["l1_banks"] <= 0:
        raise ValueError("L1 bank count must be positive")
    if configuration["l2_banks"] <= 0:
        raise ValueError("L2 bank count must be positive")
    return configuration


def _attach_private_l1(tile, node_id, tile_memory, configuration):
    """Attach one timing-only private L1 and memory path to a tile."""
    memory_bytes = _memory_size_bytes(tile_memory)
    address_end = GUEST_RAM_BASE + memory_bytes - 1
    capacity_mib = (
        address_end + 1 + (1024 ** 2 - 1)
    ) // (1024 ** 2)

    memory_if = tile.setSubComponent(
        "memoryIF", "memHierarchy.standardInterface"
    )

    l1 = sst.Component(
        f"tile{node_id}.private_l1", "memHierarchy.Cache"
    )
    l1.addParams(
        {
            "access_latency_cycles":
                configuration["l1_access_latency_cycles"],
            "cache_frequency": configuration["l1_clock"],
            "replacement_policy": "lru",
            "coherence_protocol": "MESI",
            "associativity": configuration["l1_associativity"],
            "cache_line_size": configuration["cache_line_size"],
            "cache_size": configuration["l1_size"],
            "max_requests_per_cycle":
                configuration["l1_max_requests_per_cycle"],
            "banks": configuration["l1_banks"],
            "L1": 1,
        }
    )

    memory_controller = sst.Component(
        f"tile{node_id}.memory_controller",
        "memHierarchy.MemController",
    )
    memory_controller.addParams(
        {
            "clock": configuration["lower_memory_clock"],
            "backing": "none",
            # Preserve the absolute RISC-V physical address in responses.
            "addr_range_start": 0,
            "addr_range_end": address_end,
        }
    )
    memory = memory_controller.setSubComponent(
        "backend", "memHierarchy.simpleMem"
    )
    memory.addParams(
        {
            "access_time": configuration["lower_memory_access_time"],
            # simpleMem is timing-only and does not allocate this capacity.
            "mem_size": f"{capacity_mib}MiB",
        }
    )

    cpu_l1 = sst.Link(f"tile{node_id}.cpu_l1")
    cpu_l1.connect(
        (memory_if, "lowlink", configuration["cpu_l1_latency"]),
        (l1, "highlink", configuration["cpu_l1_latency"]),
    )
    # Keep private memory accesses within the tile's SST partition. Mesh links
    # remain partition cuts, so independent tile groups can run concurrently.
    cpu_l1.setNoCut()
    l1_memory = sst.Link(f"tile{node_id}.l1_memory")
    l1_memory.connect(
        (l1, "lowlink", configuration["l1_memory_latency"]),
        (memory_controller, "highlink", configuration["l1_memory_latency"]),
    )
    l1_memory.setNoCut()

    l1.enableStatistics(["CacheHits", "CacheMisses"])


def _shared_address_range(network_size, tile_memory):
    tile_bytes = _memory_size_bytes(tile_memory)
    if network_size <= 0 or tile_bytes > (1 << 64) // network_size:
        raise ValueError("shared timing-memory address range overflow")
    return tile_bytes, network_size * tile_bytes


def _memory_nic(component, slot, group, configuration):
    nic = component.setSubComponent(slot, "memHierarchy.MemNIC")
    nic.addParams(
        {
            "group": group,
            "network_bw": configuration["memory_network_bandwidth"],
            "network_input_buffer_size":
                configuration["memory_network_buffer_size"],
            "network_output_buffer_size":
                configuration["memory_network_buffer_size"],
        }
    )
    return nic


def _connect_memory_endpoint(name, endpoint, network, port, configuration):
    link = sst.Link(name)
    latency = configuration["memory_network_latency"]
    link.connect(
        (endpoint, "port", latency),
        (network, f"port{port}", latency),
    )


def _make_shared_l2_fabric(network_size, tile_memory, configuration):
    """Create one logically shared, physically banked L2."""
    tile_bytes, total_bytes = _shared_address_range(
        network_size, tile_memory
    )
    banks = configuration["l2_banks"]
    line_bytes = configuration["cache_line_size"]
    if total_bytes < banks * line_bytes:
        raise ValueError("shared memory is too small for the L2 bank count")

    network = sst.Component("memory_network", "merlin.hr_router")
    network.addParams(
        {
            "id": 0,
            "num_ports": network_size + 3 * banks,
            "flit_size": configuration["memory_network_flit_size"],
            "xbar_bw": configuration["memory_network_bandwidth"],
            "link_bw": configuration["memory_network_bandwidth"],
            "input_buf_size":
                configuration["memory_network_buffer_size"],
            "output_buf_size":
                configuration["memory_network_buffer_size"],
        }
    )
    network.setSubComponent("topology", "merlin.singlerouter")

    capacity_mib = (total_bytes + 1024 ** 2 - 1) // (1024 ** 2)
    interleave_step = banks * line_bytes
    for bank in range(banks):
        start = bank * line_bytes
        end = total_bytes - (banks - bank) * line_bytes + line_bytes - 1

        l2 = sst.Component(
            f"shared_l2.bank{bank}", "memHierarchy.Cache"
        )
        l2.addParams(
            {
                "access_latency_cycles":
                    configuration["l2_access_latency_cycles"],
                "cache_frequency": configuration["l2_clock"],
                "replacement_policy": "lru",
                "coherence_protocol": "MESI",
                "associativity": configuration["l2_associativity"],
                "cache_line_size": line_bytes,
                "cache_size": configuration["l2_slice_size"],
                "num_cache_slices": banks,
                "slice_allocation_policy": "rr",
                "slice_id": bank,
            }
        )
        l2_nic = _memory_nic(l2, "highlink", 2, configuration)

        directory = sst.Component(
            f"shared_l2.directory{bank}",
            "memHierarchy.DirectoryController",
        )
        directory.addParams(
            {
                "clock": configuration["l2_clock"],
                "coherence_protocol": "MESI",
                "entry_cache_size": configuration["directory_entries"],
                "interleave_size": f"{line_bytes}B",
                "interleave_step": f"{interleave_step}B",
                "addr_range_start": start,
                "addr_range_end": end,
            }
        )
        directory_nic = _memory_nic(
            directory, "highlink", 3, configuration
        )

        controller = sst.Component(
            f"shared_l2.memory{bank}", "memHierarchy.MemController"
        )
        controller.addParams(
            {
                "clock": configuration["lower_memory_clock"],
                "backing": "none",
                "interleave_size": f"{line_bytes}B",
                "interleave_step": f"{interleave_step}B",
                "addr_range_start": start,
                "addr_range_end": end,
            }
        )
        controller_nic = _memory_nic(
            controller, "highlink", 4, configuration
        )
        backend = controller.setSubComponent(
            "backend", "memHierarchy.simpleMem"
        )
        backend.addParams(
            {
                "access_time":
                    configuration["lower_memory_access_time"],
                "mem_size": f"{capacity_mib}MiB",
            }
        )

        for offset, endpoint in enumerate(
            (l2_nic, directory_nic, controller_nic)
        ):
            port = network_size + offset * banks + bank
            _connect_memory_endpoint(
                f"memory_network.bank{bank}.group{offset + 2}",
                endpoint,
                network,
                port,
                configuration,
            )
        l2.enableStatistics(["CacheHits", "CacheMisses"])

    return network, tile_bytes


def _attach_shared_l1(tile, node_id, configuration, network):
    memory_if = tile.setSubComponent(
        "memoryIF", "memHierarchy.standardInterface"
    )
    l1 = sst.Component(
        f"tile{node_id}.private_l1", "memHierarchy.Cache"
    )
    l1.addParams(
        {
            "access_latency_cycles":
                configuration["l1_access_latency_cycles"],
            "cache_frequency": configuration["l1_clock"],
            "replacement_policy": "lru",
            "coherence_protocol": "MESI",
            "associativity": configuration["l1_associativity"],
            "cache_line_size": configuration["cache_line_size"],
            "cache_size": configuration["l1_size"],
            "max_requests_per_cycle":
                configuration["l1_max_requests_per_cycle"],
            "banks": configuration["l1_banks"],
            "L1": 1,
        }
    )
    l1_nic = _memory_nic(l1, "lowlink", 1, configuration)
    cpu_l1 = sst.Link(f"tile{node_id}.cpu_l1")
    cpu_l1.connect(
        (memory_if, "lowlink", configuration["cpu_l1_latency"]),
        (l1, "highlink", configuration["cpu_l1_latency"]),
    )
    cpu_l1.setNoCut()
    _connect_memory_endpoint(
        f"tile{node_id}.l1_memory_network",
        l1_nic,
        network,
        node_id,
        configuration,
    )
    l1.enableStatistics(["CacheHits", "CacheMisses"])


def _attach_tile(router, node_id, image, qemu_path, network_size,
                 mesh_width, mesh_height, verbosity, tile_params,
                 buffer_size, link_bandwidth, mesh_link_width_bits,
                 mesh_link_clock, network_packet_words, memory_backend,
                 memory_hierarchy, mesh_router_backend,
                 wormhole_input_buffer_flits,
                 wormhole_injection_buffer_flits,
                 mesh_link_latency,
                 shared_memory_fabric=None, tile_memory_stride=0):
    tile = sst.Component(f"tile{node_id}", "mittens.tile")
    params = {
        "tile_id": node_id,
        "network_size": network_size,
        "mesh_width": mesh_width,
        "mesh_height": mesh_height,
        "qemu_path": qemu_path,
        "elf": image,
        "memory": "16M",
        "launch_mode": "managed",
        "cpu_clock": "1GHz",
        "verbose": verbosity,
    }
    params.update(tile_params)
    # Keep endpoint completion timing identical to the physical Merlin links.
    params["mesh_link_width_bits"] = mesh_link_width_bits
    params["mesh_link_clock"] = mesh_link_clock
    params["network_packet_words"] = network_packet_words
    params["network_tail_delivery"] = (
        mesh_router_backend == "mittens"
    )
    params["memory_backend"] = memory_backend
    if shared_memory_fabric is not None:
        params["memory_guest_base"] = GUEST_RAM_BASE
        params["memory_tile_stride"] = tile_memory_stride
        params["memory_cache_line_size"] = memory_hierarchy[
            "cache_line_size"
        ]
    tile.addParams(params)

    if memory_backend == "memhierarchy":
        if shared_memory_fabric is None:
            _attach_private_l1(
                tile, node_id, params["memory"], memory_hierarchy
            )
        else:
            _attach_shared_l1(
                tile, node_id, memory_hierarchy, shared_memory_fabric
            )

    if mesh_router_backend == "merlin":
        network = tile.setSubComponent(
            "networkIF",
            "merlin.linkcontrol",
        )
        network.addParams(
            {
                "link_bw": link_bandwidth,
                "input_buf_size": buffer_size,
                "output_buf_size": buffer_size,
            }
        )
        network_port = "rtr_port"
    else:
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
            }
        )
        network_port = "router_port"

    link = sst.Link(f"tile{node_id}_local_link")
    link.connect(
        (network, network_port, mesh_link_latency),
        (router, f"port{LOCAL_PORT}", mesh_link_latency),
    )
    link.setNoCut()


def build_mesh(*, width, height, qemu_path, images, statistics_path,
               verbosity=2, tile_params=None, network_cell_words=1,
               network_buffer_cells=16, mesh_link_width_bits=32,
               mesh_link_clock="1GHz", network_packet_words=None,
               mesh_link_latency=LINK_LATENCY,
               mesh_router_backend="merlin",
               wormhole_input_buffer_flits=32,
               wormhole_injection_buffer_flits=64,
               wormhole_pipeline_cycles=3,
               memory_backend="native",
               memory_hierarchy=None, active_tiles=None):
    """Build a mesh whose physical links carry fixed 32-bit words.

    ``mesh_link_width_bits`` controls how many of those words a link can move
    per ``mesh_link_clock`` cycle. Timing cells are padded to a complete
    physical beat; neither setting changes the guest-visible NIC word size.
    ``active_tiles`` limits QEMU-backed endpoints; every physical router and
    cardinal link remains available for transit traffic.
    """
    network_size = width * height
    if memory_backend not in MEMORY_BACKENDS:
        raise ValueError(
            "memory_backend must be native or memhierarchy"
        )
    if mesh_router_backend not in MESH_ROUTER_BACKENDS:
        raise ValueError(
            "mesh_router_backend must be merlin or mittens"
        )
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
    memory_hierarchy = _memory_hierarchy_configuration(memory_hierarchy)
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
    if tile_params is None:
        tile_params = {}
    if network_cell_words <= 0 or network_buffer_cells <= 0:
        raise ValueError(
            "network cell and buffer counts must both be positive"
        )
    buffer_words = network_cell_words * network_buffer_cells
    if network_packet_words is None:
        network_packet_words = buffer_words
    if network_packet_words < 5:
        raise ValueError(
            "network_packet_words must be at least five words"
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
    shared_memory_fabric = None
    tile_memory_stride = 0
    if (memory_backend == "memhierarchy" and
            memory_hierarchy["topology"] == "shared_l2"):
        tile_memory = tile_params.get("memory", "16M")
        shared_memory_fabric, tile_memory_stride = _make_shared_l2_fabric(
            network_size, tile_memory, memory_hierarchy
        )
    if mesh_router_backend == "merlin":
        routers = {
            (x, y): _make_router(
                x,
                y,
                width,
                height,
                flit_size,
                buffer_size,
                link_bandwidth,
            )
            for y in range(height)
            for x in range(width)
        }
    else:
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
            )
            for y in range(height)
            for x in range(width)
        }

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
            _attach_tile(
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
                memory_hierarchy,
                mesh_router_backend,
                wormhole_input_buffer_flits,
                wormhole_injection_buffer_flits,
                mesh_link_latency,
                shared_memory_fabric,
                tile_memory_stride,
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
