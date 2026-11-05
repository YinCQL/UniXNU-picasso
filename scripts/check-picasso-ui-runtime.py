#!/usr/bin/env python3
"""Compile replayed UI directory/MAC helpers with host API models; no SDK or boot qualification."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xnu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('buttons', ROOT / 'scripts/check-picasso-buttons.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    files = {name: (args.xnu / name).read_text() for name in
             ('bsd/kern/kern_exec.c', 'security/mac_file.c', 'security/mac_internal.h', 'security/mac_base.c')}
    x = files['bsd/kern/kern_exec.c']
    view = '\n'.join(module.extract(x, name) for name in
        ('static int\npicasso_ui_library_directory(', 'static int\npicasso_ui_library_prepare(',
         'static void\npicasso_ui_library_mount('))
    (out / 'ui-view.inc').write_text(view, newline='\n')
    header = files['security/mac_internal.h']
    macros = header[header.index('#define MAC_POLICY_ITERATE'):header.index('/*\n * MAC_GRANT')]
    mac = module.extract(files['security/mac_base.c'], 'int\nmac_error_select(')
    mac += '\n' + module.extract(files['security/mac_file.c'], 'int\nmac_file_check_mmap(')
    (out / 'ui-mac.inc').write_text(macros + mac, newline='\n')
    cc = shutil.which('clang') or (r'C:\Program Files\LLVM\bin\clang.exe' if os.name == 'nt' else None)
    runs = []
    for fixture in ('picasso-ui-view-host.c', 'picasso-ui-mac-host.c'):
        for sanitized in (False, True):
            name = Path(fixture).stem + ('-sanitized' if sanitized else '')
            exe = out / (name + ('.exe' if os.name == 'nt' else ''))
            cmd = [cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-I', str(out),
                   str(ROOT / 'scripts/tests' / fixture), '-o', str(exe)]
            if sanitized:
                cmd += ['-fsanitize=address' if os.name == 'nt' else '-fsanitize=address,undefined']
            for stage, command in (('compile', cmd), ('run', [str(exe)])):
                result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
                (out / (name + '-' + stage + '.log')).write_text(result.stdout + result.stderr)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                runs.append(dict(name=name, stage=stage, passed=True))
    (out / 'results.json').write_text(json.dumps(dict(scope=__doc__, passed=True, runs=runs,
        sources={name: hashlib.sha256(text.encode()).hexdigest() for name, text in files.items()}), indent=2) + '\n')
    print('PASS replayed UI mount validation and MAC verdict equivalence, normal and sanitizer')


if __name__ == '__main__':
    main()
