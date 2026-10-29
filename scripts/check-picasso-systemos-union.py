#!/usr/bin/env python3
"""Exercise actual XNU union mount/preflight and root-reference functions on MSVC."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index('static int\n' + signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('host', ROOT / 'scripts/check-picasso-user-fault.py')
    host = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(host)
    vcvars = host.find_vcvars()
    if not vcvars:
        raise RuntimeError('This host check requires installed MSVC')
    files = {'root': 'bsd/vfs/vfs_syscalls.c', 'mount': 'bsd/kern/kern_exec.c'}
    sources = {key: (args.xnu / name).read_text(encoding='utf-8') for key, name in files.items()}
    helpers = {
        'root': function(sources['root'], 'checkdirs_callback(proc_t p, void * arg)') +
                function(sources['root'], 'checkdirs(vnode_t olddp, vfs_context_t ctx)'),
        'mount': function(sources['mount'], 'native_owner_union_path(const char *path, mount_t expected)') +
                 function(sources['mount'], 'native_owner_cache_prepare(proc_t p, user_addr_t scratch, user_addr_t *envp)'),
    }
    report = {'scope': 'Extracted production functions with host VFS/owner/copyout stand-ins; not Apple SDK or device execution',
              'source_sha256': {files[k]: hashlib.sha256(v.encode()).hexdigest() for k, v in sources.items()},
              'helpers': {}, 'runs': []}
    for kind, count in [('root', 8), ('mount', 23)]:
        helper = helpers[kind]
        (out / (kind + '-functions.inc')).write_text(helper, encoding='utf-8', newline='\n')
        fixture = (ROOT / f'scripts/tests/picasso-union-{kind}-host.c').read_text(encoding='utf-8')
        (out / (kind + '.c')).write_text(fixture, encoding='utf-8', newline='\n')
        report['helpers'][kind] = {'sha256': hashlib.sha256(helper.encode()).hexdigest(),
                                   'fixture_sha256': hashlib.sha256(fixture.encode()).hexdigest(), 'cases': count}
        for asan in [False, True]:
            name = kind + ('-asan' if asan else '')
            flags = ' /fsanitize=address' if asan else ''
            command = f'cl /nologo /std:c11 /W4 /WX /Od /Zi{flags} /Fd:{name}.pdb /Fo:{name}.obj /Fe:{name}.exe {kind}.c /link /INCREMENTAL:NO'
            result = host.batch_run(out, name + '-compile', command + f' > {name}-compile.log 2>&1\nexit /b %errorlevel%\n', vcvars)
            if result.returncode:
                raise RuntimeError('Compile failed: ' + str(out / (name + '-compile.log')))
            commands = ''.join(f'{name}.exe {case} >> {name}-run.log 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\n' for case in range(count))
            result = host.batch_run(out, name + '-run', commands + 'exit /b 0\n', vcvars)
            cases = list(map(int, re.findall(r'^PASS scenario=(\d+)', (out / (name + '-run.log')).read_text(), re.M)))
            assert result.returncode == 0 and cases == list(range(count)), str(out / (name + '-run.log'))
            report['runs'].append({'kind': kind, 'asan': asan, 'passed_cases': cases})
    # Restore only the historical cwd transfer to reproduce the usecount error.
    begin = helpers['root'].index('\tif (fdp->fd_cdir == olddp) {', helpers['root'].index('proc_dirs_lock_exclusive(p)'))
    end = helpers['root'].index('\n\tif (fdp->fd_rdir', begin)
    historical = helpers['root'][:begin] + '''\tif (fdp->fd_cdir == olddp) {
\t\told_cvp = olddp;
\t\tfdp->fd_cdir = newdp;
\t\tnew_cvp = NULL;
\t}''' + helpers['root'][end:]
    (out / 'root-historical.inc').write_text(historical, encoding='utf-8', newline='\n')
    fixture = (out / 'root.c').read_text().replace('"root-functions.inc"', '"root-historical.inc"')
    (out / 'root-historical.c').write_text(fixture, encoding='utf-8', newline='\n')
    command = 'cl /nologo /std:c11 /W4 /WX /Od /Fe:root-historical.exe root-historical.c > root-historical-compile.log 2>&1\nif not "%errorlevel%"=="0" exit /b %errorlevel%\nroot-historical.exe 0 > root-historical-run.log 2>&1\nexit /b %errorlevel%\n'
    result = host.batch_run(out, 'root-historical', command, vcvars)
    assert result.returncode == 77 and 'Detected old root usecount undercount' in (out / 'root-historical-run.log').read_text()
    report.update(historical_root_undercount_reproduced=True, passed=True)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS: 8 root-reference and 23 mount/preflight cases, ordinary and ASan; historical undercount reproduced')


if __name__ == '__main__':
    main()
