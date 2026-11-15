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

def identity(data,original=None):
    from picasso_ui_trust import library_identity
    source=inspect.getsource(library_identity)
    if source.count('kind != 6')!=1:raise ValueError('Changed code-signing verifier')
    ns=dict(library_identity.__globals__)
    exec(source.replace('kind != 6','kind not in (2, 6)'),ns)
    info=ns['library_identity'](data)
    parts=blobs(data);cd=parts[0]
    at,special=struct.unpack_from('>I',cd,16)[0],struct.unpack_from('>I',cd,24)[0]
    for i in range(1,special+1):
        expected=sha(parts[i]) if i in parts else (original_special(original,i).hex() if original is not None and i in (1,3) else bytes(32).hex())
        if cd[at-i*32:at-(i-1)*32].hex()!=expected:raise ValueError('Invalid signed special slot')
    if original is not None:
        if sha(original)!=STOCK or parts.get(5)!=blobs(original).get(5):
            raise ValueError('Original bluetoothd entitlements must be retained')
    return info

def paths(work):
    work=Path(work)
    return work/'bluetooth-userland',Path(os.environ.get('PICASSO_BT_STOCK',work/'bluetooth-input/bluetoothd'))

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
    # A platform service with its original launchd MachService. No sandbox
    # profile for Broadcom-specific hardware is assigned to this replacement.
    (out/'BlueTool.entitlements.plist').write_bytes(plistlib.dumps({'platform-application':True}))
    for name,source,library in [('BlueTool','BlueTool.c',False),('PicassoBTType0.dylib','PicassoBTType0.c',True)]:
        obj=out/(source+'.o')
        run([cc,'-target','arm64-apple-ios16.0','-isysroot',sdk,'-O2','-Wall','-Wextra','-Werror','-fblocks','-c',src/source,'-o',obj])
        cmd=[ld,'-arch','arm64','-platform_version','ios','16.0','16.5','-syslibroot',sdk,'-lSystem','-adhoc_codesign','-o',out/name,obj]
        if library:cmd+=['-dylib','-install_name','/usr/lib/'+name]
        run(cmd)
        entitlement='-S'+str(out/'BlueTool.entitlements.plist') if not library else '-S'
        run([signer,'-Cadhoc','-Hsha256',entitlement,out/name])
    ent=run([signer,'-e',stock]);(out/'bluetoothd.entitlements.plist').write_text(ent)
    run([signer,'-Cadhoc','-Hsha256','-S'+str(out/'bluetoothd.entitlements.plist'),'-Icom.apple.bluetoothd',out/'bluetoothd'])
    report={'original_sha256':STOCK,'runtime_qualified':False,'source_sha256':{str(p.relative_to(ROOT)):sha(p.read_bytes()) for p in [*src.glob('*'),Path(__file__)] if p.is_file()}}
    report['identities']={name:identity((out/name).read_bytes(),original if name=='bluetoothd' else None) for name in ['BlueTool','PicassoBTType0.dylib','bluetoothd']}
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
    return report

def make(work):
    work=Path(work);out,stock=paths(work);original=stock.read_bytes()
    report=json.loads((out/'build-report.json').read_text())
    for name,digest in report['source_sha256'].items():
        if sha((ROOT/name).read_bytes())!=digest:raise ValueError('Stale Bluetooth source '+name)
    entries=[];ids={}
    fw=json.loads((ROOT/'drivers/PicassoBluetooth/firmware.json').read_text())['files']
    for name,mode in FILES.items():
        base=name.split('/')[-1]
        data=(work/'bluetooth-firmware'/base if base in fw else out/base).read_bytes()
        if base in fw:
            if len(data)!=fw[base]['size'] or sha(data)!=fw[base]['sha256']:raise ValueError('Wrong firmware '+base)
        else:
            ids[base]=identity(data,original if base=='bluetoothd' else None)
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
