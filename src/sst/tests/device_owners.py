#!/usr/bin/env python3
"""Host correctness tests for memory, DMA, barrier, and analog controllers."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SST = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    output = Path(tempfile.mkdtemp(prefix='golem-device-owners-'))
    sources = ['memory/memoryAccessController.cc', 'memory/instructionCache.cc',
               'memory/globalDMAClient.cc',
               'synchronization/initializationBarrierClient.cc',
               'memory/scratchpad/scratchpadTimingModel.cc',
               'analog/analogController.cc', 'analog/analogDevice.cc',
               'analog/nativeAnalogBackend.cc', 'analog/timingAnalogBackend.cc',
               'analog/crossSimAnalogBackend.cc', 'bridge/sharedAnalogMemoryBridge.cc',
               'profiling/performanceProfile.cc', 'profiling/measurementWriter.cc']
    python_flags = shlex.split(subprocess.check_output(
        ['python3-config', '--embed', '--cflags', '--ldflags'], text=True))
    # Python's NDEBUG must not disable test assertions.
    python_flags = [flag for flag in python_flags if flag != '-DNDEBUG']
    command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-I' + str(SST.parent / 'bridge/include'), str(HERE / 'device_owners_test.cpp')]
    command += [str(SST / source) for source in sources]
    command += python_flags
    if args.sanitize:
        command += ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    command += ['-o', str(output / 'device-owners')]
    print('Host artifacts:', output, flush=True)
    print(shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    subprocess.run([str(output / 'device-owners')], check=True, timeout=30)


if __name__ == '__main__':
    main()
