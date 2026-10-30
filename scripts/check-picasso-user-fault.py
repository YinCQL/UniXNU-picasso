#!/usr/bin/env python3
"""Compile and exercise generated user-fault helpers with an opaque BSD task_t.

Requires an already replayed XNU tree. The helper and dyld types are extracted
without edits. Host API/atomic stand-ins do not validate Apple SDK compilation,
ARM64 execution, or device behavior. Output must be a new directory; all failed
and successful compiler artifacts are preserved. No tools are installed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

CASE_COUNT = 72


def one(pattern, source):
    matches = list(re.finditer(pattern, source, re.S | re.M))
    if len(matches) != 1:
        raise ValueError(f'Expected one match, got {len(matches)}: {pattern}')
    return matches[0].group()


def extract(xnu, out):
    kernel = (xnu / 'bsd/kern/kern_exit.c').read_text(encoding='utf-8')
    sleh = (xnu / 'osfmk/arm64/sleh.c').read_text(encoding='utf-8')
    dyld = (xnu / 'osfmk/kdp/kdp_dyld.h').read_text(encoding='utf-8')
    capture = one(r'/\* PICASSO_USER_FAULT_BEGIN.*?/\* PICASSO_USER_FAULT_END \*/', kernel)
    ticket = one(r'^static unsigned\npicasso_pid1_trace_ticket\(.*?\n}', kernel)
    signature = 'void picasso_user_breakpoint(void *, uint32_t, uint32_t, uint64_t);'
    if signature not in capture or 'extern ' + signature not in sleh:
        raise ValueError('BSD and ARM64 declarations must accept the same four arguments')
    one(r'^picasso_user_breakpoint\(void \*saved, uint32_t esr, uint32_t shared_cache_slide, uint64_t main_header\)', capture)
    call = one(r'^\s*picasso_user_breakpoint\(state, esr, current_task\(\)->task_shared_region_slide,\s*'
               r'current_task\(\)->mach_header_vm_address\);', sleh)
    if sleh.index(call) > sleh.index('boot_progress_user_breakpoint(state, esr);'):
        raise ValueError('Capture must precede the existing breakpoint handling')
    if capture.count('f->slide = shared_cache_slide;') != 1:
        raise ValueError('BSD capture must preserve the supplied slide value')
    if capture.count('f->main_header = main_header;') != 1:
        raise ValueError('BSD capture must preserve the supplied executable header address')
    production = ticket + '\n' + capture + '\n'
    (out / 'actual-helper.inc').write_text(production, encoding='utf-8', newline='\n')
    types = '\n'.join(one(r'^struct ' + name + r' \{.*?^\};', dyld)
                      for name in ('user64_dyld_image_info', 'user64_dyld_all_image_infos')) + '\n'
    (out / 'dyld-types.h').write_text(types, encoding='utf-8', newline='\n')
    # Reintroduce only the failing private-field read. The same fixture must
    # reject this at compile time while compiling the real helper successfully.
    negative = production.replace('f->slide = shared_cache_slide;',
                                  'f->slide = current_task()->task_shared_region_slide;\n'
                                  '\t(void)shared_cache_slide;', 1)
    (out / 'private-field-negative.inc').write_text(negative, encoding='utf-8', newline='\n')
    digest = lambda value: hashlib.sha256(value.encode()).hexdigest()
    return {'source_sha256': {'kern_exit.c': digest(kernel), 'sleh.c': digest(sleh),
                             'kdp_dyld.h': digest(dyld)},
            'helper_sha256': digest(production), 'dyld_types_sha256': digest(types),
            'declarations_and_arm64_call_verified': True}


def find_vcvars():
    base = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
    vswhere = base / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if vswhere.is_file():
        found = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                                'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                '-property', 'installationPath'], capture_output=True, text=True, check=True)
        for line in found.stdout.splitlines():
            candidate = Path(line) / 'VC/Auxiliary/Build/vcvars64.bat'
            if candidate.is_file():
                return candidate
    fallback = base / 'Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat'
    return fallback if fallback.is_file() else None


def batch_run(out, name, commands, vcvars):
    batch = out / (name + '.cmd')
    batch.write_text('@echo off\nsetlocal\n'
                     f'call "{vcvars}" > "%~dp0{name}-toolchain.log" 2>&1\n'
                     'if not "%errorlevel%"=="0" exit /b %errorlevel%\ncd /d "%~dp0"\n' + commands,
                     encoding='utf-8', newline='\r\n')
    return subprocess.run(['cmd', '/d', '/c', batch.name], cwd=out, capture_output=True, text=True)


def compile_case(out, name, source, asan, cc, vcvars):
    log = out / (name + '-compile.log')
    exe = name + ('.exe' if os.name == 'nt' else '')
    if vcvars:
        command = f'cl /nologo /std:c11 /W4 /WX /Od /Zi /Fd:{name}.pdb'
        if asan:
            command += ' /fsanitize=address'
        command += f' /Fo:{name}.obj /Fe:{exe} {source}'
        if asan:
            command += ' /link /INCREMENTAL:NO'
        result = batch_run(out, name + '-compile',
                           f'{command} > {log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
    else:
        command = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O0', '-g']
        if asan:
            command += ['-fsanitize=address', '-fno-omit-frame-pointer']
        command += [source, '-o', exe]
        with log.open('w', encoding='utf-8') as output:
            result = subprocess.run(command, cwd=out, stdout=output, stderr=subprocess.STDOUT)
    return {'name': name, 'asan': asan, 'command': command, 'exit': result.returncode,
            'log': str(log)}, exe


def run_cases(out, name, exe, vcvars):
    log = out / (name + '-run.log')
    if vcvars:
        # Configure the runtime PATH for execution too: a child compiler cannot
        # put the MSVC ASan DLL directory on this Python process's PATH.
        commands = ''.join(f'{exe} {case} >> {log.name} 2>&1\n'
                           'if not "%errorlevel%"=="0" exit /b %errorlevel%\n'
                           for case in range(CASE_COUNT)) + 'exit /b 0\n'
        result = batch_run(out, name + '-run', commands, vcvars)
        code = result.returncode
    else:
        code = 0
        with log.open('w', encoding='utf-8') as output:
            for case in range(CASE_COUNT):
                result = subprocess.run([str(out / exe), str(case)], cwd=out,
                                        stdout=output, stderr=subprocess.STDOUT)
                if result.returncode:
                    code = result.returncode
                    break
    text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
    cases = list(map(int, re.findall(r'^PASS scenario=(\d+) ', text, re.M)))
    return {'exit': code, 'passed_cases': cases, 'all_pass': code == 0 and cases == list(range(CASE_COUNT)),
            'log': str(log)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True, help='Complete replayed XNU source tree')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    parser.add_argument('--cc', help='C compiler (clang/gcc); Windows defaults to installed MSVC')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    vcvars = find_vcvars() if os.name == 'nt' and not args.cc else None
    cc = args.cc or shutil.which('clang') or shutil.which('gcc')
    if not vcvars and not cc:
        parser.error('No installed MSVC, clang or gcc found; no tools were installed')
    report = extract(args.xnu.resolve(), out)
    fixture = Path(__file__).parent / 'tests/picasso-user-fault-host.c'
    source = fixture.read_text(encoding='utf-8')
    if re.search(r'\bstruct\s+task\s*\{', source) or 'typedef struct task *task_t;' not in source:
        raise ValueError('Fixture must keep struct task incomplete')
    if source.count('#include "actual-helper.inc"') != 1:
        raise ValueError('Fixture must include the actual helper once')
    (out / 'fault-host.c').write_text(source, encoding='utf-8', newline='\n')
    (out / 'private-field-negative.c').write_text(
        source.replace('#include "actual-helper.inc"', '#include "private-field-negative.inc"'),
        encoding='utf-8', newline='\n')
    report.update(fixture_sha256=hashlib.sha256(source.encode()).hexdigest(), cases=CASE_COUNT, runs=[],
                  scope='Extracted helpers with opaque task_t and host API/atomic stand-ins; '
                        'not a full Apple SDK build, ARM64 execution, or device validation.')
    report_path = out / 'results.json'
    try:
        for name, source_name, asan, negative in (
            ('fault-host', 'fault-host.c', False, False),
            ('private-field-negative', 'private-field-negative.c', False, True),
            ('fault-host-asan', 'fault-host.c', True, False),
        ):
            compiled, exe = compile_case(out, name, source_name, asan, cc, vcvars)
            entry = {'compile': compiled}
            report['runs'].append(entry)
            if negative:
                log = Path(compiled['log']).read_text(encoding='utf-8', errors='replace')
                rejected = compiled['exit'] != 0 and bool(re.search(
                    r'(?is)(?:C2027|C2037).*?task|(?:incomplete|undefined).*?struct task', log))
                entry['private_field_access_rejected'] = rejected
                if not rejected:
                    raise RuntimeError('Private struct task field negative did not fail for the expected type error')
            else:
                if compiled['exit']:
                    raise RuntimeError(f'{name} failed compilation; see {compiled["log"]}')
                entry['run'] = run_cases(out, name, exe, vcvars)
                if not entry['run']['all_pass']:
                    raise RuntimeError(f'{name} did not pass all {CASE_COUNT} cases; see {entry["run"]["log"]}')
            report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        report['pass'] = True
        print(f'PASS: {CASE_COUNT} normal + {CASE_COUNT} ASan cases; private-field compile negative rejected. {report_path}')
        return 0
    except Exception as error:
        report.update({'pass': False, 'error': str(error)})
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    sys.exit(main())
