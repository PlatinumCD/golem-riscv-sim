"""Independent byte and LSQ-beat oracles for grouped RVV regressions."""
PHASE_IDS = list(range(100, 151))
SOURCE = 0x90101000
OUTPUT = 0x90110000
CROSS_SOURCE = 0x90102ff0
CROSS_OUTPUT = 0x9014fff0
BASE = 0x90000000


def pattern(start, count):
    return bytes((13 * i + (i >> 8) * 11 + 7) & 255 for i in range(start, start + count))


def partial_shape(index, vb):
    if index >= 40:
        return 8, 4, index % 2, 4
    group = (2, 4, 8)[(index - 16) // 8]
    element = 1 << (((index - 16) % 8) // 2)
    return group, element, index % 2, group * vb - element


def expected_entries(index, vb):
    result = []

    def transfer(address, count, write):
        for offset in range(0, count, vb):
            result.append((address + offset, min(vb, count - offset), write))

    out = OUTPUT + index * 0x1000
    if index < 16:
        group = (1, 2, 4, 8)[index % 4]
        transfer(SOURCE, group * vb, 0)
        transfer(out, group * vb, 1)
    elif index < 42:
        group, _, _, active = partial_shape(index, vb)
        for write, base in ((0, SOURCE), (1, out)):
            transfer(base, active, write)
            transfer(base + group * vb, active, write)
    elif index == 42:
        transfer(SOURCE, 8 * vb, 0)
        transfer(out, vb, 1)
    elif index == 43:
        transfer(SOURCE, 8 * vb, 0)
        transfer(SOURCE + 8 * vb, vb, 0)
        transfer(out, 8 * vb, 1)
    elif index == 44:
        transfer(out, 8 * vb, 1)
        transfer(out + 8 * vb, vb, 1)
    elif index == 45:
        transfer(out, 8 * vb, 1)  # Cross-page load remains synchronous.
    elif index == 46:
        transfer(SOURCE, 8 * vb, 0)  # Cross-page store remains synchronous.
    elif index == 49:
        transfer(SOURCE, vb + 4, 0)
        transfer(out, 8 * vb, 1)
    elif index == 50:
        transfer(SOURCE, 8 * vb, 0)
        transfer(SOURCE + 8 * vb, 8 * vb, 0)
        transfer(out, 8 * vb, 1)
    # Nonzero vstart retains the reference helper and makes no async entries.
    return result


def check_phase(identity, selected, case, phase):
    if case['depth'] == 1:
        return
    index, vb = identity - 100, case['vlen'] // 8
    wanted = expected_entries(index, vb)
    actual = [(entry['address'], entry['bytes'], entry['write']) for entry in selected]
    assert actual == wanted, ('Grouped instruction beat admission', identity, actual, wanted)
    if wanted and index not in (0, 4, 8, 12, 40, 41):
        assert phase['peak'] > 1, ('Grouped beats did not overlap', identity, phase)
    if index in (42, 43, 49, 50):
        assert phase['stalls'].get('register', 0) > 0, ('Last-register RAW/WAW did not wait', identity, phase)
    if index == 44:
        assert phase['stalls'].get('register', 0) == 0, ('Captured store falsely reserved its source', phase)


def check_outputs(trial, case):
    vb = case['vlen'] // 8
    checked = 0
    with (trial / 'scratchpad.bin').open('rb') as memory:
        def read(address, count):
            memory.seek(address - BASE)
            data = memory.read(count)
            assert len(data) == count, ('Missing output', address, count)
            return data

        def compare(address, wanted, context):
            nonlocal checked
            actual = read(address, len(wanted))
            assert actual == wanted, (context, hex(address), actual.hex(), wanted.hex())
            checked += len(wanted)

        for index in range(16):
            group = (1, 2, 4, 8)[index % 4]
            out = OUTPUT + index * 0x1000
            compare(out, pattern(0, group * vb) + b'\xa5' * 16, ('Whole-register bytes and guard', index))
            vl = int.from_bytes(read(0x90160000 + 16 * index, 8), 'little')
            vtype = int.from_bytes(read(0x90160008 + 16 * index, 8), 'little')
            illegal = (index // 4) % 2
            assert vl == 0 and bool(vtype >> 63) == bool(illegal), ('Whole transfer changed VL/vtype', index, vl, vtype)

        for index in range(16, 42):
            group, element, ta, active = partial_shape(index, vb)
            out, group_bytes = OUTPUT + index * 0x1000, group * vb
            for number in range(2):
                prefix = pattern(number * group_bytes, active)
                compare(out + number * group_bytes, prefix + b'\xa5' * (group_bytes - active),
                        ('Grouped short store modified inactive bytes', index, number))
                address = out + 0x800 + number * group_bytes
                compare(address, prefix, ('Grouped load active bytes', index, number))
                tail = read(address + active, group_bytes - active)
                old_element = (3 + 2 * number).to_bytes(element, 'little')
                allowed = (old_element, b'\xff' * element) if ta else (old_element,)
                assert all(tail[offset:offset + element] in allowed for offset in range(0, len(tail), element)), (
                    'Grouped load tail policy', index, number, tail.hex(), allowed)
                checked += len(tail)

        compare(OUTPUT + 42 * 0x1000, bytes((byte + 1) & 255 for byte in pattern(7 * vb, vb)), 'Last-register RAW')
        compare(OUTPUT + 43 * 0x1000, pattern(0, 7 * vb) + pattern(8 * vb, vb), 'Last-register WAW')
        compare(OUTPUT + 44 * 0x1000, pattern(0, 8 * vb) + b'\x07' * vb, 'Captured grouped store')
        compare(OUTPUT + 45 * 0x1000, bytes((17 * i + 9) & 255 for i in range(8 * vb)), 'Cross-page whole load')
        compare(CROSS_OUTPUT, pattern(0, 8 * vb) + b'\xa5' * 16, 'Cross-page whole store and guard')
        # vl8re32's vstart counts 32-bit elements, while vs8r's counts bytes.
        compare(OUTPUT + 47 * 0x1000, b'\xa5' + pattern(1, 3) + pattern(8 * vb + 4, 8 * vb - 4),
                'Whole-register vstart element units')
        compare(OUTPUT + 48 * 0x1000, b'\xa5' * 4 + pattern(4, 8 * vb - 4), 'Grouped ordinary vstart')
        active = vb + 4
        address = OUTPUT + 49 * 0x1000
        compare(address, pattern(0, active), 'Pending load survives vtype change')
        tail = read(address + active, 7 * vb - active)
        assert all(tail[offset:offset + 4] in (b'\x03\x00\x00\x00', b'\xff' * 4)
                   for offset in range(0, len(tail), 4)), ('Pending load tail metadata changed', tail.hex())
        checked += len(tail)
        compare(address + 7 * vb, b'\x07' * vb, 'Tail-only register WAW after vtype change')
        lhs, rhs = pattern(0, 8 * vb), pattern(8 * vb, 8 * vb)
        added = b''.join(((int.from_bytes(lhs[offset:offset + 4], 'little') +
                           int.from_bytes(rhs[offset:offset + 4], 'little')) & 0xffffffff).to_bytes(4, 'little')
                          for offset in range(0, 8 * vb, 4))
        compare(OUTPUT + 50 * 0x1000, added, 'Grouped arithmetic sources and destination')
    return checked


def snapshots(trial, vlen):
    """Compare complete destination groups to synchronous QEMU tail policy."""
    vb = vlen // 8
    data = []
    with (trial / 'scratchpad.bin').open('rb') as memory:
        for index in range(16, 42):
            group, _, _, _ = partial_shape(index, vb)
            memory.seek(OUTPUT - BASE + index * 0x1000 + 0x800)
            data.append(memory.read(2 * group * vb))
        memory.seek(OUTPUT - BASE + 49 * 0x1000)
        data.append(memory.read(8 * vb))
    return data
