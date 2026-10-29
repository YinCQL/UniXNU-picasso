#!/usr/bin/env python3
"""Run the actual IOKit trace helper with host IOKit/process stand-ins."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
CASE_COUNT = 7


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('host', ROOT / 'scripts/check-picasso-user-fault.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    vcvars = host.find_vcvars()
    if not vcvars:
        raise RuntimeError('This host check requires installed MSVC')
    source = (args.xnu / 'iokit/Kernel/IOUserClient.cpp').read_text(encoding='utf-8')
    blocks = re.findall(r'/\* PICASSO_SERVICE_TRACE_BEGIN \*/.*?/\* PICASSO_SERVICE_TRACE_END \*/', source, re.S)
    assert len(blocks) == 12
    assert 'struct picasso_service_trace' in blocks[0]
    (out / 'service-trace.inc').write_text(blocks[0], encoding='utf-8', newline='\n')
    fixture = (ROOT / 'scripts/tests/picasso-service-trace-host.cpp').read_text(encoding='utf-8')
    (out / 'trace.cpp').write_text(fixture, encoding='utf-8', newline='\n')
    report = {'scope': __doc__, 'helper_sha256': hashlib.sha256(blocks[0].encode()).hexdigest(),
              'fixture_sha256': hashlib.sha256(fixture.encode()).hexdigest(), 'runs': []}
    for name, asan in [('trace', False), ('trace-asan', True)]:
        flags = ' /fsanitize=address' if asan else ''
        cmd = f'cl /nologo /std:c++17 /EHsc /W4 /WX /Od /Zi{flags} /Fd:{name}.pdb /Fo:{name}.obj /Fe:{name}.exe trace.cpp /link /INCREMENTAL:NO'
        result = host.batch_run(out, name + '-compile', cmd + f' > {name}-compile.log 2>&1\nexit /b %errorlevel%\n', vcvars)
        if result.returncode:
            raise RuntimeError('Compile failed: ' + str(out / (name + '-compile.log')))
        commands = ''.join(f'{name}.exe {case} >> {name}-run.log 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\n' for case in range(CASE_COUNT))
        result = host.batch_run(out, name + '-run', commands + 'exit /b 0\n', vcvars)
        cases = list(map(int, re.findall(r'^PASS scenario=(\d+) ', (out / (name + '-run.log')).read_text(), re.M)))
        assert result.returncode == 0 and cases == list(range(CASE_COUNT))
        report['runs'].append({'asan': asan, 'cases': cases, 'pass': True})
    report['pass'] = True
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: ordinary and ASan, {CASE_COUNT} trace/bounds/concurrency scenarios each')


if __name__ == '__main__':
    main()
