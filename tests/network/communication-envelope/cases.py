"""Small orthogonal controls. Coordinates and complete directed paths are explicit."""
from dataclasses import asdict, dataclass, field


@dataclass
class Case:
    name: str
    study: str
    flows: list[tuple[int, int]] = field(default_factory=list)
    width: int = 5
    height: int = 5
    payload: int = 4096
    waves: int = 1
    tx_lanes: int = 2
    rvv_tile: int = -1
    rvv_iterations: int = 0
    delay_tile: int = -1
    delay_instructions: int = 0
    gap_instructions: int = 0
    bank_phase: bool = False
    fifo: int = 128
    rx_queue: int = 4
    fit_role: str = 'evaluation'

    def path(self, source, destination):
        # Existing Mittens routing is X then Y. Edges are directed: reverse
        # traffic does not share the forward link's bandwidth.
        path = []
        while source % self.width != destination % self.width:
            nxt = source + (1 if destination % self.width > source % self.width else -1)
            path.append((source, nxt)); source = nxt
        while source != destination:
            nxt = source + (self.width if destination > source else -self.width)
            path.append((source, nxt)); source = nxt
        return path

    def document(self):
        result = asdict(self)
        result['paths'] = [self.path(*flow) for flow in self.flows]
        result['active_tiles'] = sorted({t for pair in self.flows for t in pair} |
                                        ({self.rvv_tile} if self.rvv_tile >= 0 else set()))
        result['segments'] = (self.payload + 16383) // 16384
        result['expected_payload_bytes'] = len(self.flows) * self.payload * self.waves
        return result


def registry():
    result = []
    # RX attribution and isolated bank-phase controls.
    for n in (1, 2, 4):
        for delay in (0, 4096):
            result.append(Case(f'rx-{n}-delay{delay}', 'rx-attribution',
                [(s, 12) for s in (13, 7, 11, 17)[:n]], delay_tile=12,
                delay_instructions=delay))
    # Every subset of the three real integrated clients, at the same work per client.
    for mask in range(1, 8):
        flows = ([(12, t) for t in (13, 7)] if mask & 2 else [])
        flows += ([(t, 12) for t in (11, 17)] if mask & 4 else [])
        result.append(Case(f'clients-{mask}', 'clients', flows, waves=8,
            rvv_tile=12 if mask & 1 else -1, rvv_iterations=8192 if mask & 1 else 0))
    for phase in (False, True):
        result.append(Case(f'banks-phase{int(phase)}', 'banks',
            [(12, t) for t in (13, 7, 11, 17)] + [(t, 12) for t in (13, 7, 11, 17)],
            waves=4, tx_lanes=4, bank_phase=phase, rvv_tile=12, rvv_iterations=8192))
    shapes = {
        'independent-pairs': [(6, 7), (8, 9), (16, 17), (18, 19)],
        # Eight flows and eight 4 KiB messages, matching many-to-many; no
        # directed edge or endpoint is shared between these independent pairs.
        'independent-eight': [(0, 1), (2, 3), (5, 6), (7, 8),
                              (10, 11), (12, 13), (15, 16), (17, 18)],
        'source-shared': [(12, t) for t in (13, 7, 11, 17)],
        'sink-shared': [(t, 12) for t in (13, 7, 11, 17)],
        'both-shared': [(12, 13)] * 4,
        'many-to-many': [(6, 7), (6, 11), (8, 7), (8, 13), (16, 11), (16, 17), (18, 13), (18, 17)],
        'forward': [(11, 13)],
        'reverse': [(13, 11)],
        'duplex': [(11, 13), (13, 11)],
        # Same source/destination counts and H=4; change only path sharing.
        'paths-disjoint': [(5, 9), (15, 19)],
        'paths-shared': [(5, 9), (6, 14)],
    }
    for name, flows in shapes.items():
        for lanes in ((1, 2, 4) if name in ('source-shared', 'both-shared', 'many-to-many') else (2,)):
            result.append(Case(f'{name}-t{lanes}', 'sharing', flows, tx_lanes=lanes,
                               fit_role='holdout'))
    result.append(Case('source-shared-delayed', 'sharing', shapes['source-shared'],
        tx_lanes=2, delay_tile=13, delay_instructions=16384, fit_role='holdout'))
    # Independent pairs with equal H, shared directed edges, unique endpoints.
    # P <= H ensures every route includes a common link; H=1/P>1 is omitted.
    for hops in (4, 8, 16):
        for pairs in (1, 2, 4):
            result.append(Case(f'distance-h{hops}-p{pairs}', 'distance-contention',
                [(s, s + hops) for s in range(pairs)], width=hops + pairs, height=1,
                fit_role='holdout' if pairs > 1 else 'train'))
    for gap in (0, 512, 2048):
        result.append(Case(f'load-gap{gap}', 'sustained', [(11, 13), (12, 14)],
            payload=1024, waves=16, gap_instructions=gap, fit_role='holdout'))
    for size in (32, 64, 256, 1024, 4096, 16384, 16388, 65536):
        result.append(Case(f'payload-{size}', 'descriptors', [(12, 13)], payload=size,
                           fit_role='train' if size <= 16384 else 'holdout'))
    # A sustained delayed receiver with a genuinely small DMA queue and TX FIFO.
    result.append(Case('bounded-backpressure', 'sustained', [(11, 12), (13, 12)],
        payload=1024, waves=8, delay_tile=12, delay_instructions=8192, fifo=32, rx_queue=1))
    names = [c.name for c in result]
    assert len(names) == len(set(names))
    for c in result:
        assert c.flows or c.rvv_iterations
        assert len(c.flows) <= 8 and c.payload > 0 and c.payload % 4 == 0
        assert all(a != b and min(a,b) >= 0 and max(a,b) < c.width*c.height for a,b in c.flows)
    return {c.name: c for c in result}


REGRESSION = ('rx-2-delay4096', 'source-shared-t2', 'both-shared-t4',
              'duplex-t2', 'clients-7', 'banks-phase0', 'banks-phase1',
              'payload-16388', 'bounded-backpressure')
