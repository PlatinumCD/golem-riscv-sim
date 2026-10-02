"""Compose a guest-controlled DRAM Tile on the existing Mordred message mesh."""
from collections.abc import Mapping
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
import re

from configuration import connect_riscv


@dataclass(frozen=True)
class DramParameters:
    capacity_bytes: int = 64 * 1024 * 1024
    base: int = 0x100000000
    request_bytes: int = 64
    channels: int = 2
    ranks_per_channel: int = 1
    banks_per_rank: int = 8
    row_bytes: int = 1024
    queue_depth: int = 32
    t_cas_cycles: int = 12
    t_rcd_cycles: int = 12
    t_rp_cycles: int = 12
    burst_cycles: int = 4
    clock: str = "1GHz"

    def __post_init__(self):
        limits = dict(capacity_bytes=(1 << 63)-1, base=(1 << 63)-1,
                      request_bytes=64, channels=64, ranks_per_channel=16,
                      banks_per_rank=256, row_bytes=1 << 20, queue_depth=65536,
                      t_cas_cycles=1 << 20, t_rcd_cycles=1 << 20,
                      t_rp_cycles=1 << 20, burst_cycles=1 << 20)
        for field, maximum in limits.items():
            value = getattr(self, field)
            if type(value) is not int or not 1 <= value <= maximum:
                raise ValueError(f'DRAM {field} must be an integer in [1, {maximum}]')
        if self.request_bytes & (self.request_bytes-1):
            raise ValueError('DRAM request_bytes must be a power of two, at most 64')
        if self.row_bytes & (self.row_bytes-1) or self.row_bytes < self.request_bytes:
            raise ValueError('DRAM row_bytes must be a power of two at least request_bytes')
        # timingDRAM's round-robin mapper stores its divider as uint32 and
        # exposes a signed-int row. Also cover its intermediate default banks.
        divider = self.channels*self.ranks_per_channel*max(8,self.banks_per_rank)*(self.row_bytes//self.request_bytes)
        if divider > (1 << 32)-1:
            raise ValueError('DRAM geometry exceeds the timing backend address mapper')
        row_span = self.channels*self.ranks_per_channel*self.banks_per_rank*self.row_bytes
        if self.capacity_bytes > row_span*(1 << 31):
            raise ValueError('DRAM capacity exceeds the timing backend row index')
        if self.base % self.request_bytes or self.capacity_bytes % self.request_bytes:
            raise ValueError('DRAM base and capacity must align to request_bytes')
        if self.capacity_bytes > (1 << 63)-1-self.base:
            raise ValueError('DRAM payload region must fit nonnegative 63-bit addresses')
        match = re.fullmatch(r'([0-9]+(?:\.[0-9]+)?)(GHz|MHz|kHz|Hz)', self.clock) if isinstance(self.clock, str) else None
        if not match or Decimal(match[1]) <= 0:
            raise ValueError('DRAM clock must be a positive frequency with units')


def resolve_dram_tiles(configurations, architecture, mesh):
    """Validate every DRAM image and configuration before mesh construction."""
    if configurations is None:
        return {}
    if not isinstance(configurations, Mapping):
        raise ValueError('dram_tiles must map row-major tile IDs to DRAM settings')
    result = {}
    for tile, values in configurations.items():
        if type(tile) is not int or not 0 <= tile < mesh.endpoint_count:
            raise ValueError('DRAM tile ID must identify an existing mesh endpoint')
        if not isinstance(values, Mapping) or 'image' not in values:
            raise ValueError('each DRAM tile requires an image and optional DRAM parameters')
        values = dict(values)
        try:
            image = Path(values.pop('image')).resolve()
            parameters = DramParameters(**values)
        except (TypeError, ValueError) as error:
            raise ValueError(f'invalid DRAM tile {tile}: {error}') from error
        if not image.is_file() or image.stat().st_size != parameters.capacity_bytes:
            raise ValueError('DRAM image must be a regular file exactly capacity_bytes long (sparse is supported)')
        spm_start, spm_end = 0x90000000, 0x90000000+architecture['spm_capacity_bytes']
        if parameters.base < spm_end and spm_start < parameters.base+parameters.capacity_bytes:
            raise ValueError('DRAM payload address region must not overlap control SPM')
        result[tile] = (parameters, image)
    return result


def connect_dram_tile(sst, architecture, *, dram, image, endpoint_parameters,
                      elf, qemu, memory_file, dram_memory_file, cpu_parameters, name_prefix):
    """Build a validated tile; connect its returned endpoint to connect_mesh."""
    endpoint = sst.Component(f'{name_prefix}router_spm', 'tilecomponents.DramTile')
    endpoint.addParams(endpoint_parameters | dict(dram_base=dram.base,
        dram_capacity_bytes=dram.capacity_bytes, dram_request_bytes=dram.request_bytes))
    control = endpoint.setSubComponent('memory', 'memHierarchy.standardInterface')
    payload = endpoint.setSubComponent('dram_memory', 'memHierarchy.standardInterface')
    cpu, scratchpad = connect_riscv(sst, architecture, elf=elf, qemu=qemu,
        memory_file=memory_file, cpu_parameters=cpu_parameters, clients=(control,),
        name_prefix=name_prefix, router_requestor=f'{name_prefix}router_spm:memory')
    sst.Link(f'{name_prefix}network_commands').connect(
        (cpu, 'network_commands', '1ns'), (endpoint, 'network_commands', '1ns'))
    controller = sst.Component(f'{name_prefix}dram_controller', 'memHierarchy.MemController')
    controller.addParams(dict(clock=dram.clock, addr_range_start=0,
        addr_range_end=dram.capacity_bytes-1, request_width=dram.request_bytes,
        backing='mmap', backing_in_file=str(image),
        backing_out_file=str(dram_memory_file)))
    backend = controller.setSubComponent('backend', 'memHierarchy.timingDRAM')
    backend.addParams({
        'id': endpoint_parameters['tile_id'], 'mem_size': f'{dram.capacity_bytes}B',
        'clock': dram.clock, 'request_width': dram.request_bytes,
        'max_requests_per_cycle': dram.channels,
        'addrMapper': 'memHierarchy.roundRobinAddrMapper',
        'addrMapper.interleave_size': f'{dram.request_bytes}B',
        'addrMapper.row_size': f'{dram.row_bytes}B',
        'channels': dram.channels, 'channel.numRanks': dram.ranks_per_channel,
        'channel.rank.numBanks': dram.banks_per_rank,
        'channel.transaction_Q_size': dram.queue_depth,
        'channel.rank.bank.CL': dram.t_cas_cycles, 'channel.rank.bank.CL_WR': dram.t_cas_cycles,
        'channel.rank.bank.RCD': dram.t_rcd_cycles, 'channel.rank.bank.TRP': dram.t_rp_cycles,
        'channel.rank.bank.dataCycles': dram.burst_cycles,
        'channel.rank.bank.pagePolicy': 'memHierarchy.simplePagePolicy',
        'channel.rank.bank.pagePolicy.close': 0,
        'channel.rank.bank.transactionQ': 'memHierarchy.fifoTransactionQ',
        'printconfig': 0, 'channel.printconfig': 0,
        'channel.rank.printconfig': 0, 'channel.rank.bank.printconfig': 0,
    })
    sst.Link(f'{name_prefix}dram_payload').connect(
        (payload, 'lowlink', '1ns'), (controller, 'highlink', '1ns'))
    return dict(cpu=cpu, arrays=None, scratchpad=scratchpad, router_spm=endpoint,
                memory_file=memory_file, dram_controller=controller, dram_backend=backend,
                dram_parameters=dram, dram_image=image, dram_memory_file=dram_memory_file, tile_kind='dram')
