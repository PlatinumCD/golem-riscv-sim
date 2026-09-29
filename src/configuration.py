"""Supported architecture parameters and SST component composition."""
from pathlib import Path
from profiling import enabled as profiling_enabled

DEFAULTS = {
    "cost_per_array_program_cycles": 0,
    "array_program_delay_scope": "per_command",
    "cost_per_mvm_cycles": 100,
    "arrays_per_tile": 1,
    "array_rows": 32,
    "array_cols": 32,
    "riscv_vector_length_bits": 256,
    "spm_capacity_bytes": 2 * 1024 * 1024,
    "spm_banks": 8,
    "cpu_spm_banks": None,
    "router_spm_banks": None,
    "spm_bank_width": 4,
    "spm_read_ports_per_bank": 1,
    "spm_write_ports_per_bank": 1,
    "spm_channels": 2,
    "spm_channel_width": 32,
    "spm_request_bytes": 32,
    "array_inflight_bytes": 64,
    "array_link_duplex": "shared",
    "array_pipeline_enabled": True,
}
BANK_MAP_KEYS = frozenset(("cpu_spm_banks", "router_spm_banks"))
SPM_KEYS = frozenset(k for k in DEFAULTS if k.startswith("spm_")
                    and k not in ("spm_request_bytes", "spm_capacity_bytes")) | BANK_MAP_KEYS
ARRAY_KEYS = (frozenset(k for k in DEFAULTS if not k.startswith("spm_"))
              - BANK_MAP_KEYS) | {"array_link_width"}


def resolve(overrides=None, *, cpu_parameters=None):
    """Resolve shared VLEN and derive the register link width in bytes/cycle.

    A resolved parameter snapshot can be passed back in, but its derived link
    width must agree with VLEN. The existing CPU-parameter spelling of VLEN is
    accepted when it agrees with any explicitly supplied architecture value.
    """
    overrides = {} if overrides is None else overrides
    cpu_parameters = {} if cpu_parameters is None else cpu_parameters
    unknown = set(overrides) - DEFAULTS.keys() - {"array_link_width"}
    if unknown:
        if unknown & {"spm_to_array_link_width", "spm_channels_shared"}:
            raise ValueError("Direct SPM-array settings were removed; the register link width is derived from riscv_vector_length_bits")
        raise ValueError(f"Unknown architecture parameters: {sorted(unknown)}")
    p = DEFAULTS | {key: value for key, value in overrides.items() if key != "array_link_width"}
    vlen_key = "riscv_vector_length_bits"
    if vlen_key in cpu_parameters:
        if vlen_key in overrides and overrides[vlen_key] != cpu_parameters[vlen_key]:
            raise ValueError("Conflicting architecture and CPU riscv_vector_length_bits")
        p[vlen_key] = cpu_parameters[vlen_key]
    for name, value in p.items():
        if name in BANK_MAP_KEYS:
            continue
        if name == "array_pipeline_enabled":
            if type(value) is not bool:
                raise ValueError("array_pipeline_enabled must be boolean")
        elif name == "array_link_duplex":
            if value not in ("shared", "independent"):
                raise ValueError("array_link_duplex must be shared or independent")
        elif name == "array_program_delay_scope":
            if value not in ("per_command", "initial_full_array"):
                raise ValueError("array_program_delay_scope must be per_command or initial_full_array")
        elif type(value) is not int or value < (0 if name.startswith("cost_") else 1) or value > 2**31 - 1:
            raise ValueError(f"Invalid integer parameter: {name}={value!r}")
    for name in BANK_MAP_KEYS:
        value = p[name]
        if value is None:
            value = list(range(p["spm_banks"])) if name == "cpu_spm_banks" else []
        if not isinstance(value, (list, tuple)) or any(type(bank) is not int or
                not 0 <= bank < p["spm_banks"] for bank in value) or len(set(value)) != len(value):
            raise ValueError(f"{name} must contain unique integer physical bank IDs in range(spm_banks)")
        p[name] = list(value)
    vlen = p[vlen_key]
    if vlen not in (128, 256, 512, 1024):
        raise ValueError("riscv_vector_length_bits must be a power of two from 128 to 1024")
    width = vlen // 8
    if "array_link_width" in overrides and (type(overrides["array_link_width"]) is not int or overrides["array_link_width"] != width):
        raise ValueError("array_link_width is derived from riscv_vector_length_bits / 8 and cannot be overridden")
    p["array_link_width"] = width
    if p["array_rows"] * p["array_cols"] > (2**32 - 1) // 4:
        raise ValueError("One array matrix exceeds the model's 32-bit byte-count limit")
    request_bytes = p["spm_request_bytes"]
    if request_bytes < 4 or request_bytes > 2 * 1024 * 1024 or request_bytes & (request_bytes - 1):
        raise ValueError("spm_request_bytes must be a power of two from 4 bytes through 2 MiB")
    return p


def _name_prefix(value):
    if not isinstance(value, str) or any(c.isspace() for c in value):
        raise ValueError("name_prefix must be a string without whitespace")
    return value


def connect(sst, parameters, clients=(), *, experimental=None, name_prefix=""):
    """Instantiate independent scratchpad and array components for test fixtures.

    Only clients connect to SPM. Arrays expose a payload command port, with no
    memory interface. Real CPU composition uses connect_riscv_arrays().
    """
    name_prefix = _name_prefix(name_prefix)
    p = resolve(parameters)
    experimental = _experimental_controls(experimental)
    arrays = connect_arrays(sst, p, name_prefix=name_prefix)
    scratch = _connect_scratchpad(sst, p, clients, experimental=experimental,
                                  name_prefix=name_prefix)
    return arrays, scratch


def connect_arrays(sst, parameters, *, name_prefix=""):
    """Instantiate arrays with only a vector-payload command connection."""
    name_prefix = _name_prefix(name_prefix)
    p = resolve(parameters)
    arrays = sst.Component(f"{name_prefix}arrays", "tilecomponents.AnalogArrays")
    arrays.addParams({k: p[k] for k in ARRAY_KEYS})
    return arrays


def connect_scratchpad(sst, parameters, clients=(), *, experimental=None, name_prefix="",
                       cpu_requestor="", router_requestor=""):
    """Instantiate SPM, optionally binding ordinary clients to physical banks.

    Requestor names identify StandardMem interfaces, not component roles inferred
    from names. These bindings do not imply QEMU's external-commit protocol.
    """
    return _connect_scratchpad(sst, resolve(parameters), clients, experimental=experimental,
                               name_prefix=name_prefix, cpu_requestor=cpu_requestor,
                               router_requestor=router_requestor)


def _bank_requestors(p, cpu_requestor, router_requestor, external_write_requestor=None):
    for name, value in (("cpu_requestor", cpu_requestor), ("router_requestor", router_requestor),
                         ("external_write_requestor", external_write_requestor)):
        if value is not None and (not isinstance(value, str) or any(c.isspace() for c in value)):
            raise ValueError(f"{name} must be a requestor name without whitespace")
    if cpu_requestor and external_write_requestor and cpu_requestor != external_write_requestor:
        raise ValueError("cpu_requestor must match external_write_requestor when both are supplied")
    cpu = cpu_requestor or external_write_requestor or ""
    router = router_requestor or ""
    if cpu and router and cpu == router:
        raise ValueError("CPU and router requestors must be distinct")
    if cpu and not p["cpu_spm_banks"]:
        raise ValueError("a named CPU requestor requires nonempty cpu_spm_banks")
    if router and not p["router_spm_banks"]:
        raise ValueError("a named router requestor requires nonempty router_spm_banks")
    return dict(cpu_spm_banks=p["cpu_spm_banks"], router_spm_banks=p["router_spm_banks"],
                cpu_requestor=cpu, router_requestor=router)


def _connect_scratchpad(sst, p, clients, *, experimental=None, memory_file=None,
                        external_write_requestor=None, name_prefix="", router_requestor=None,
                        cpu_requestor=None):
    name_prefix = _name_prefix(name_prefix)
    experimental = _experimental_controls(experimental)
    bank_access = _bank_requestors(p, cpu_requestor, router_requestor, external_write_requestor)
    scratch = sst.Component(f"{name_prefix}scratchpad", "tilecomponents.Scratchpad")
    request_bytes = p["spm_request_bytes"]
    capacity = f'{p["spm_capacity_bytes"]}B'
    scratch.addParams(dict(clock="1GHz", size=capacity, scratch_line_size=request_bytes,
                          memory_line_size=request_bytes, backing="malloc", response_per_cycle=0))
    scratch.addParams(bank_access | dict(spm_banks=p["spm_banks"], spm_bank_width=p["spm_bank_width"]))
    if memory_file is not None:
        scratch.addParams(dict(backing="mmap", memory_file=str(memory_file),
                              external_write_requestor=external_write_requestor))
    converter = scratch.setSubComponent("backendConvertor", "tilecomponents.ExactConvertor")
    converter.addParams(dict(request_width=request_bytes))
    backend = converter.setSubComponent("backend", "tilecomponents.BankedBackend")
    backend.addParams({k: p[k] for k in SPM_KEYS})
    backend.addParams(bank_access)
    backend.addParams(dict(mem_size=capacity, request_width=request_bytes, max_requests_per_cycle=-1))
    backend.addParam("experimental_queue_entries", experimental.get("queue_entries", 64))
    # Routing only: no additional single-request bottleneck ahead of the banks.
    connection_type = ("tilecomponents.ProfiledSpmConnections"
                       if profiling_enabled() else "memHierarchy.Bus")
    bus = sst.Component(f"{name_prefix}spm_connections", connection_type)
    bus.addParams(dict(bus_frequency="1GHz", drain_bus=True))
    for index, client in enumerate(clients):
        sst.Link(f"{name_prefix}spm_client_{index}").connect((client, "lowlink", "1ns"), (bus, f"highlink{index}", "1ns"))
    sst.Link(f"{name_prefix}spm_controller").connect((bus, "lowlink0", "1ns"), (scratch, "highlink", "1ns"))
    return scratch


def _experimental_controls(experimental):
    experimental = experimental or {}
    if set(experimental) - {"queue_entries"}:
        raise ValueError("Only the scratchpad queue_entries experimental control is supported")
    if "queue_entries" in experimental and (type(experimental["queue_entries"]) is not int or not 1 <= experimental["queue_entries"] <= 4096):
        raise ValueError("Experimental queue_entries must be 1..4096")
    return experimental


CPU_DEFAULTS = {
    "instruction_budget": 256,
    "issue_width": 1,
    "load_store_queue_depth": 1,
    "analog_command_queue_depth": 0,
    "analog_command_queue_bytes": 16384,
    "instruction_cache_enabled": True,
    "instruction_cache_bytes": 8192,
    "instruction_cache_line_bytes": 64,
    "instruction_cache_ways": 2,
    "instruction_cache_hit_cycles": 1,
    "riscv_vector_enabled": True,
    "riscv_vector_length_bits": DEFAULTS["riscv_vector_length_bits"],
    "riscv_vector_element_bits": 64,
    "host_timeout_seconds": 30,
    "serial_output": "",
}


def _cpu_options(p, cpu_parameters):
    """Validate CPU settings without creating components or backing files."""
    cpu_parameters = cpu_parameters or {}
    if not p["cpu_spm_banks"]:
        raise ValueError("cpu_spm_banks must be nonempty for a real RISC-V CPU")
    unknown = set(cpu_parameters) - CPU_DEFAULTS.keys()
    if unknown:
        raise ValueError(f"Unknown RISC-V CPU parameters: {sorted(unknown)}")
    options = CPU_DEFAULTS | cpu_parameters
    options["riscv_vector_length_bits"] = p["riscv_vector_length_bits"]
    options["array_pipeline_enabled"] = p["array_pipeline_enabled"]
    for name in ("instruction_budget", "issue_width", "host_timeout_seconds"):
        if type(options[name]) is not int or not 1 <= options[name] <= 2**31 - 1:
            raise ValueError(f"{name} must be a positive integer")
    depth = options["load_store_queue_depth"]
    if type(depth) is not int or not 1 <= depth <= 64:
        raise ValueError("load_store_queue_depth must be an integer from 1 through 64")
    depth = options["analog_command_queue_depth"]
    if type(depth) is not int or not 0 <= depth <= 16:
        raise ValueError("analog_command_queue_depth must be an integer from 0 through 16")
    size = options["analog_command_queue_bytes"]
    if type(size) is not int or not 1024 <= size <= 16384 or size % 4:
        raise ValueError("analog_command_queue_bytes must be a multiple of four from 1024 through 16384")
    if options["host_timeout_seconds"] > 3600:
        raise ValueError("host_timeout_seconds must not exceed 3600")
    if type(options["riscv_vector_enabled"]) is not bool:
        raise ValueError("riscv_vector_enabled must be boolean")
    vlen, elen = options["riscv_vector_length_bits"], options["riscv_vector_element_bits"]
    if type(vlen) is not int or not 128 <= vlen <= 1024 or vlen & (vlen - 1):
        raise ValueError("riscv_vector_length_bits must be a power of two from 128 to 1024")
    if type(elen) is not int or elen not in (32, 64):
        raise ValueError("riscv_vector_element_bits must be 32 or 64")
    capacity = p["spm_capacity_bytes"]
    if capacity > 32 * 1024 * 1024 or capacity % 4096 or capacity % p["spm_request_bytes"]:
        raise ValueError("QEMU SPM capacity must be at most 32 MiB and divisible by 4096 and spm_request_bytes")
    if type(options["instruction_cache_enabled"]) is not bool:
        raise ValueError("instruction_cache_enabled must be boolean")
    for name in ("instruction_cache_bytes", "instruction_cache_line_bytes", "instruction_cache_ways"):
        value = options[name]
        if type(value) is not int or value < 1 or value > 2**31 - 1 or value & (value - 1):
            raise ValueError(f"{name} must be a positive power of two")
    cache_bytes = options["instruction_cache_bytes"]
    line_bytes = options["instruction_cache_line_bytes"]
    if cache_bytes > 16 * 1024 * 1024:
        raise ValueError("instruction_cache_bytes must be at most 16 MiB")
    if line_bytes < 4 or line_bytes > capacity or capacity % line_bytes:
        raise ValueError("instruction_cache_line_bytes must be at least 4, fit in SPM, and divide its capacity")
    if cache_bytes < line_bytes * options["instruction_cache_ways"]:
        raise ValueError("instruction_cache_bytes must hold at least one line per way")
    hit_cycles = options["instruction_cache_hit_cycles"]
    if type(hit_cycles) is not int or not 1 <= hit_cycles <= 2**31 - 1:
        raise ValueError("instruction_cache_hit_cycles must be a positive integer")
    return options


def connect_riscv(sst, parameters, *, elf, qemu=None, memory_file,
                  cpu_parameters=None, clients=(), name_prefix="", router_requestor=None):
    """Connect a standalone RV64 QEMU core and optional clients to one SPM.

    memory_file is a run output, freshly zeroed for each simulation. All ELF
    load segments must be inside [0x90000000, 0x90000000 + spm_capacity_bytes).
    Other clients use SPM-relative addresses, as in connect(). ``name_prefix``
    qualifies every tile component and link; each tile needs its own memory_file.
    """
    name_prefix = _name_prefix(name_prefix)
    p = resolve(parameters, cpu_parameters=cpu_parameters)
    options = _cpu_options(p, cpu_parameters)
    _bank_requestors(p, None, router_requestor, f"{name_prefix}riscv:qemu_memory")
    capacity = p["spm_capacity_bytes"]
    root = Path(__file__).resolve().parent.parent
    qemu = Path(qemu or root / "build/src/qemu/qemu-system-riscv64").resolve()
    elf = Path(elf).resolve()
    if not qemu.is_file():
        raise ValueError("Build shared-SPM QEMU first: python3 -B source_new/components/riscv-qemu/build_qemu.py")
    if not elf.is_file():
        raise ValueError(f"RISC-V ELF does not exist: {elf}")
    backing = Path(memory_file).resolve()
    if backing in (elf, qemu):
        raise ValueError("memory_file must be a separate simulation output")
    backing.parent.mkdir(parents=True, exist_ok=True)
    with backing.open("wb") as output:
        output.truncate(capacity)
    cpu = sst.Component(f"{name_prefix}riscv", "tilecomponents.RiscvQemu")
    cpu.addParams(options | dict(qemu=str(qemu), elf=str(elf), memory_file=str(backing),
                                spm_capacity_bytes=capacity, spm_request_bytes=p["spm_request_bytes"]))
    interface = cpu.setSubComponent("qemu_memory", "memHierarchy.standardInterface")
    scratch = _connect_scratchpad(sst, p, [interface, *clients], memory_file=backing,
                                  external_write_requestor=f"{name_prefix}riscv:qemu_memory",
                                  name_prefix=name_prefix, router_requestor=router_requestor)
    sst.Link(f"{name_prefix}riscv_spm_commit").connect((cpu, "external_commit", "0ps"),
                                       (scratch, "external_commit", "0ps"))
    return cpu, scratch


def connect_riscv_arrays(sst, parameters, *, elf, qemu=None, memory_file,
                         cpu_parameters=None, clients=(), name_prefix="", router_requestor=None):
    """Connect CPU to SPM and connect its vector registers to analog arrays.

    mvm.vset/vl/vs move register chunks over the array link; the existing mvm
    instruction executes the array. With array_pipeline_enabled, mvm returns
    after validated compute start and mvm.vs waits for the oldest result.
    Returns (cpu, arrays, scratchpad).
    """
    name_prefix = _name_prefix(name_prefix)
    p = resolve(parameters, cpu_parameters=cpu_parameters)
    _cpu_options(p, cpu_parameters)
    _bank_requestors(p, None, router_requestor, f"{name_prefix}riscv:qemu_memory")
    arrays = connect_arrays(sst, p, name_prefix=name_prefix)
    cpu, scratch = connect_riscv(sst, p, elf=elf, qemu=qemu, memory_file=memory_file,
        cpu_parameters=cpu_parameters, clients=clients, name_prefix=name_prefix,
        router_requestor=router_requestor)
    sst.Link(f"{name_prefix}riscv_array_commands").connect((cpu, "analog_commands", "1ns"),
                                             (arrays, "commands", "1ns"))
    return cpu, arrays, scratch
