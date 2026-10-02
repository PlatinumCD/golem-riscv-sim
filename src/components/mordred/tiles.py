"""Compose complete QEMU/SPM/array tiles on an explicitly wired Mordred mesh.

Each router interface reaches its local SPM only through StandardMem and the banked memory
controller. Its SimpleNetwork interface is connected by ``connect_mesh``.
Arrays remain connected exclusively to their CPU's vector command interface.
"""
from collections.abc import Mapping, Sequence
from decimal import Decimal
from pathlib import Path
import re

from configuration import _cpu_options, connect_riscv_arrays, resolve
from .configuration import MeshParameters, connect_mesh
from .thread_placement import place_tiles, resolve_threads


ROUTER_DEFAULTS = dict(request_window=4, max_request_bytes=256, memory_queue_depth=8,
                       posted_receive_slots_per_source=16, posted_credit_batch=4,
                       posted_credit_delay_cycles=4, net_command_queue_depth=4,
                       net_ticket_capacity=16)
MESSAGE_HEADER_BYTES = 96


def _one_ghz(clock):
    # MeshParameters has already validated and normalized this quantity.
    number, unit = re.fullmatch(r"(.+?)(THz|GHz|MHz|kHz|Hz)", clock).groups()
    return Decimal(number) * dict(Hz=1, kHz=1000, MHz=1000000,
                                  GHz=1000000000, THz=1000000000000)[unit] == 1000000000


def _router_options(parameters, architecture, mesh):
    if parameters is None:
        parameters = {}
    if not isinstance(parameters, Mapping):
        raise ValueError("router_parameters must be a mapping")
    parameters = dict(parameters)
    derived = dict(tile_count=mesh.endpoint_count,
        spm_capacity_bytes=architecture["spm_capacity_bytes"],
        spm_request_bytes=architecture["spm_request_bytes"],
        spm_banks=architecture["spm_banks"], spm_bank_width=architecture["spm_bank_width"],
        router_spm_banks=architecture["router_spm_banks"],
        flit_size_bits=mesh.flit_size_bits, clock="1GHz")
    unknown = set(parameters) - ROUTER_DEFAULTS.keys() - derived.keys()
    if unknown:
        raise ValueError(f"Unknown router SPM parameters: {sorted(unknown)}")
    for key in parameters.keys() & derived.keys():
        if type(parameters[key]) is not type(derived[key]) or parameters[key] != derived[key]:
            raise ValueError(f"Router SPM {key} is derived from the tile and mesh parameters")
    options = ROUTER_DEFAULTS | parameters | derived
    for key, maximum in (("request_window", (1 << 32) - 1),
                         ("max_request_bytes", 4096),
                         ("memory_queue_depth", (1 << 32) - 1),
                         ("posted_credit_batch", (1 << 32) - 1),
                         ("posted_credit_delay_cycles", (1 << 32) - 1)):
        value = options[key]
        if type(value) is not int or not 1 <= value <= maximum:
            raise ValueError(f"Router SPM {key} must be an integer in [1, {maximum}]")
    slots = options["posted_receive_slots_per_source"]
    if type(slots) is not int or not 0 <= slots <= (1 << 32) - 1:
        raise ValueError("Router SPM posted_receive_slots_per_source must be an integer in [0, 4294967295]")
    for key, maximum in (("net_command_queue_depth", 1024),
                         ("net_ticket_capacity", 1024)):
        if type(options[key]) is not int or not 1 <= options[key] <= maximum:
            raise ValueError(f"Router SPM {key} must be an integer in [1, {maximum}]")
    if options['net_ticket_capacity'] < options['net_command_queue_depth']:
        raise ValueError('net_ticket_capacity must cover net_command_queue_depth')
    return options


def _validate_packet_capacity(options, mesh):
    # Every data packet uses the whole-message format, including endpoints with
    # no active deployment records. There is no smaller compatibility format.
    header_bytes = MESSAGE_HEADER_BYTES
    flit_bytes = mesh.flit_size_bits // 8
    packet_flits = max(2, (header_bytes + options["max_request_bytes"] + flit_bytes - 1) // flit_bytes)
    if packet_flits * flit_bytes > mesh.nic_output_buffer_bytes:
        raise ValueError("NIC output buffer must hold the complete rounded router SPM header and payload packet")


def _transfer_options(records, architecture, router, mesh):
    """Validate compiler deployment data before creating components or files.

    Produce six-word SST records only for the producer and consumer. A transfer
    ID has exactly one producer/consumer pair; destination reservations cannot
    overlap, even across different producers. No guest connection setup exists.
    """
    if records is None:
        records = []
    if not isinstance(records, (tuple, list)) or len(records) > 1024:
        raise ValueError('network_transfers must contain at most 1024 deployment records')
    fields = ('transfer_id', 'source_tile', 'destination_tile', 'receive_base', 'slot_capacity', 'slot_count')
    result = [[] for _ in range(mesh.endpoint_count)]
    ranges = [[] for _ in result]
    counts = [0 for _ in result]
    identities = set()
    capacity = architecture['spm_capacity_bytes']
    width, banks = architecture['spm_bank_width'], architecture['spm_banks']
    # net.info(POINTER) promises a directly usable CPU/RVV address.
    allowed = set(architecture['router_spm_banks']) & set(architecture['cpu_spm_banks'])
    for record in records:
        if not isinstance(record, Mapping) or set(record) != set(fields):
            raise ValueError(f'each network transfer requires exactly {fields}')
        values = [record[key] for key in fields]
        if any(type(value) is not int for value in values):
            raise ValueError('network transfer fields must be integers')
        identity, source, destination, base, size, slots = values
        if not 0 <= identity <= (1 << 63) - 1 or identity in identities:
            raise ValueError('transfer IDs must be unique nonnegative 63-bit values')
        identities.add(identity)
        if not (0 <= source < len(result) and 0 <= destination < len(result)) or source == destination:
            raise ValueError('transfer source and destination must be distinct existing tiles')
        if not 1 <= slots <= 256 or not 1 <= size <= (1 << 63) - 1:
            raise ValueError('transfer slot count must be 1..256 and capacity must be positive')
        span, offset = size * slots, base - 0x90000000
        if not router['posted_receive_slots_per_source'] or base % 8 or not 0 <= offset < capacity or span > capacity - offset:
            raise ValueError('transfer receive reservation needs aligned accessible SPM and posted storage')
        end = offset + min(span, banks * width)
        while offset < end:
            if offset // width % banks not in allowed:
                raise ValueError('receive reservation crosses a bank inaccessible to the CPU or router')
            offset += min(end - offset, width - offset % width)
        if any(base < b and a < base + span for a, b in ranges[destination]):
            raise ValueError('transfer receive reservations on the same tile must not overlap')
        ranges[destination].append((base, base + span))
        counts[destination] += slots
        if counts[destination] > 65536:
            raise ValueError('destination receive slots exceed token capacity')
        result[source].extend(values)
        result[destination].extend(values)
    return result


def _file_inputs(elfs, qemu, memory_directory, count, *, extra_inputs=(), extra_outputs=()):
    """Validate all inputs/output aliases before any file is truncated."""
    if not isinstance(elfs, Sequence) or isinstance(elfs, (str, bytes)) or len(elfs) != count:
        raise ValueError(f"elfs must contain exactly {count} paths in row-major tile order")
    root = Path(__file__).resolve().parents[3]
    try:
        qemu = Path(qemu or root / "build/src/qemu/qemu-system-riscv64").resolve()
        elfs = tuple(Path(elf).resolve() for elf in elfs)
        directory = Path(memory_directory).resolve()
    except (TypeError, ValueError) as error:
        raise ValueError("QEMU, ELF and memory_directory values must be filesystem paths") from error
    for label, path in [("QEMU", qemu), *(("RISC-V ELF", elf) for elf in elfs)]:
        if not path.is_file():
            raise ValueError(f"{label} does not exist or is not a regular file: {path}")
    files = tuple((directory / f"tile{i}-spm.bin").resolve() for i in range(count))
    input_paths = {qemu, *elfs, *extra_inputs}
    input_inodes = {(path.stat().st_dev, path.stat().st_ino) for path in input_paths}
    output_paths, output_inodes = set(), set()
    for path in (*files, *extra_outputs):
        for ancestor in path.parents:
            if ancestor.exists():
                if not ancestor.is_dir():
                    raise ValueError(f"tile backing output has a non-directory ancestor: {ancestor}")
                break
        if path in input_paths:
            raise ValueError("tile backing files must be separate from every ELF and QEMU input")
        if path in output_paths:
            raise ValueError("every tile must use a distinct backing file")
        output_paths.add(path)
        if path.exists():
            if not path.is_file():
                raise ValueError(f"tile backing output is not a regular file: {path}")
            inode = (path.stat().st_dev, path.stat().st_ino)
            if inode in input_inodes:
                raise ValueError("tile backing files must not alias an ELF or QEMU input")
            if inode in output_inodes:
                raise ValueError("every tile must use a distinct backing file")
            output_inodes.add(inode)
    return elfs, qemu, files


def connect_riscv_mesh(sst, parameters, *, elfs, memory_directory, qemu=None,
                       cpu_parameters=None, mesh_parameters=None,
                       router_parameters=None, network_transfers=None, dram_tiles=None,
                       tile_threads=None, name="tile_mesh"):
    """Build one complete tile per mesh router; return ``mesh`` and ``tiles``.

    ``elfs`` is an explicit row-major sequence, one path per tile. The same ELF
    can appear more than once. Each tile has an independent zeroed backing file
    named ``tileN-spm.bin`` within ``memory_directory``; all guests may therefore
    use the same virtual SPM addresses. The validated model uses one local port
    per router, one VN and a common 1 GHz CPU/SPM/router clock.

    The mesh defaults to four physical banks, CPU access to all four, and router
    access to banks 2 and 3. An overridden bank count selects all CPU banks and
    the highest min(2, count) router banks, unless explicit lists are supplied.
    These are address-selected physical banks, not separate bandwidth quotas.
    Resolved snapshots retain their explicit lists, including an empty router
    list, which this complete-mesh helper rejects.

    Each CPU connects to its NIU through the ``network_commands`` port for
    whole-message instructions. The NIU captures descriptors on submission and
    suspends pending receive/wait instructions without guest polling. It reads
    only local source SPM and writes only deployed destination receive slots.
    See ``network-instructions.md``.

    ``network_transfers`` contains compiler deployment records with transfer_id,
    source_tile, destination_tile, receive_base, slot_capacity and slot_count.
    Only participating tiles reserve state; only destinations reserve SPM.
    Transfers route through Mordred XY to the final tile, without intermediate
    CPU or SPM service. ``net.recv()`` acquires any eligible completed transfer.

    File paths, all output aliases, and every parameter are checked before
    creating SST components or truncating any backing file. Component prefixes
    are ``<name>.tileN.``. ``tiles`` is a tuple of dictionaries containing
    ``cpu``, ``arrays``, ``scratchpad``, ``router_spm`` and ``memory_file``.
    ``mesh`` has the same graph collections as ``connect_mesh``.

    ``dram_tiles`` maps selected endpoint IDs to an image and DRAM parameters.
    Those endpoints have a control CPU/SPM and a DRAM-backed NIU instead of
    analog arrays. Their payload source addresses identify DRAM; descriptors
    and incoming messages still occupy local SPM. See components/dram_tile.

    With multiple SST threads, complete tiles are assigned to spatial workers
    using sst.self; only mesh links cross workers. ``tile_threads`` optionally
    supplies one worker ID per row-major physical tile. One MPI rank is required.
    The resolved assignment is returned in ``tile_threads``. See docs/threading.md.
    """
    if not isinstance(name, str) or not name or any(c.isspace() for c in name):
        raise ValueError("name must be a nonempty string without whitespace")
    if mesh_parameters is None:
        mesh = MeshParameters()
    elif isinstance(mesh_parameters, Mapping):
        try:
            mesh = MeshParameters(**mesh_parameters)
        except TypeError as error:
            raise ValueError(f"invalid mesh parameter fields: {error}") from error
    elif isinstance(mesh_parameters, MeshParameters):
        mesh = mesh_parameters
    else:
        raise ValueError("mesh_parameters must be MeshParameters or a mapping")
    if mesh.local_ports != 1:
        raise ValueError("complete tile meshes require exactly one local port per router")
    if not _one_ghz(mesh.clock):
        raise ValueError("complete tile meshes currently require a 1 GHz mesh clock")
    threads, workers = resolve_threads(sst, mesh, tile_threads)
    if parameters is None:
        parameters = {}
    if not isinstance(parameters, Mapping):
        raise ValueError("parameters must be an architecture mapping")
    overrides = dict(spm_banks=4) | dict(parameters)
    architecture = resolve(overrides, cpu_parameters=cpu_parameters)
    if overrides.get("router_spm_banks") is None:
        count = architecture["spm_banks"]
        architecture["router_spm_banks"] = list(range(max(0, count - 2), count))
    if not architecture["router_spm_banks"]:
        raise ValueError("router_spm_banks must be nonempty for a tile mesh")
    cpu_options = _cpu_options(architecture, cpu_parameters)
    router_options = _router_options(router_parameters, architecture, mesh)
    transfers = _transfer_options(network_transfers, architecture, router_options, mesh)
    _validate_packet_capacity(router_options, mesh)
    from components.dram_tile.configuration import resolve_dram_tiles, connect_dram_tile
    dram_tiles = resolve_dram_tiles(dram_tiles, architecture, mesh)
    try:
        dram_outputs = {identity: (Path(memory_directory)/f'tile{identity}-dram.bin').resolve()
                        for identity in dram_tiles}
    except (TypeError, ValueError) as error:
        raise ValueError('memory_directory must be a filesystem path') from error
    elfs, qemu, files = _file_inputs(elfs, qemu, memory_directory, mesh.endpoint_count,
        extra_inputs=tuple(image for _, image in dram_tiles.values()), extra_outputs=tuple(dram_outputs.values()))

    tiles, endpoints = [], []
    for identity, (elf, memory_file) in enumerate(zip(elfs, files)):
        prefix = f"{name}.tile{identity}."
        if identity in dram_tiles:
            dram, image = dram_tiles[identity]
            tile = connect_dram_tile(sst, architecture, dram=dram, image=image,
                endpoint_parameters=router_options | dict(tile_id=identity, net_transfers=transfers[identity]),
                elf=elf, qemu=qemu, memory_file=memory_file, cpu_parameters=cpu_parameters,
                dram_memory_file=dram_outputs[identity], name_prefix=prefix)
            tiles.append(tile)
            endpoints.append(tile['router_spm'])
            continue
        router_spm = sst.Component(f"{prefix}router_spm", "tilecomponents.MordredSpmEndpoint")
        router_spm.addParams(router_options | dict(tile_id=identity, net_transfers=transfers[identity]))
        memory = router_spm.setSubComponent("memory", "memHierarchy.standardInterface")
        cpu, arrays, scratch = connect_riscv_arrays(sst, architecture, elf=elf,
            qemu=qemu, memory_file=memory_file, cpu_parameters=cpu_parameters,
            clients=(memory,), name_prefix=prefix,
            router_requestor=f"{prefix}router_spm:memory")
        sst.Link(f"{prefix}network_commands").connect(
            (cpu, "network_commands", "1ns"), (router_spm, "network_commands", "1ns"))
        tiles.append(dict(cpu=cpu, arrays=arrays, scratchpad=scratch,
                          router_spm=router_spm, memory_file=memory_file))
        endpoints.append(router_spm)
    fabric = connect_mesh(sst, mesh, name=name, endpoints=endpoints)
    if threads > 1:
        place_tiles(sst, tiles, fabric["routers"], workers, name)
    return dict(parameters=architecture, cpu_parameters=cpu_options,
                router_parameters=router_options, mesh=fabric, tiles=tuple(tiles),
                tile_threads=workers)
