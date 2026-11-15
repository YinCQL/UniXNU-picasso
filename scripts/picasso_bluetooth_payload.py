"""Build and qualify the exact Picasso/20H392 native Bluetooth runtime."""
from pathlib import Path
import hashlib
import importlib.util
import inspect
import json
import os
import plistlib
import shutil
import struct
import subprocess
import uuid

ROOT=Path(__file__).resolve().parents[1]
FILES={
    'ui/PicassoBTType0.dylib':0o644,
    'bluetooth-bin/bluetoothd':0o755,
    'bluetooth-bin/BlueTool':0o755,
    'bluetooth-firmware/crbtfw32.tlv':0o644,
    'bluetooth-firmware/crnv32.bin':0o644,
    'bluetooth-firmware/crnv32u.bin':0o644,
    'ui/PicassoBTAudio.dylib':0o644,
    'audio/BTAudioHALPlugin.driver/Info.plist':0o644,
    'audio/BTAudioHALPlugin.driver/BTAudioHALPlugin':0o755,
}
STOCK='8b7c0a0bea51544db82269d7f2775ab7996b8740927283428c4753e032df3a8f'

def sha(data):return hashlib.sha256(data).hexdigest()

def blobs(data):
    at=32
    result=[]
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        cmd,size=struct.unpack_from('<II',data,at)
        if cmd==0x1d:
            start,length=struct.unpack_from('<II',data,at+8)
            result.append(data[start:start+length])
        at+=size
    if len(result)!=1:raise ValueError('Require one signature')
    signature=result[0]
    magic,length,count=struct.unpack_from('>III',signature)
    if magic!=0xfade0cc0 or length>len(signature):raise ValueError('Invalid signature')
    result={}
    for i in range(count):
        slot,at=struct.unpack_from('>II',signature,12+i*8)
        if slot in result or at+8>length:raise ValueError('Invalid signature slot')
        size=struct.unpack_from('>I',signature,at+4)[0]
        if size<8 or at+size>length:raise ValueError('Invalid signature extent')
        result[slot]=signature[at:at+size]
    return result

def original_special(original, slot):
    for cd in blobs(original).values():
        if struct.unpack_from('>I',cd)[0]!=0xfade0c02 or cd[37]!=2:continue
        at=struct.unpack_from('>I',cd,16)[0]
        count=struct.unpack_from('>I',cd,24)[0]
        if count>=slot:return cd[at-slot*32:at-(slot-1)*32]
    return bytes(32)

def identity(data,original=None,bundle=False,retained_special=False):
    from picasso_ui_trust import library_identity
    source=inspect.getsource(library_identity)
    if source.count('kind != 6')!=1:raise ValueError('Changed code-signing verifier')
    ns=dict(library_identity.__globals__)
    exec(source.replace('kind != 6','kind not in (2, 6, 8)' if bundle else 'kind not in (2, 6)'),ns)
    info=ns['library_identity'](data)
    parts=blobs(data);cd=parts[0]
    at,special=struct.unpack_from('>I',cd,16)[0],struct.unpack_from('>I',cd,24)[0]
    for i in range(1,special+1):
        expected=sha(parts[i]) if i in parts else (original_special(original,i).hex() if original is not None and i in (1,3) else bytes(32).hex())
        if cd[at-i*32:at-(i-1)*32].hex()!=expected:raise ValueError('Invalid signed special slot')
    if original is not None and not retained_special:
        if sha(original)!=STOCK or parts.get(5)!=blobs(original).get(5):
            raise ValueError('Original bluetoothd entitlements must be retained')
    return info

def paths(work):
    work=Path(work)
    return work/'bluetooth-userland',Path(os.environ.get('PICASSO_BT_STOCK',work/'bluetooth-input/bluetoothd'))

def skywalk_objects(work,src,out,sdk,cc,run):
    # Use the exact prepared kernel headers and wrappers. An SDK version of
    # these private structures is incompatible with the shared memory map.
    xnu=Path(os.environ.get('PICASSO_BT_XNU',work/'tree/.cache/xnu-work')).resolve()
    private=out/'skywalk';private.mkdir(parents=True,exist_ok=True)
    include=private/'include';(include/'skywalk').mkdir(parents=True,exist_ok=True)
    inputs={}
    def bind(path):
        inputs[str(path.relative_to(xnu))]=sha(path.read_bytes())
    for header in sorted((xnu/'bsd/skywalk').rglob('*.h')):
        bind(header)
        target=include/'skywalk'/header.relative_to(xnu/'bsd/skywalk')
        target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(header,target)
        flat=include/'skywalk'/header.name
        if flat.exists() and flat.read_bytes()!=header.read_bytes():
            raise ValueError('Ambiguous Skywalk exported header '+header.name)
        shutil.copyfile(header,flat)
    master=xnu/'bsd/kern/syscalls.master';bind(master)
    for number,name in [(510,'open'),(511,'get_info'),(512,'sync'),(513,'get_opt'),(514,'set_opt')]:
        if not any(line.startswith(str(number)+'\t') and 'int __channel_'+name+'(' in line for line in master.read_text().splitlines()):
            raise ValueError('Skywalk syscall ABI differs: '+name)
    wrappers=[]
    for name in ['os_channel','os_packet']:
        original=xnu/'libsyscall/wrappers/skywalk'/(name+'.c');bind(original)
        source=original.read_text()
        if name=='os_channel':
            # Explicitly spell the existing uintptr conversions for modern
            # clang. No field widths, offset computations or checks change.
            source=source.replace('(_bft) = _CHANNEL_RING_BFT(', '(_bft) = (struct __user_buflet *)(uintptr_t)_CHANNEL_RING_BFT(')
            source=source.replace('ubft = _CHANNEL_RING_BFT(', 'ubft = (struct __user_buflet *)(uintptr_t)_CHANNEL_RING_BFT(')
            source=source.replace('&(pbft->buf_addr) = NULL;', '&(pbft->buf_addr) = 0;')
            source=source.replace('init.ci_key = cha->cha_key;', 'init.ci_key = (user_addr_t)(uintptr_t)cha->cha_key;')
            source=source.replace('prop->sp_mdata_ptr = q;', 'prop->sp_mdata_ptr = (mach_vm_address_t)(uintptr_t)q;')
            source=source.replace('ubft = _SLOT_BFT_METADATA(', 'ubft = (struct __user_buflet *)(uintptr_t)_SLOT_BFT_METADATA(')
            source=source.replace('*ehandle = NULL;', '*ehandle = 0;')
        target=private/(name+'.c');target.write_text(source);wrappers.append(target)
    # Prefix every public wrapper, including their internal cross-calls, so
    # other processes and libraries retain their original system bindings.
    import re
    api=set()
    for header in ['channel/os_channel.h','channel/os_channel_private.h','packet/os_packet.h','packet/os_packet_private.h']:
        api.update(re.findall(r'\b(os_(?:channel|packet|buflet|inet|copy)[a-zA-Z0-9_]*)\s*\(', (xnu/'bsd/skywalk'/header).read_text()))
    for source in wrappers:
        api.update(re.findall(r'^((?:os_)[a-zA-Z0-9_]+)\(',source.read_text(),re.M))
    rename=['-D'+name+'=picasso_sw_'+name for name in sorted(api)]
    rename+=['-D__channel_'+name+'=picasso_sw___channel_'+name for name in ['open','get_info','sync','get_opt','set_opt']]
    entries=json.loads((src/'skywalk-imports.json').read_text())
    (private/'skywalk-slots.inc').write_text(''.join('    *(void **)((uintptr_t)'+entry['slot']+'ULL + slide) = (void *)picasso_sw_'+entry['name']+';\n' for entry in entries))
    flags=['-target','arm64-apple-ios16.0','-isysroot',sdk,'-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-but-set-variable','-Wno-sign-compare','-DPRIVATE=1','-DLIBSYSCALL_INTERFACE=1','-include',src/'PicassoBTSkywalk.h','-I'+str(include),'-I'+str(private),'-I'+str(xnu/'libkern'),'-idirafter',xnu/'bsd']+rename
    objects=[]
    for source in [*wrappers,src/'PicassoBTSkywalk.c']:
        obj=private/(source.stem+'.o');run([cc,*flags,'-c',source,'-o',obj]);objects.append(obj)
    obj=private/'syscalls.o';run([cc,'-target','arm64-apple-ios16.0','-c',src/'PicassoBTSkywalkSyscalls.S','-o',obj]);objects.append(obj)
    # Include libkern dependencies in the source binding, as these supply the
    # atomics used when the channel is shared between user and kernel.
    for header in (xnu/'libkern/os').glob('*.h'):bind(header)
    return objects,dict(root=str(xnu),schema_version=18,source_sha256=inputs,objects={str(p.relative_to(out)):sha(p.read_bytes()) for p in objects},imports=entries)

def build(work):
    work=Path(work);out,stock=paths(work);out.mkdir(parents=True,exist_ok=True)
    original=stock.read_bytes()
    if sha(original)!=STOCK:raise ValueError('Require original 20H392 bluetoothd')
    src=ROOT/'drivers/PicassoBluetooth/userland'
    spec=importlib.util.spec_from_file_location('picasso_bt_patch',src/'patch-ios16.py')
    patcher=importlib.util.module_from_spec(spec);spec.loader.exec_module(patcher)
    patched,_=patcher.patch(original);(out/'bluetoothd').write_bytes(patched)
    signer=Path(os.environ.get('PICASSO_BT_LDID',work/'bluetooth-tools/ldid'))
    sdk=Path(os.environ.get('PICASSO_BT_SDK','/home/y/toolchains/audio-sdks/iPhoneOS16.5.sdk'))
    cc=os.environ.get('PICASSO_BT_CC',shutil.which('clang-18') or 'clang')
    ld=os.environ.get('PICASSO_BT_LD',shutil.which('ld64.lld-18') or 'ld64.lld')
    commands=[]
    def run(cmd):
        cmd=list(map(str,cmd));commands.append(cmd)
        result=subprocess.run(cmd,text=True,capture_output=True)
        with (out/'build.log').open('a') as log:log.write(' '.join(cmd)+'\n'+result.stdout+result.stderr)
        result.check_returncode();return result.stdout
    (out/'build.log').write_text('')
    sky_objects,sky_report=skywalk_objects(work,src,out,sdk,cc,run)
    # A platform service with its original launchd MachService. No sandbox
    # profile for Broadcom-specific hardware is assigned to this replacement.
    (out/'BlueTool.entitlements.plist').write_bytes(plistlib.dumps({'platform-application':True}))
    for name,source,library in [('BlueTool','BlueTool.c',False),('PicassoBTType0.dylib','PicassoBTType0.c',True)]:
        obj=out/(source+'.o')
        run([cc,'-target','arm64-apple-ios16.0','-isysroot',sdk,'-O2','-Wall','-Wextra','-Werror','-fblocks','-c',src/source,'-o',obj])
        cmd=[ld,'-arch','arm64','-platform_version','ios','16.0','16.5','-syslibroot',sdk,'-lSystem','-adhoc_codesign','-o',out/name,obj]+(sky_objects if library else [])
        if library:cmd+=['-dylib','-install_name','/usr/lib/'+name]
        run(cmd)
        entitlement='-S'+str(out/'BlueTool.entitlements.plist') if not library else '-S'
        run([signer,'-Cadhoc','-Hsha256',entitlement,out/name])
    ent=run([signer,'-e',stock]);(out/'bluetoothd.entitlements.plist').write_text(ent)
    run([signer,'-Cadhoc','-Hsha256','-S'+str(out/'bluetoothd.entitlements.plist'),'-Icom.apple.bluetoothd',out/'bluetoothd'])
    audio_source=Path(os.environ.get('PICASSO_BT_AUDIO_STOCK',str(work/'bluetooth-input/BTAudioHALPlugin.driver')))
    audio_original=(audio_source/'BTAudioHALPlugin').read_bytes()
    audio_info=(audio_source/'Info.plist').read_bytes()
    spec=importlib.util.spec_from_file_location('picasso_bt_audio_patch',src/'patch-btaudio-ios16.py')
    audio_patcher=importlib.util.module_from_spec(spec);spec.loader.exec_module(audio_patcher)
    if sha(audio_info)!=audio_patcher.INFO:raise ValueError('Require original BTAudioHALPlugin Info.plist')
    audio_patched,audio_report=audio_patcher.patch(audio_original)
    audio_bundle=out/'BTAudioHALPlugin.driver';audio_bundle.mkdir(exist_ok=True)
    (audio_bundle/'BTAudioHALPlugin').write_bytes(audio_patched);(audio_bundle/'Info.plist').write_bytes(audio_info)
    audio_imports=json.loads((src/'btaudio-imports.json').read_text())
    (out/'skywalk/btaudio-slots.inc').write_text(''.join('        *(void **)((uintptr_t)'+e['slot']+'ULL + slide) = (void *)picasso_sw_'+e['name']+';\n' for e in audio_imports))
    audio_flags=next(c[1:c.index('-c')] for c in commands if '-c' in c and str(src/'PicassoBTSkywalk.c') in c)
    obj=out/'PicassoBTAudio.o';run([cc,*audio_flags,'-c',src/'PicassoBTAudio.c','-o',obj])
    run([ld,'-arch','arm64','-platform_version','ios','16.0','16.5','-syslibroot',sdk,'-lSystem','-adhoc_codesign','-dylib','-install_name','/usr/lib/PicassoBTAudio.dylib','-o',out/'PicassoBTAudio.dylib',obj,*sky_objects])
    run([signer,'-Cadhoc','-Hsha256','-S',out/'PicassoBTAudio.dylib'])
    resources=audio_source/'_CodeSignature/CodeResources'
    if sha(resources.read_bytes())!='3c727b0f043e463c0227db49cef54c5567715986d08a46c98923c9bb73585e5a':raise ValueError('Original BTAudio resource signature differs')
    run([signer,'-Cadhoc','-Hsha256','-S','-Icom.apple.audio.BTAudioHALPlugin','-E1:'+str(audio_bundle/'Info.plist'),'-E3:'+str(resources),audio_bundle/'BTAudioHALPlugin'])
    audio_report['identity']=identity((audio_bundle/'BTAudioHALPlugin').read_bytes(),audio_original,bundle=True,retained_special=True)
    audio_report['info_sha256']=sha(audio_info)
    report={'audio':audio_report,'skywalk':sky_report,'original_sha256':STOCK,'runtime_qualified':False,'source_sha256':{str(p.relative_to(ROOT)):sha(p.read_bytes()) for p in [*src.glob('*'),Path(__file__)] if p.is_file()}}
    report['identities']={name:identity((out/name).read_bytes(),original if name=='bluetoothd' else None) for name in ['BlueTool','PicassoBTType0.dylib','bluetoothd','PicassoBTAudio.dylib']}
    report['identities']['BTAudioHALPlugin']=audio_report['identity']
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    return report

def make(work):
    work=Path(work);out,stock=paths(work);original=stock.read_bytes()
    report=json.loads((out/'build-report.json').read_text())
    for name,digest in report['source_sha256'].items():
        if sha((ROOT/name).read_bytes())!=digest:raise ValueError('Stale Bluetooth source '+name)
    for name,digest in report['skywalk']['source_sha256'].items():
        if sha((Path(report['skywalk']['root'])/name).read_bytes())!=digest:raise ValueError('Stale Skywalk source '+name)
    entries=[];ids={}
    fw=json.loads((ROOT/'drivers/PicassoBluetooth/firmware.json').read_text())['files']
    for name,mode in FILES.items():
        base=name.split('/')[-1]
        path=out/'BTAudioHALPlugin.driver'/base if 'BTAudioHALPlugin.driver/' in name else out/base
        data=(work/'bluetooth-firmware'/base if base in fw else path).read_bytes()
        if base in fw:
            if len(data)!=fw[base]['size'] or sha(data)!=fw[base]['sha256']:raise ValueError('Wrong firmware '+base)
        elif base=='Info.plist':
            if sha(data)!=report['audio']['info_sha256']:raise ValueError('Stale BTAudio Info.plist')
        else:
            audio_original=(Path(os.environ.get('PICASSO_BT_AUDIO_STOCK',str(work/'bluetooth-input/BTAudioHALPlugin.driver')))/'BTAudioHALPlugin').read_bytes() if base=='BTAudioHALPlugin' else None
            ids[base]=identity(data,original if base=='bluetoothd' else audio_original,bundle=base=='BTAudioHALPlugin',retained_special=base=='BTAudioHALPlugin')
            if ids[base]!=report['identities'][base]:raise ValueError('Stale signed Bluetooth executable')
        entries.append((name,mode,data))
    from picasso_media_payload import modules
    original_hash=hashlib.sha256(blobs(original)[0]).digest()[:20]
    stock_entries=[module[at:at+24]
        for module in modules((work/'audio-hal/audio-trust.bundle').read_bytes())[:2]
        for at in range(24,len(module),24) if module[at:at+20]==original_hash]
    if len(stock_entries)!=1 or stock_entries[0][20:]!=bytes([2,0,1,0]):
        raise ValueError('Original bluetoothd trust metadata differs')
    metadata={bytes.fromhex(v['cdhash']):(stock_entries[0][20:] if name=='bluetoothd' else bytes([2,0,0,0]))
              for name,v in ids.items()}
    hashes=sorted(metadata)
    module=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'picasso:bluetooth:'+':'.join(h.hex() for h in hashes)).bytes,len(hashes))
    module+=b''.join(h+metadata[h] for h in hashes)
    return entries,module,report

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--work',type=Path,required=True)
    a=p.parse_args();print(json.dumps(build(a.work)))
