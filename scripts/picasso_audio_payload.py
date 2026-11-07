#!/usr/bin/env python3
"""Exact audio-addon trust plus the unchanged base Data audit.

This layer adds one signed bundle to the already-qualified UI trust module.
It does not change the Compose recipe or accept unrecognized trust entries.
"""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct,sys,tomllib,uuid
ROOT=Path(__file__).resolve().parents[1]
def load(p):
    spec=importlib.util.spec_from_file_location(p.stem.replace('-','_'),p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def make(work):
    from picasso_ui_trust import verify_bundle
    base=(work/'ui-trust.bundle').read_bytes();ui=verify_bundle(base,(work/'libpicasso-sb.dylib').read_bytes())
    hal=load(ROOT/'scripts/build-picasso-audio-hal.py')
    identity=hal.bundle_identity((work/'audio-hal/PicassoAudio.driver/PicassoAudio').read_bytes())
    count,*offsets=struct.unpack_from('<4I',base);assert count==3
    modules=[base[offsets[i]:offsets[i+1] if i<2 else len(base)] for i in range(3)]
    own=struct.pack('<I16sI',2,uuid.uuid5(uuid.NAMESPACE_URL,'picasso:audio:'+identity['cdhash']).bytes,1)+bytes.fromhex(identity['cdhash'])+bytes([2,0,0,0])
    modules.append(own);cursor=20;starts=[]
    for m in modules:starts.append(cursor);cursor+=len(m)
    blob=struct.pack('<5I',4,*starts)+b''.join(modules)
    return blob,ui,identity
def verify(bundle,work):
    expected,ui,identity=make(work)
    if bundle!=expected:raise ValueError('Audio trust differs from exact UI/base/audio modules')
    return ui,identity
def check(work,tree,output):
    # Execute the existing strict Data checker with two precisely constrained
    # adapters: the extra audio argument and the extra exact trust module. Its
    # seed, ownership, payload extraction, library and capacity checks still run.
    import picasso_ui_trust
    original=picasso_ui_trust.accepted_payload_bundle
    def accepted(blob,library,device):
        if library!='out/picasso/libpicasso-sb.dylib' or Path(device).resolve()!=ROOT:raise ValueError('Bad UI trust identity')
        ui,_=verify(blob,work);return ui
    picasso_ui_trust.accepted_payload_bundle=accepted
    source=(ROOT/'scripts/check-picasso-data-artifacts.py').read_text()
    old="require(payload_report['kernel_args'] == ' '.join(spec['kernel_args']), 'Unexpected boot arguments')"
    new="require(payload_report['kernel_args'] == ' '.join(spec['kernel_args']) + ' picasso-audio-addon=1', 'Unexpected audio boot arguments')"
    if source.count(old)!=1:raise ValueError('Data checker contract changed')
    ns={'__name__':'picasso_audio_data_check','__file__':str(ROOT/'scripts/check-picasso-data-artifacts.py')}
    try:
        exec(compile(source.replace(old,new),ns['__file__'],'exec'),ns)
        result=ns['check'](work,tree,output)
    finally:picasso_ui_trust.accepted_payload_bundle=original
    report=json.loads((work/'audio-hal/build-report.json').read_text());assert report['passed']
    for name,digest in report['sources'].items():assert sha(ROOT/name)==digest,name
    files=output.with_suffix('.files')/'EFI/UniXNU/Payloads'
    addon=work/'audio-hal/audio.addon';assert sha(addon)==report['addon_sha256']
    assert (files/'kernel.macho.gpuaddon').read_bytes()==addon.read_bytes()
    decode=load(ROOT/'scripts/check-picasso-adt.py').decode
    assert decode((files/'kernel.macho.adt').read_bytes())['/chosen/memory-map']['GpuAddon']==bytes(16)
    _,identity=verify((files/'kernel.macho.trustcache').read_bytes(),work)
    result['audio']=dict(identity=identity,addon_sha256=sha(addon),exact_owned_entries=2,hal_installed_in_payload=True,runtime_qualified=False)
    output.write_text(json.dumps(result,indent=2)+'\n');return result
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('mode',choices=['prepare','check']);p.add_argument('--work',type=Path,required=True);p.add_argument('--tree',type=Path);p.add_argument('--output',type=Path);a=p.parse_args();work=a.work.resolve()
    if a.mode=='prepare':
        blob,ui,audio=make(work);(work/'audio-hal/audio-trust.bundle').write_bytes(blob);print(json.dumps(audio))
    else:
        result=check(work,a.tree.resolve(),a.output.resolve());print(json.dumps({'passed':True,'audio':result['audio']}))
if __name__=='__main__':main()
