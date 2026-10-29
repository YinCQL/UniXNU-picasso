#!/usr/bin/env python3
"""Exercise the production SM7250 watchdog backend with host MMIO/IRQ stand-ins.

Extracts unchanged functions from the canonical USB patch. Uses only installed
MSVC or clang++/g++; preserves every output in a new directory. These tests do
not validate hardware register timing, an Apple SDK build, or phone behavior.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def function(source, name):
    match = re.search(r'^(?:static )?(?:void|bool|uint64_t)\n' + re.escape(name) + r'\(', source, re.M)
    if not match:
        raise ValueError(f'Missing function: {name}')
    cursor = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[match.start():cursor] + '\n'


def extract(device, out):
    patch = (device / 'patches/picasso-usb-console-xnu.patch').read_text(encoding='utf-8')
    start = patch.index('+++ b/bsd/dev/picasso_usb.c\n')
    begin = patch.index('@@ -0,0 +1,', start)
    end = patch.index('\ndiff --git ', begin)
    rows = patch[begin:end].splitlines()
    count = int(re.search(r'\+1,(\d+)', rows[0])[1])
    assert len(rows) - 1 == count and all(row.startswith('+') for row in rows[1:])
    source = '\n'.join(row[1:] for row in rows[1:]) + '\n'
    (out / 'picasso_usb.c').write_text(source, encoding='utf-8', newline='\n')
    backend = re.search(r'/\* PICASSO_WATCHDOG_BACKEND_BEGIN.*?/\* PICASSO_WATCHDOG_BACKEND_END \*/',
                        source, re.S).group()
    # All hardware writers must belong to the shared ownership implementation.
    outside = source.replace(backend, '', 1)
    assert not re.search(r'usb_wr\(ud\.wdt,', outside), 'Unserialized WDT writer outside backend'
    for name in ('picasso_usb_panic_begin', 'picasso_usb_panic_reset', 'picasso_usb_system_reset',
                 'picasso_usb_pshold_reset', 'picasso_usb_psci_reset', 'picasso_usb_watchdog_reset',
                 'ud_finish_reboot', 'ud_finish_hang'):
        body = function(source, name)
        assert re.search(r'\{\s*picasso_usb_watchdog_terminal\(\);', body), name
    for field in ('panic_pending', 'reset_pending', 'hang_pending', 'reboot_pending'):
        assert source.count(f'ud.{field} = true;') == 1
        assert f'picasso_usb_watchdog_terminal();\n\t\tud.{field} = true;' in source
    header = (device / 'package/PicassoWatchdogBackend.h').read_text(encoding='utf-8')
    for name in ('claim', 'enable', 'disable', 'pet'):
        declaration = f'bool picasso_watchdog_backend_{name}(const void *owner);'
        assert declaration in source and declaration in header
    (out / 'PicassoWatchdogBackend.h').write_text(header, encoding='utf-8', newline='\n')
    constants = '\n'.join(re.findall(r'^#define APSS_WDT_[^\n]+', source, re.M))
    assert '#define APSS_WDT_TIMEOUT_MS 15000u' in constants
    assert '#define APSS_WDT_RATE_HZ    32768u' in constants
    definitions = source[source.index('#define UD_WATCHDOG_INITIAL_REPORTS'):
                         source.index('static uint64_t\nud_monotonic_ns')]
    names = ['ud_command_is', 'ud_process_command', 'picasso_usb_panic_begin',
             'ud_poll_serialized', 'picasso_usb_console_drive_poll',
             'ud_monotonic_ns', 'ud_timer_irq_snapshot', 'ud_background_poll']
    production = '\n'.join([constants, backend, definitions] + [function(source, name) for name in names])
    (out / 'backend-production.inc').write_text(production, encoding='utf-8', newline='\n')
    digest = lambda text: hashlib.sha256(text.encode()).hexdigest()
    return {'patch_sha256': digest(patch), 'source_sha256': digest(source),
            'backend_sha256': digest(backend), 'production_sha256': digest(production),
            'header_sha256': digest(header), 'new_file_hunk_lines': count,
            'all_wdt_writes_serialized': True, 'terminal_entry_points_verified': True}


def main():
    device = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New directory; must not exist')
    parser.add_argument('--cxx', help='C++ compiler; defaults to installed MSVC on Windows')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    # Reuse only the established process/toolchain helpers, never its old fixture.
    spec = importlib.util.spec_from_file_location('fault_runner', device / 'scripts/check-picasso-user-fault.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    vcvars = runner.find_vcvars() if os.name == 'nt' and not args.cxx else None
    cxx = args.cxx or shutil.which('clang++') or shutil.which('g++')
    if not vcvars and not cxx:
        parser.error('No installed MSVC/clang++/g++; no tools were installed')
    report = extract(device, out)
    fixture = (device / 'scripts/tests/picasso-watchdog-backend-host.cpp').read_text(encoding='utf-8')
    (out / 'backend-host.cpp').write_text(fixture, encoding='utf-8', newline='\n')
    report.update(fixture_sha256=hashlib.sha256(fixture.encode()).hexdigest(), runs=[],
                  scope='Unchanged production backend and USB progress helpers; simulated MMIO, IRQ state, '
                        'atomic primitives, commands and scheduling. Not Apple SDK or device validation.')
    report_path = out / 'results.json'
    try:
        for name, asan in [('backend-host', False), ('backend-host-asan', True)]:
            compile_log = out / (name + '-compile.log')
            run_log = out / (name + '-run.log')
            exe = name + ('.exe' if os.name == 'nt' else '')
            entry = {'name': name, 'asan': asan}
            report['runs'].append(entry)
            if vcvars:
                command = f'cl /nologo /std:c++17 /EHsc /W4 /WX /Od /Zi /Fd:{name}.pdb'
                if asan:
                    command += ' /fsanitize=address'
                command += f' /Fo:{name}.obj /Fe:{exe} backend-host.cpp'
                if asan:
                    command += ' /link /INCREMENTAL:NO'
                compiled = runner.batch_run(out, name + '-compile',
                    f'{command} > {compile_log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                command = [cxx, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror', '-O0', '-g']
                if asan:
                    command += ['-fsanitize=address', '-fno-omit-frame-pointer']
                command += ['backend-host.cpp', '-o', exe]
                with compile_log.open('w', encoding='utf-8') as log:
                    compiled = subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT)
            entry.update(compile_command=command, compile_exit=compiled.returncode, compile_log=str(compile_log))
            if compiled.returncode:
                raise RuntimeError(f'Compilation failed: {compile_log}')
            if vcvars:
                ran = runner.batch_run(out, name + '-run',
                    f'{exe} > {run_log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
            else:
                with run_log.open('w', encoding='utf-8') as log:
                    ran = subprocess.run([str(out / exe)], cwd=out, stdout=log, stderr=subprocess.STDOUT)
            output = run_log.read_text(encoding='utf-8', errors='replace') if run_log.exists() else ''
            entry.update(run_exit=ran.returncode, run_log=str(run_log),
                         passed_groups=re.findall(r'^PASS: (.+)$', output, re.M))
            if ran.returncode or 'ALL BACKEND CHECKS PASSED' not in output:
                raise RuntimeError(f'Host checks failed: {run_log}')
            print(f'{name}: {len(entry["passed_groups"])} groups passed')
            report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        report['pass'] = True
        print(f'PASS: {report_path}')
    except Exception as error:
        report.update({'pass': False, 'error': str(error)})
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
