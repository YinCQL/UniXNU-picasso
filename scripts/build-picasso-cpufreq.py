#!/usr/bin/env python3
"""Build the picasso SM7250 CPU-frequency probe (NativeCpuFreq) for the kernel.

Compiles drivers/NativeCpuFreq/CpuFreqProbe.cpp with this tree's recorded XNU
kext flags. The probe is inert unless the boot arguments carry
sheng-cpufreq=<1..4>; BootProgress.c calls its probe once and its poll every
round (both are weak imports, stubbed in PicassoBootProgressStubs.c).

The governor measures a CPU's idle time through sheng_cpu_idle_mach(), which
patches/picasso-cpu-idle-xnu.patch appends to osfmk/kern/processor.c.

Output: build/picasso-cpufreq/libPicassoCpuFreq.a, force-loaded by
scripts/build-xnu-4k-baseline.sh when CONFIG_XNU_PICASSO_CPUFREQ=1.
"""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / 'build/xnu').is_dir() and (ROOT / 'out/picasso/tree/build/xnu').is_dir():
    ROOT = ROOT / 'out/picasso/tree'

DRIVER = ROOT / 'drivers/NativeCpuFreq'
RECORD = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
OUT = ROOT / 'build/picasso-cpufreq'
OUT.mkdir(parents=True, exist_ok=True)


def run(command, cwd=None):
    print('+', ' '.join(map(str, command)))
    subprocess.run([str(value) for value in command], check=True, timeout=900, cwd=cwd)


def flags(record_path, source, obj):
    """Recorded IOPlatformExpert C++ compile line, retargeted to our source."""
    info = json.loads(Path(record_path).read_text().strip().rstrip(','))
    command, index, arguments = [], 0, info['arguments']
    while index < len(arguments):
        value = arguments[index]
        if value == info['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(obj)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ', '--serialize-diagnostics'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP', '--driver-mode=g++', '-fno-odr-hash-protocols',
                       '-fno-c++-static-destructors'):
            pass
        elif value == '-Xclang':
            index += 1
        elif value.startswith('-W'):
            pass
        else:
            command.append(value)
        index += 1
    command.extend(['-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unknown-warning-option',
                    '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(DRIVER)])
    return command, info['directory']


def main():
    source = DRIVER / 'CpuFreqProbe.cpp'
    obj = OUT / 'CpuFreqProbe.o'
    command, cwd = flags(str(ROOT / RECORD), source, obj)
    run(command, cwd=cwd)
    archive = OUT / 'libPicassoCpuFreq.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive, obj])
    print('picasso-cpufreq:', archive)


if __name__ == '__main__':
    main()
