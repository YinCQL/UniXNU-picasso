#!/usr/bin/env python3
"""Build the WCN399x board and transport into the qualified late archive."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

def build(root, platform, record, out):
    root=Path(root)
    driver=root/'drivers/PicassoBluetooth'
    subprocess.run([sys.executable,'-B',driver/'tools/prepare-linux.py'],check=True)
    c_record=json.loads((root/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
    # Apple clang records the implicit <source-stem>.o separately in output.
    # Materialize that exact default before the strict source/output adapter.
    if '-o' not in c_record['arguments']:
        if (c_record.get('output') != 'kern_exec.o' or
                Path(c_record['file']).name != 'kern_exec.c' or
                c_record['arguments'].count('-c') != 1 or
                any(a.startswith('-o') for a in c_record['arguments'])):
            raise ValueError('Unexpected implicit C compile output')
        c_record['arguments'] += ['-o', c_record['output']]
    inputs=[root/'kits/picasso/PicassoBluetooth.cpp',root/'kits/picasso/PicassoBluetoothPower.cpp']
    inputs += [driver/name for name in ['src/qbt_core.c','src/qbt_linux.c','src/qbt_geni.c','src/qbt_transport.c','src/PicassoBtFirmware.c','linux/btqca.c']]
    objects=[]
    for source in inputs:
        obj=out/(source.stem+'.o')
        recipe=record if source.suffix=='.cpp' else c_record
        cmd=platform.compile_command(recipe,source,obj,root)
        cmd += ['-I'+str(driver/d) for d in ['include','src','shim','linux']]
        cmd += ['-I'+str(root/'kits/picasso'),'-I'+str(root/'drivers/NativeAdrenoPicasso'),
                '-Wno-sign-compare','-Wno-misleading-indentation','-mgeneral-regs-only']
        platform.run(cmd,obj.with_suffix('.compile.log'),recipe['directory'])
        objects.append(obj)
    inputs += [p for p in driver.rglob('*') if p.is_file() and not any(x in p.parts for x in ('out','__pycache__'))
               and p.suffix in ('.h','.json','.license')]
    inputs += [root/'kits/picasso/PicassoBluetoothBoard.h',root/'scripts/build-picasso-bluetooth.py']
    return objects,inputs
