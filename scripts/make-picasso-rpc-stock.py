#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Recreate the bounded DSP stock filesystem from byte-audited factory files."""
import argparse
import hashlib
import json
from pathlib import Path

def generate(manifest, root):
    files=manifest['files'];assert 0<len(files)<=128
    names=set();text='// Generated from the user-provided V13.0.5.0.SGICNXM factory images.\n// Original vendor data, byte-preserved; not per-device calibration.\n// Regenerate with scripts/make-picasso-rpc-stock.py from audited inputs.\n'
    for i,f in enumerate(files):
        p=f['path'];assert p.startswith(('/vendor/etc/sensors/','/mnt/vendor/persist/sensors')) and p not in names and len(p)<256
        assert '..' not in Path(p).parts;names.add(p)
        if f['directory']:assert f['size']==0 and f['mode']&0o170000==0o040000;continue
        b=(root/p.lstrip('/')).read_bytes();assert len(b)==f['size']<=8192 and hashlib.sha256(b).hexdigest()==f['sha256'],p
        assert f['mode']&0o170000==0o100000
        text+=f'static const uint8_t rpcStockData{i}[]={{\n'
        for off in range(0,len(b),24):text+=','.join(str(x) for x in b[off:off+24])+',\n'
        text+='0};\n'
    text+='static const RpcStock rpcStock[]={\n'
    for i,f in enumerate(files):
        text+='{"'+f['path']+'",'+str(f['mode'])+','+str(f['size'])+','+('nullptr' if f['directory'] else f'rpcStockData{i}')+'},\n'
    return text+'};\n'

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--root',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--verify',action='store_true');a=p.parse_args()
    b=generate(json.loads(a.manifest.read_text()),a.root).encode()
    if a.verify:assert a.output.read_bytes()==b
    else:a.output.write_bytes(b)
    print('PASS stock bytes',hashlib.sha256(b).hexdigest())
if __name__=='__main__':main()
