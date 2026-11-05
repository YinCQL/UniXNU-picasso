#!/usr/bin/env python3
"""Check decoding/fallback on regular local dumps; no device access."""
import argparse,importlib.util,json,struct,subprocess,sys,zlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def slot(kind,capacity,generation,data):
    header=bytearray(4096);header[:8]=b'PCRCD001'
    struct.pack_into('<IIQII',header,8,kind,capacity,generation,zlib.crc32(data),len(data))
    struct.pack_into('<I',header,32,zlib.crc32(header[:32]))
    return header+data+bytes(capacity-len(data))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    path=ROOT/'scripts/read-picasso-records.py';spec=importlib.util.spec_from_file_location('reader',path)
    reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
    cases=[]
    for name,kind,capacity in [('nvram',1,8192),('log',2,262144),('aux-owner',3,4096)]:
        first=slot(kind,capacity,7,b'old');second=slot(kind,capacity,8,b'new')
        dump=out/(name+'.bin');dump.write_bytes(bytes(19)+first+second)
        records=reader.records(dump,19,kind,capacity)
        assert all(v['valid'] for v in records) and max(records,key=lambda v:v['generation'])['data']==b'new'
        target=out/(name+'.decoded');cmd=[sys.executable,str(path),str(dump),'--offset','19','--kind',name,'--output',str(target)]
        result=subprocess.run(cmd,capture_output=True);assert not result.returncode and target.read_bytes()==b'new'
        assert subprocess.run(cmd,capture_output=True).returncode and target.read_bytes()==b'new'
        second[4096]^=1;dump.write_bytes(bytes(19)+first+second)
        records=reader.records(dump,19,kind,capacity);assert records[0]['valid'] and not records[1]['valid']
        dump.write_bytes(bytes(19)+first+second[:4200]);records=reader.records(dump,19,kind,capacity)
        assert records[0]['valid'] and not records[1]['valid']
        dump.write_bytes(bytes(19+2*(4096+capacity)));assert not any(v['valid'] for v in reader.records(dump,19,kind,capacity))
        cases.append(name)
    (out/'results.json').write_text(json.dumps(dict(passed=True,kinds=cases,checks=['latest','CRC fallback','truncation','blank','no overwrite']),indent=2)+'\n')
    print('PASS: local record reader, CRC fallback and exclusive output')
if __name__=='__main__':main()
