"""Compose an explicitly wired Mordred mesh without importing SST at module load.

Router IDs are row major: ``y * x_dim + x``. Endpoint IDs are router ID times
``local_ports`` plus the local-port index. Supply endpoints in that order; each
must expose an unused ``networkIF`` SimpleNetwork subcomponent slot. Endpoint
creation, traffic parameters and application-specific IDs remain with callers.
"""
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
import math
import re


_UINT32_MAX = (1 << 32) - 1
_INT32_MAX = (1 << 31) - 1
_INT16_MAX = (1 << 15) - 1
_NUMBER = r"(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"


def _integer(name, value, minimum=1, maximum=_UINT32_MAX):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be an integer in [{minimum}, {maximum}]")


def _quantity(name, value, units):
    """Check positive finite frequency/time strings before SST creates objects."""
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a positive quantity with units")
    match = re.fullmatch(rf"\s*({_NUMBER})\s*({'|'.join(units)})\s*", value)
    if match is None:
        raise ValueError(f"{name} must be a positive quantity in {', '.join(units)}")
    number = float(match[1]) * units[match[2]]
    if not math.isfinite(number) or number <= 0:
        raise ValueError(f"{name} must be finite and positive")
    return match[1] + match[2]


@dataclass(frozen=True)
class MeshParameters:
    """Single-VN, direct-link XY mesh parameters.

    Router buffers are flits per VC, per port. NIC buffers are bytes. All NICs
    and routers share ``clock``; links use ``link_latency`` in both directions.
    Buffer sizes must represent whole flits. The NIC transmit buffer must also
    hold the caller's largest packet, which this endpoint-independent helper
    cannot infer. Upstream's completed receive-packet queue is unbounded.

    Multiple VNs are deliberately excluded: the current upstream NIC can emit
    one flit per VN per cycle on its shared link and shares latency timestamps
    between VNs. Multiple VCs within the single VN remain supported.
    """
    x_dim: int = 2
    y_dim: int = 2
    local_ports: int = 1
    num_vns: int = 1
    num_vcs: int = 1
    clock: str = "1GHz"
    link_latency: str = "1ns"
    flit_size_bits: int = 128
    router_input_buffer_flits: int = 8
    router_output_buffer_flits: int = 8
    nic_input_buffer_bytes: int = 4096
    nic_output_buffer_bytes: int = 4096
    verbose: int = 0

    def __post_init__(self):
        for name in ("x_dim", "y_dim", "local_ports", "num_vcs", "flit_size_bits"):
            _integer(name, getattr(self, name))
        _integer("num_vns", self.num_vns, maximum=1)
        _integer("verbose", self.verbose, minimum=0)
        if self.endpoint_count > _INT32_MAX + 1:
            raise ValueError("mesh endpoint IDs must fit Mordred's signed 32-bit topology IDs")
        if self.local_ports > _UINT32_MAX - 4:
            raise ValueError("local_ports plus four directional ports must fit uint32")
        if self.flit_size_bits % 8:
            raise ValueError("flit_size_bits must be a whole number of bytes")
        _integer("router_input_buffer_flits", self.router_input_buffer_flits, maximum=_INT32_MAX)
        # Upstream casts output-buffer credits to int16_t during initialization.
        _integer("router_output_buffer_flits", self.router_output_buffer_flits, maximum=_INT16_MAX)
        for name in ("router_input_buffer_flits", "router_output_buffer_flits"):
            if getattr(self, name) * self.flit_size_bits > _UINT32_MAX:
                raise ValueError(f"{name} times flit_size_bits must fit the router's uint32 bit count")
        for name in ("nic_input_buffer_bytes", "nic_output_buffer_bytes"):
            value = getattr(self, name)
            _integer(name, value, maximum=((1 << 63) - 1) // 8)
            bits = value * 8
            if bits % self.flit_size_bits or not 1 <= bits // self.flit_size_bits <= _INT32_MAX:
                raise ValueError(f"{name} must hold a positive whole number of flits with int32 credits")
        if self.nic_output_buffer_bytes * 8 < 2 * self.flit_size_bits:
            raise ValueError("nic_output_buffer_bytes must hold at least a two-flit packet")
        object.__setattr__(self, "clock", _quantity("clock", self.clock,
            {"Hz": 1, "kHz": 1e3, "MHz": 1e6, "GHz": 1e9, "THz": 1e12}))
        object.__setattr__(self, "link_latency", _quantity("link_latency", self.link_latency,
            {"s": 1, "ms": 1e-3, "us": 1e-6, "ns": 1e-9, "ps": 1e-12, "fs": 1e-15}))

    @property
    def router_count(self):
        return self.x_dim * self.y_dim

    @property
    def endpoint_count(self):
        return self.router_count * self.local_ports


def connect_mesh(sst, params=None, name="mesh", endpoints=None):
    """Return routers, topologies, NICs and links for an explicit 2D mesh.

    ``params`` is a MeshParameters instance or a mapping of its fields. When
    ``endpoints`` is None, only the router mesh is built and local ports remain
    unconnected. Otherwise supply exactly ``endpoint_count`` distinct endpoint
    components. The helper attaches ``mordred.mordredNIC`` to ``networkIF``;
    caller-owned endpoint parameters are never changed.

    Returned component/link collections are tuples. Routers and topologies
    follow router-ID order; endpoints and NICs follow endpoint-ID order. The
    directional port mapping is north/east/south/west = 0/1/2/3, with north
    increasing y. Local endpoint k uses port 4+k. Mesh boundaries have no links.
    """
    if params is None:
        params = MeshParameters()
    elif isinstance(params, Mapping):
        try:
            params = MeshParameters(**params)
        except TypeError as error:
            raise ValueError(f"invalid mesh parameter fields: {error}") from error
    elif not isinstance(params, MeshParameters):
        raise ValueError("params must be MeshParameters or a mapping of its fields")
    if not isinstance(name, str) or not name or any(c.isspace() for c in name):
        raise ValueError("name must be a nonempty string without whitespace")
    if endpoints is None:
        endpoints = ()
    else:
        if not isinstance(endpoints, Sequence) or isinstance(endpoints, (str, bytes)):
            raise ValueError("endpoints must be an explicit sequence of SST endpoint components")
        endpoints = tuple(endpoints)
        if len(endpoints) != params.endpoint_count:
            raise ValueError(f"endpoints must contain exactly {params.endpoint_count} components in endpoint-ID order")
        if len({id(endpoint) for endpoint in endpoints}) != len(endpoints):
            raise ValueError("endpoints must contain distinct components")
        if any(not callable(getattr(endpoint, "setSubComponent", None)) for endpoint in endpoints):
            raise ValueError("every endpoint must support setSubComponent for its networkIF slot")

    routers, topologies, nics = [], [], []
    router_links, endpoint_links = [], []
    router_parameters = dict(clock=params.clock, num_ports=4 + params.local_ports,
        num_local_ports=params.local_ports, num_vns=params.num_vns, num_vcs=params.num_vcs,
        flit_size=f"{params.flit_size_bits}b",
        input_buf_size=f"{params.router_input_buffer_flits * params.flit_size_bits}b",
        output_buf_size=f"{params.router_output_buffer_flits * params.flit_size_bits}b",
        verbose=params.verbose)
    for y in range(params.y_dim):
        for x in range(params.x_dim):
            identity = y * params.x_dim + x
            router = sst.Component(f"{name}.router.{x}.{y}", "mordred.mordred_router")
            router.addParams(dict(router_parameters, id=identity))
            topology = router.setSubComponent("topology", "mordred.MeshTopology")
            topology.addParams(dict(xDim=params.x_dim, yDim=params.y_dim, verbose=params.verbose))
            routers.append(router)
            topologies.append(topology)

    def connect(link_name, left, left_port, right, right_port, collection):
        link = sst.Link(f"{name}.{link_name}")
        link.connect((left, left_port, params.link_latency), (right, right_port, params.link_latency))
        collection.append(link)

    for y in range(params.y_dim):
        for x in range(params.x_dim):
            identity = y * params.x_dim + x
            if y + 1 < params.y_dim:
                connect(f"north.{x}.{y}", routers[identity], "port0",
                    routers[identity + params.x_dim], "port2", router_links)
            if x + 1 < params.x_dim:
                connect(f"east.{x}.{y}", routers[identity], "port1",
                    routers[identity + 1], "port3", router_links)

    for identity, endpoint in enumerate(endpoints):
        nic = endpoint.setSubComponent("networkIF", "mordred.mordredNIC")
        nic.addParams(dict(clock=params.clock, verbose=params.verbose,
            input_buf_size=f"{params.nic_input_buffer_bytes * 8}b",
            output_buf_size=f"{params.nic_output_buffer_bytes * 8}b"))
        nics.append(nic)
        router_id, local = divmod(identity, params.local_ports)
        connect(f"endpoint.{identity}", routers[router_id], f"port{4 + local}", nic, "port", endpoint_links)

    return dict(parameters=params, routers=tuple(routers), topologies=tuple(topologies),
        nics=tuple(nics), endpoints=endpoints, router_links=tuple(router_links),
        endpoint_links=tuple(endpoint_links), links=tuple(router_links + endpoint_links))
