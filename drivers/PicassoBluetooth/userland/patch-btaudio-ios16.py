"""Offline private Skywalk binding adaptation of exact 20H392 BTAudioHALPlugin."""
import hashlib,json,struct
from pathlib import Path

STOCK = '58d65dbdc009dc8cea4e1acb23e86c7d93aff3e0451dc2cc4e0c5969745db88d'
INFO = '58d261506fbd9add9fe0850d0ed363890120222d6a7203dab4547ef38096b6de'
DYLIB = b'/usr/lib/PicassoBTAudio.dylib'

def adrp(pc,rd,target):
    imm=((target&~4095)-(pc&~4095))>>12
    if not -(1<<20)<=imm<(1<<20):raise ValueError('BTAudio ADRP range')
    imm&=(1<<21)-1
    return 0x90000000|(imm&3)<<29|(imm>>2)<<5|rd

def patch(data):
    if hashlib.sha256(data).hexdigest()!=STOCK:raise ValueError('Require exact original BTAudioHALPlugin')
    result=bytearray(data);n,size=struct.unpack_from('<II',data,16)
    at=32;data_segment=common=None
    for _ in range(n):
        cmd,length=struct.unpack_from('<II',data,at)
        if cmd==0x19 and data[at+8:at+24].rstrip(b'\0')==b'__DATA':
            data_segment=at
            for i in range(struct.unpack_from('<I',data,at+64)[0]):
                section=at+72+80*i
                if data[section:section+16].rstrip(b'\0')==b'__common':common=section
        at+=length
    if at!=32+size or data_segment is None or common is None:raise ValueError('BTAudio Mach-O layout differs')
    if struct.unpack_from('<4Q',data,data_segment+24)!=(0x78000,0x10000,0x78000,0x4000):raise ValueError('BTAudio writable segment differs')
    if struct.unpack_from('<2Q',data,common+32)!=(0x85368,16):raise ValueError('BTAudio common tail differs')
    # Declare the reserved zero-fill tail as writable storage, without adding
    # file bytes, moving a segment or writing runtime TEXT/DATA_CONST.
    struct.pack_into('<Q',result,common+40,0x85470-0x85368)
    entries=json.loads((Path(__file__).parent/'btaudio-imports.json').read_text())
    for entry in entries:
        pc,slot=int(entry['stub'],0),int(entry['slot'],0)
        words=struct.unpack_from('<3I',data,pc)
        if words[0]&0x9f00001f!=0x90000010 or words[1]&0xffc003ff!=0xf9400210 or words[2]!=0xd61f0200:
            raise ValueError('BTAudio import stub differs '+entry['name'])
        struct.pack_into('<3I',result,pc,adrp(pc,16,slot),0xf9400210|((slot&4095)//8)<<10,0xd61f0200)
    length=(24+len(DYLIB)+1+7)&~7
    command=struct.pack('<6I',0xc,length,24,2,0x10000,0x10000)+DYLIB+b'\0'
    command+=bytes(length-len(command))
    if at+length>0x3930 or any(data[at:0x3930]):raise ValueError('BTAudio header padding differs')
    result[at:at+length]=command
    struct.pack_into('<II',result,16,n+1,size+length)
    return bytes(result),dict(original_sha256=STOCK,imports=entries,storage=[hex(0x85400),hex(0x85470)],runtime_text_write=False)
