#!/usr/bin/env python3
"""Decode a saved local userdata dump or board record window; never opens a device."""
import argparse,json,struct,zlib
from pathlib import Path
BLOCK=4096

def records(path,offset,kind,capacity):
    result=[]
    with path.open('rb') as f:
        for slot in range(2):
            f.seek(offset+slot*(BLOCK+capacity));h=f.read(BLOCK);body=f.read(capacity)
            valid=len(h)==BLOCK and len(body)==capacity and h[:8]==b'PCRCD001'
            if valid:
                k,cap,gen,crc,size,headercrc=struct.unpack_from('<IIQIII',h,8)
                valid=(k==kind and cap==capacity and 0<size<=capacity and gen>0 and
                       zlib.crc32(h[:32])==headercrc and not any(h[36:]) and zlib.crc32(body[:size])==crc)
            result.append(dict(slot=slot,valid=valid,**({'generation':gen,'bytes':size,'data':body[:size]} if valid else {})))
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('dump',type=Path);p.add_argument('--offset',type=lambda s:int(s,0),required=True,help='Byte offset in this regular file; absolute device LBA is not assumed')
    p.add_argument('--kind',choices=['nvram','log','aux-owner'],required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if not a.dump.is_file() or a.dump.is_symlink() or a.offset<0:p.error('Require a regular local dump and nonnegative offset')
    kind,capacity={'nvram':(1,8192),'log':(2,262144),'aux-owner':(3,4096)}[a.kind]
    slots=records(a.dump,a.offset,kind,capacity);good=[v for v in slots if v['valid']]
    if not good:raise SystemExit('No complete CRC-valid record; input left untouched')
    latest=max(good,key=lambda v:v['generation'])
    with a.output.open('xb') as f:f.write(latest['data'])
    print(json.dumps(dict(kind=a.kind,selected_slot=latest['slot'],slots=[{k:v for k,v in s.items() if k!='data'} for s in slots]),indent=2))
if __name__=='__main__':main()
