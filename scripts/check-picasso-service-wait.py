#!/usr/bin/env python3
"""Exercise extracted watchdog diagnostics and XNU's actual user unwinder.

Host API/queue/lock/VM models are not an SDK build or device qualification.
Requires an existing replayed tree and installed Clang; preserves all outputs.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path


def one(pattern, text):
    found = re.findall(pattern, text, re.S | re.M)
    assert len(found) == 1, (pattern, len(found))
    return found[0]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--xnu', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--cc', default=shutil.which('clang'))
    a = p.parse_args()
    if not a.cc:
        p.error('An installed Clang is required; nothing will be installed')
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = {path: (a.xnu / path).read_text(encoding='utf-8') for path in (
        'bsd/kern/kern_exit.c', 'osfmk/kern/task.c', 'osfmk/kern/backtrace.c',
        'osfmk/kern/backtrace.h', 'osfmk/vm/vm_fault.c', 'osfmk/arm64/sleh.c')}
    gate = one(r'(/\* PICASSO_SERVICE_WAIT_BSD_BEGIN.*?/\* PICASSO_SERVICE_WAIT_BSD_END \*/)', source['bsd/kern/kern_exit.c'])
    helper = one(r'(/\* PICASSO_SERVICE_WAIT_TASK_BEGIN.*?/\* PICASSO_SERVICE_WAIT_TASK_END \*/)', source['osfmk/kern/task.c'])
    ticket = one(r'(static unsigned\npicasso_pid1_trace_ticket\(.*?\n})', source['bsd/kern/kern_exit.c'])
    unwinder = one(r'(static errno_t\n_backtrace_copyin\(.*?^unsigned int\nbacktrace_user\(.*?^})', source['osfmk/kern/backtrace.c'])
    # The host runs the exact native walker, not a fixture that invents frames.
    for name, value in [('gate.inc', ticket + '\n' + gate), ('task.inc', helper), ('unwinder.inc', unwinder)]:
        (out / name).write_text(value, encoding='utf-8', newline='\n')
    assert '->task_shared_region_slide' not in gate and '->mach_header_vm_address' not in gate
    assert 'btc_user_thread =' not in helper and 'thread_get_state(' not in helper
    assert helper.index('task_unlock(task);') < helper.index('printf(')
    assert helper.index('disable_preemption();') < helper.index('copyin(src, dst, size)') < helper.index('enable_preemption();')
    assert 'if (get_preemption_level() != 0)' in source['osfmk/vm/vm_fault.c']
    nofault = source['osfmk/vm/vm_fault.c'].split('if (get_preemption_level() != 0)', 1)[1].split('\n\t}', 1)[0]
    assert 'return KERN_FAILURE;' in nofault
    kernel = source['bsd/kern/kern_exit.c']
    prepare = kernel[kernel.index('\nproc_prepareexit(proc_t p, int rv, boolean_t perf_notify)'):]
    assert prepare.index('picasso_report_service_wait(p, rv);') < prepare.index('task_exception_notify(')
    exit_func = kernel[kernel.index('\nexit_with_reason(proc_t p,'):kernel.index('\n#if CONFIG_MEMORYSTATUS', kernel.index('\nexit_with_reason(proc_t p,'))]
    assert exit_func.index('proc_prepareexit(p, rv, perf_notify);') < exit_func.index('task_terminate_internal(task);')
    report = {'scope': __doc__, 'source_sha256': {k: hashlib.sha256(v.encode()).hexdigest() for k, v in source.items()},
              'helper_sha256': hashlib.sha256(helper.encode()).hexdigest(), 'runs': [], 'pass': False}
    try:
        for fixture, cases in [('service-wait-gate', 14), ('service-wait-task', 20)]:
            text = (Path(__file__).parent / 'tests' / ('picasso-' + fixture + '.c')).read_text(encoding='utf-8')
            (out / (fixture + '.c')).write_text(text, encoding='utf-8', newline='\n')
            for asan in (False, True):
                name = fixture + ('-asan' if asan else '')
                exe = out / (name + '.exe')
                cmd = [a.cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-O0', '-g',
                       str(out / (fixture + '.c')), '-o', str(exe)]
                if asan:
                    cmd += ['-fsanitize=address', '-fno-omit-frame-pointer']
                r = subprocess.run(cmd, capture_output=True, text=True)
                (out / (name + '-compile.log')).write_text(r.stdout + r.stderr)
                entry = {'name': name, 'command': cmd, 'compile_exit': r.returncode, 'passed_cases': []}
                report['runs'].append(entry)
                assert r.returncode == 0, name + ' compile failed'
                with (out / (name + '-run.log')).open('w', encoding='utf-8') as log:
                    for case in range(cases):
                        r = subprocess.run([str(exe), str(case)], stdout=log, stderr=subprocess.STDOUT)
                        assert r.returncode == 0, f'{name} case {case} failed'
                        entry['passed_cases'].append(case)
        report['pass'] = True
        print('PASS: 34 normal + 34 ASan gate/walker/lock/VM cases')
    finally:
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
