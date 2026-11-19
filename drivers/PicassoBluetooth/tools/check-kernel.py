#!/usr/bin/env python3
"""Compile the Bluetooth core with the selected XNU's recorded BSD command."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('tree', type=Path, help='assembled target tree with a built XNU')
p.add_argument('--record', type=Path, required=True, help='JSON compile command of a board kernel source, relative to the tree')
p.add_argument('--extra-source', type=Path, action='append', default=[])
p.add_argument('--board-export', action='append', default=[], help='symbol an extra source may export')
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
tree = args.tree.resolve()
record = tree/args.record
command = json.loads(record.read_text())
bsd_record = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json'
cwd = Path(json.loads('['+bsd_record.read_text().rstrip().rstrip(',')+']')[0]['directory'])
subprocess.run([sys.executable, root/'tools/prepare-linux.py'], check=True)
out = root/'out/kernel-check'
out.mkdir(parents=True, exist_ok=True)
sources = ['src/qbt_core.c', 'src/qbt_linux.c', 'linux/btqca.c']
if (root/'src/qbt_geni.c').exists():
    sources.append('src/qbt_geni.c')
if (root/'src/qbt_transport.c').exists():
    sources.append('src/qbt_transport.c')
sources += [str(s.resolve()) for s in args.extra_source]
objects = []
for source in sources:
    obj = out/(Path(source).stem+'.o')
    cc = list(command)
    cc[cc.index('-xc')+1] = str(root/source)
    cc[cc.index('-o')+1] = str(obj)
    if '-MF' in cc:
        cc[cc.index('-MF')+1] = str(obj.with_suffix('.d'))
    cc += ['-I'+str(root/d) for d in ('include', 'src', 'shim', 'linux')]
    cc += ['-I'+str((root/source).parent)]
    cc += ['-Werror', '-Wno-unused-parameter', '-Wno-sign-compare']
    (out/(obj.stem+'.command.json')).write_text(json.dumps(cc, indent=2)+'\n')
    result = subprocess.run(cc, cwd=cwd, text=True, capture_output=True)
    (out/(obj.stem+'.log')).write_text(result.stdout+result.stderr)
    if result.returncode:
        raise SystemExit(result.stdout+result.stderr)
    objects.append(obj)
symbols = subprocess.check_output(['nm', '-g', *map(str, objects)], text=True)
(out/'symbols.txt').write_text(symbols)
defined = [line.split()[-1] for line in symbols.splitlines() if ' T ' in line or ' S ' in line or ' D ' in line]
sysctls = {'_sysctl__kern_qbt_'+s for s in ('boot', 'power_on', 'firmware')}
board_exports = set(args.board_export)
unscoped = [s for s in defined if not s.startswith('_qbt_') and s not in sysctls|board_exports]
if unscoped:
    raise SystemExit('Unscoped kernel exports: '+', '.join(unscoped))
kernel = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
availability = None
if kernel.is_file():
    kernel_nm = subprocess.check_output(['nm', str(kernel)], text=True)
    kernel_symbols = {l.split()[-1] for l in kernel_nm.splitlines() if l.split()}
    undefined = {l.split()[-1] for l in symbols.splitlines() if len(l.split())>=2 and l.split()[-2]=='U'}
    missing = sorted(undefined-set(defined)-kernel_symbols)
    availability = dict(kernel=str(kernel), sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(), missing=missing)
    if missing:
        raise SystemExit('References absent from the selected kernel: '+', '.join(missing))
(out/'report.json').write_text(json.dumps(dict(passed=True, hardware_verified=False,
    compile_record=str(record), inputs={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources},
    objects={o.name:hashlib.sha256(o.read_bytes()).hexdigest() for o in objects},
    defined_symbols=defined, kernel_symbol_availability=availability), indent=2)+'\n')
print('PASS: selected XNU BSD compile and isolated qbt_ exports ('+str(len(objects))+' objects)')
