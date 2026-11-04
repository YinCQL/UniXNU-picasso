#!/usr/bin/env python3
"""Qualify an owned offline HID source against the unchanged target IOHIDDevice."""
import hashlib,json,struct,subprocess,sys
from pathlib import Path
from picasso_llvm import tool as llvm_tool
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-hid-input';OUT.mkdir(exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/NativePrelink'))
from native_prelink import macho,backed_offset,exports

def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def require(value,why):
    if not value:raise SystemExit(why)
from picasso_original_abi import load_reference, qualify_hid, qualify_hid_event_driver
reference=load_reference()
names=reference.names
vptr=0xfffffff005d045c0
methods=qualify_hid(reference)
event_methods=qualify_hid_event_driver(reference)
kernel_methods=set(reference.names.values())
event_header=('// Qualified IOHIDFamily-2031.120.4 event driver; generated, never committed.\n'
              '// Zero entries are kernel exports rebound by NativePrelink to the compiled XNU.\n'
              '#pragma once\nstatic constexpr uintptr_t kOriginalEventDriverVptr=0xfffffff005d06a68ULL;\n'
              'static const uintptr_t kOriginalEventDriverMethods[]={\n' +
              ''.join(f'    {0 if address in kernel_methods else address:#x}ULL,\n' for address in event_methods) + '};\n')
(OUT/'OriginalHidEventABI.h').write_text(event_header)
header='''// Generated from the qualified original image; opaque private storage only.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
class IOHIDDevice : public IOService {
 OSDeclareAbstractStructors(IOHIDDevice)
 unsigned char originalPrivate[0xd0-sizeof(IOService)];
public:
 using IOService::setProperty;
 IOReturn Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 bool setProperty(const OSSymbol *,OSObject *) override;
 bool willTerminate(IOService *,IOOptionBits) override;
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
'''
for i in range(168,207):
    if i==171:header+=' virtual OSString *newTransportString() const;\n'
    elif i==173:header+=' virtual OSString *newProductString() const;\n'
    elif i==180:header+=' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const=0;\n'
    elif i==181:header+=' virtual IOReturn handleReport(IOMemoryDescriptor *,unsigned,IOOptionBits);\n'
    else:header+=f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOHIDDevice)==0xd0,"Original device instance size");\n'
(OUT/'OriginalHidDeviceABI.h').write_text(header)
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeHidInput'],text=True),end='')
obj=OUT/'NativeHidInput.o';raw=obj.read_bytes();om=macho(raw)
st=next(p for c,p,_ in om['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vt=next(va for name,va,_ in symbols if name=='__ZTV14NativeHidInput');relocations={}
for segment in om['segments']:
    for sec in segment['sections']:
        if sec['va']<=vt<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vt+16+i*8) for i in range(207)]
require(all(compiled) and vt+16+207*8 not in relocations,'Compiled exact 207-slot vtable')
bindings={'__ZN11IOHIDDeviceC2EPK11OSMetaClass':0xfffffff004ad94f8,'__ZN11IOHIDDeviceD2Ev':0xfffffff004ad9518,'__ZN11IOHIDDevice10gMetaClassE':0xfffffff006ae6fa0,'__ZTV11IOHIDDevice':vptr-16}
owned=[]
for i,name in enumerate(compiled):
    if name.startswith(('__ZN14NativeHidInput','__ZNK14NativeHidInput')):owned.append(i)
    elif '11IOHIDDevice' in name:bindings[name]=methods[i]
    else:require(i<168 and names.get(name)==methods[i],'Actual inherited native method '+str(i))
require(owned==[0,1,7,14,86,87,111,171,173,180],'Only qualified HID methods overridden')
# This kernel compiler resolves qualified super calls through the original vtable.
undefined={n for n,_,k in symbols if k==1 and '11IOHIDDevice' in n}
require(undefined==set(bindings),'All and only actual original HID imports bound')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n,a in sorted(bindings.items())))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,'-o',OUT/'hid-original-bindings.o'])
late=OUT/'NativeHidInput.late.o'
run([llvm_tool('llvm-objcopy'),'--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp',obj,late])
def code(data,image):
    s=next(s for g in image['segments'] for s in g['sections'] if s['name']=='__text');return data[s['fileoff']:s['fileoff']+s['size']]
require(code(raw,om)==code(late.read_bytes(),macho(late.read_bytes())),'Owned code unchanged by deferred metaclass registration')
(OUT/'abi-audit.json').write_text(json.dumps({'original_sha256':hashlib.sha256(reference.data).hexdigest(),'original_slide':-0x1000000,'reference':'unmodified IPSW kernelcache','original_slots':207,'original_instance_size':208,'owned_slots':owned,'bindings':bindings,'runtime_qualified':False,'source_sha256':hashlib.sha256((ROOT/'drivers/NativeHidInput/NativeHidInput.cpp').read_bytes()).hexdigest()},indent=2)+'\n')
print('PASS: original HID instruction identity, base imports, call boundaries and exact owned vtable')
