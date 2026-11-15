#!/usr/bin/env python3
from pathlib import Path
import argparse,hashlib,json,runpy,struct,sys
p=argparse.ArgumentParser();p.add_argument('--work',type=Path,required=True);p.add_argument('--layout',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
R=Path(__file__).resolve().parents[3];W=a.work;out=a.output;assert not out.exists();out.mkdir()
sys.argv=[str(R/'drivers/PicassoBluetooth/tests/native-skywalk.py'),'--binary',str(W/'bluetooth-userland/bluetoothd'),'--library',str(W/'bluetooth-userland/PicassoBTType0.dylib'),'--layout',str(a.layout),'--output',str(out/'daemon-skywalk.json')]
ns=runpy.run_path(sys.argv[0]);native=ns['native'];Skywalk=ns['Skywalk'];Trap=ns['Trap']
plugin=(W/'bluetooth-userland/BTAudioHALPlugin.driver/BTAudioHALPlugin').read_bytes();library=(W/'bluetooth-userland/PicassoBTAudio.dylib').read_bytes();main=(W/'bluetooth-input/BTAudioHALPlugin.driver/BTAudioHALPlugin').read_bytes()
entries=json.loads((R/'drivers/PicassoBluetooth/userland/btaudio-imports.json').read_text())
BASE=0x120000000
ns['Skywalk'].__init__.__globals__['library']=library
def initialize(self,binary,ignored):
    self.uc=native.Uc(native.UC_ARCH_ARM64,native.UC_MODE_ARM);self.main=native.MachO(plugin);self.lib=native.MachO(library)
    self.lib.load(self.uc)
    for va,length,offset,size in self.main.segments:
        self.uc.mem_map(BASE+va,(length+4095)&~4095)
        if size:self.uc.mem_write(BASE+va,plugin[offset:offset+size])
    for addr,size in [(self.STACK,0x10000),(self.ARENA,0x20000),(self.RETURN,0x1000)]:self.uc.mem_map(addr,size)
    self.hooks={};self.initialized=[];self.commands=[];self.advances=[];self.deferred=[]
    for address,(kind,name) in self.lib.imports.items():
        if kind==8:self.hooks[address]=lambda name=name:self.import_call(name)
        elif name=='___stack_chk_guard':self.write64(address,self.ARENA+0x1f000)
    self.uc.hook_add(native.UC_HOOK_CODE,self.code)
    self.call(self.lib.symbols['_btaudio_init'])
original_import=Skywalk.import_call
def imports(self,name):
    if name=='__dyld_image_count':self.ret(1)
    elif name=='__dyld_get_image_header':self.ret(BASE)
    elif name=='__dyld_get_image_vmaddr_slide':self.ret(BASE)
    else:original_import(self,name)
native.NativeAdvertising.__init__=initialize;Skywalk.import_call=imports
t=Skywalk()
for e in entries:assert t.read(BASE+int(e['slot'],0),8)==t.lib.symbols['_picasso_sw_'+e['name']]
attr=t.call(BASE+0x57440);assert attr
assert t.call(BASE+0x57458,attr,6,1024)==0
assert t.call(BASE+0x57464,attr,t.ARENA+0x10000,16)==0
ch=t.call(BASE+0x57470,t.ARENA+0x10000,0,0,0xffffffff,attr);assert ch
tx=t.call(BASE+0x574d0,ch,0);rx=t.call(BASE+0x574ac,ch,0);assert tx and rx
assert t.call(BASE+0x57488,ch)==20
assert t.call(BASE+0x574a0,ch,0)==0
prop=t.ARENA+0x18000;slot=t.call(BASE+0x57494,tx,0,prop);assert slot==t.slots[0]
assert t.call(BASE+0x5744c,attr)==0
# An unrelated bundle UUID must leave the reserved slots untouched.
t.uc.mem_write(BASE+0x85400,bytes(112));off=32
for _ in range(struct.unpack_from('<I',plugin,16)[0]):
    cmd,size=struct.unpack_from('<II',plugin,off)
    if cmd==0x1b:t.uc.mem_write(BASE+off+8,bytes(16))
    off+=size
t.call(t.lib.symbols['_btaudio_init']);assert bytes(t.uc.mem_read(BASE+0x85400,112))==bytes(112)
for version in [15,17,19]:
    failed=Skywalk(version)
    try:failed.call(BASE+0x57470,failed.ARENA+0x10000,0,0,0xffffffff,0)
    except Trap:pass
    else:raise AssertionError('BTAudio accepted wrong schema '+str(version))
result=dict(passed=True,nonzero_slide=hex(BASE),imports=len(entries),wrong_uuid_rejected=True,wrong_schemas_rejected=[15,17,19],binaries={'BTAudioHALPlugin.driver/BTAudioHALPlugin':hashlib.sha256(plugin).hexdigest(),'PicassoBTAudio.dylib':hashlib.sha256(library).hexdigest()},source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
(out/'report.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
