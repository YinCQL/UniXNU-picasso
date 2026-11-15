#!/usr/bin/env python3
"""Execute the signed library wrappers and exact daemon trampolines with native ABI."""
from pathlib import Path
import argparse, importlib.util, struct

def load(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
p=argparse.ArgumentParser();p.add_argument('--original',type=Path,required=True);p.add_argument('--library',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[1];patch=load('patch',r/'userland/patch-ios16.py');native=load('native',r/'tests/native-advertising.py')
binary,_=patch.patch(a.original.read_bytes());library=a.library.read_bytes()
class Trace(native.NativeAdvertising):
    def import_call(self,name):
        if name=='_sysctlbyname':
            assert bytes(self.uc.mem_read(self.x(0),24))==b'kern.qbt_keychain_trace\0'
            assert self.x(1)==self.x(2)==0 and self.x(4)==16
            self.records.append(struct.unpack('<4I',self.uc.mem_read(self.x(3),16)));self.ret(0xffffffffffffffff);return
        super().import_call(name)
for status in (0,-25300,-34018,4,130):
    t=Trace(binary,library);t.records=[];calls=[];buffer=t.ARENA+0x19000;length=buffer+0x100
    t.uc.mem_write(buffer,b'A'*64);t.uc.mem_write(length,struct.pack('<I',16))
    def original_get():
        calls.append(tuple(t.x(i) for i in range(4)));t.ret(status&0xffffffff)
    def original_set():
        calls.append(tuple(t.x(i) for i in range(3)));t.ret(status&0xffffffff)
    t.hooks[0x100006000]=original_get;t.hooks[0x100006010]=original_set
    t.write64(0x100867fc0,t.lib.symbols['_keychain_get']);t.write64(0x100867fc8,t.lib.symbols['_keychain_set'])
    assert t.call(0x100257654,2,buffer,16,length)==status&0xffffffff
    assert t.call(0x100257820,2,buffer,16)==status&0xffffffff
    assert calls==[(2,buffer,16,length),(2,buffer,16)]
    assert t.records==[(1,2,status&0xffffffff,16),(2,2,status&0xffffffff,16)]
    assert bytes(t.uc.mem_read(buffer,64))==b'A'*64
# A missing library slot must enter each unchanged original prologue.
for site,continuation,size in [(0x100257654,0x100006000,96),(0x100257820,0x100006010,64)]:
    t=Trace(binary,library);seen=[];sp=t.STACK+0xf000
    t.write64(0x100867fc0,0);t.write64(0x100867fc8,0)
    def after_prologue():
        seen.append(t.uc.reg_read(native.UC_ARM64_REG_SP));t.uc.emu_stop()
    t.hooks[site+4]=after_prologue
    try:t.call(site,2,t.ARENA,16,t.ARENA+64)
    except AssertionError:pass
    assert seen==[sp-size]
print('PASS native read/write trampolines preserve arguments, buffers, errors and fallback prologues; trace failure never changes status')
