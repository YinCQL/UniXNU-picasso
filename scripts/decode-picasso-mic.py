#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Decode a complete, checked PICASSO_MIC capture from a serial log to WAV."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import wave

ROW=re.compile(r'PICASSO_MIC pcm offset=(\d+) bytes=(\d+) hex=([0-9a-f]+)(?:\s|$)')
END=re.compile(r'PICASSO_MIC pcm-end bytes=(\d+) fnv1a=([0-9a-f]{8}) rate=48000 channels=1 bits=16 little_endian=1')
CAPACITY=192000

def decode(text):
    data=bytearray();checksum=2166136261;complete=[];active=False
    for line in text.splitlines():
        if 'PICASSO_MIC configured=1' in line:
            data=bytearray();checksum=2166136261;active=True
        match=ROW.search(line)
        if match:
            offset,size=int(match[1]),int(match[2]);payload=bytes.fromhex(match[3])
            if offset==0:
                if data:raise ValueError("Duplicate microphone recording start")
                data=bytearray();checksum=2166136261;active=True
            if not active or offset!=len(data) or size!=len(payload) or not 0<size<=64 or size&1 or len(data)+size>CAPACITY:
                raise ValueError('Non-contiguous, duplicate, malformed or oversized microphone PCM row')
            data.extend(payload)
            for byte in payload:checksum=((checksum^byte)*16777619)&0xffffffff
        elif 'PICASSO_MIC pcm offset=' in line:
            raise ValueError('Malformed microphone PCM row')
        match=END.search(line)
        if match:
            if not active or not data or int(match[1])!=len(data) or int(match[2],16)!=checksum:
                raise ValueError('Incomplete microphone capture or checksum mismatch')
            complete.append(bytes(data));data=bytearray();active=False
        elif 'PICASSO_MIC pcm-end ' in line:
            raise ValueError('Unsupported microphone capture format')
    if data:raise ValueError('Microphone PCM ends before its completion record')
    if not complete:raise ValueError('No complete microphone capture found')
    return complete

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--session',type=int,default=-1,help='Completed recording index; default: last')
    args=parser.parse_args()
    sessions=decode(args.log.read_text(encoding='utf-8',errors='replace'))
    data=sessions[args.session]
    if args.output.exists() or args.output.with_suffix(".json").exists():raise FileExistsError(args.output)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with wave.open(str(args.output),'wb') as wav:
        wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(48000);wav.writeframes(data)
    report={'source_log':str(args.log.resolve()),'session':args.session,'complete_sessions':len(sessions),
            'pcm_bytes':len(data),'frames':len(data)//2,'rate':48000,'channels':1,'bits':16,
            'pcm_sha256':hashlib.sha256(data).hexdigest(),'wav_sha256':hashlib.sha256(args.output.read_bytes()).hexdigest()}
    args.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
