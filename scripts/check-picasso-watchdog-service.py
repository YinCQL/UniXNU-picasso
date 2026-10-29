#!/usr/bin/env python3
"""Check production watchdog lifecycle ordering with host IOKit/policy stand-ins.

Extracts the native class and unchanged lifecycle/client methods. Original kext
loading and metaclass enumeration are checked by the builder/runtime guards,
not this host fixture. No SDK or device validation is implied.
"""
import argparse
import importlib.util
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def method(source, name):
    start = re.search(r'^\S[^\n]*PicassoWatchdog::' + name + r'\(', source, re.M).start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = (ROOT / 'package/PicassoWatchdog.cpp').read_text(encoding='utf-8')
    production = source[source.index('class PicassoWatchdog'):source.index('// Only the native subclass')]
    for name in ('getWorkLoop', 'newUserClient', 'scheduleNextCheck', 'enableWatchdogHardware',
                 'disableWatchdogHardware', 'petWatchdogHardware', 'timerFired', 'start', 'free'):
        production += method(source, name)
    fixture = (ROOT / 'scripts/tests/picasso-watchdog-service-host.cpp').read_text(encoding='utf-8')
    spec = importlib.util.spec_from_file_location('platform_host', ROOT / 'scripts/check-picasso-boot-state.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.run_host(out, source, production, fixture, 'watchdog-lifecycle-production.inc', 10)


if __name__ == '__main__':
    main()
