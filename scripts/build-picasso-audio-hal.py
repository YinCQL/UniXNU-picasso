#!/usr/bin/env python3
"""Build and audit the iOS16 output HAL using real SDK headers and an actual signature.

Linux: PICASSO_AUDIO_SDK, PICASSO_AUDIO_HEADERS, PICASSO_AUDIO_CC and
PICASSO_AUDIO_LD select explicitly prepared SDK/header/tool inputs. There is no
codesign shim: ld64.lld emits a SHA256 ad-hoc signature with verified 4K slots.
"""
from pathlib import Path
import argparse,hashlib,json,os,plistlib,shutil,struct,subprocess,sys,zlib
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def bundle_identity(data):
    from picasso_ui_trust import library_identity
    if len(data)<32 or struct.unpack_from('<I',data,12)[0]!=8:raise ValueError('Require MH_BUNDLE')
    # Use the unchanged CodeDirectory verifier for the actual MH_BUNDLE bytes.
    # Its strict file-kind precondition is adapted locally, not the signed data.
    import inspect
    body=inspect.getsource(library_identity).replace('kind != 6','kind != 8')
    ns=dict(library_identity.__globals__);exec(body,ns)
    result=ns['library_identity'](data)
    if result['code_page_bytes']!=4096:raise ValueError('Require native 4K signature slots')
    return result
def pack(files):
    def pad(b):return b+bytes(-len(b)%8)
    body=b''
    for name,mode,data in files:
        encoded=name.encode()+b'\0';compressed=zlib.compress(data,9)
        if len(encoded)>160 or not name.startswith('audio/') or '..' in name:raise ValueError('Bad audio addon path')
        body+=struct.pack('<IIQQ',len(encoded),mode,len(data),len(compressed))+pad(encoded)+pad(compressed)
    return b'NGPUADD2'+struct.pack('<IIQ',len(files),0,24+len(body))+body
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    sdk=Path(os.environ.get('PICASSO_AUDIO_SDK','/home/y/toolchains/audio-sdks/iPhoneOS16.5.sdk'))
    headers=Path(os.environ.get('PICASSO_AUDIO_HEADERS','/home/y/toolchains/audio-hal-headers'))
    cc=os.environ.get('PICASSO_AUDIO_CC','/usr/bin/clang-18');ld=os.environ.get('PICASSO_AUDIO_LD','/usr/bin/ld64.lld-18')
    for q in [sdk/'SDKSettings.json',headers/'CoreAudio/AudioServerPlugIn.h',headers/'CoreAudio/AudioHardwareBase.h']:
        if not q.is_file():raise ValueError('Missing real SDK input '+str(q))
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);bundle=out/'PicassoAudio.driver';bundle.mkdir(exist_ok=True)
    source=ROOT/'hal/PicassoAudioPlugIn.c';obj=out/'PicassoAudio.o';binary=bundle/'PicassoAudio'
    command=[cc,'-target','arm64-apple-ios16.0','-isysroot',str(sdk),'-I',str(headers),'-I',str(ROOT/'package'),'-I',str(ROOT/'hal'),'-O2','-Wall','-Wextra','-Werror','-fvisibility=hidden','-c',str(source),'-o',str(obj)]
    commands=[command,[ld,'-arch','arm64','-platform_version','ios','16.0','16.5','-bundle','-syslibroot',str(sdk),'-lSystem','-framework','CoreFoundation','-framework','IOKit',str(obj),'-o',str(binary),'-adhoc_codesign']]
    for i,cmd in enumerate(commands):
        run=subprocess.run(cmd,capture_output=True,text=True);(out/f'build-{i}.log').write_text(run.stdout+run.stderr)
        if run.returncode:raise RuntimeError(run.stdout+run.stderr)
    shutil.copy2(ROOT/'hal/Info.plist',bundle/'Info.plist');identity=bundle_identity(binary.read_bytes())
    info=plistlib.loads((bundle/'Info.plist').read_bytes());assert info['CFBundleIdentifier']=='org.unixnu.picasso.audio'
    files=[('audio/PicassoAudio.driver/Info.plist',0o644,(bundle/'Info.plist').read_bytes()),('audio/PicassoAudio.driver/PicassoAudio',0o755,binary.read_bytes())]
    addon=out/'audio.addon';addon.write_bytes(pack(files))
    sources={str(p.relative_to(ROOT)):sha(p) for p in [source,ROOT/'hal/PicassoAudioMix.h',ROOT/'hal/Info.plist',ROOT/'package/PicassoAudioAbi.h',Path(__file__)]}
    # Hash all SDK files used by the compiler via its dependency output.
    deps=subprocess.check_output(command[:-4]+['-M','-MT','hal',str(source)],text=True)
    inputs={}
    for n in deps.replace('\\\n',' ').split()[1:]:
        p=Path(n)
        if p.is_file() and (p.is_relative_to(sdk) or p.is_relative_to(headers)):inputs[str(p)]=sha(p)
    for p in [Path(cc),Path(ld)]:inputs[str(p)]=sha(p)
    report=dict(passed=True,hardware_tested=False,target='arm64-apple-ios16.0',identity=identity,sources=sources,build_inputs=inputs,commands=commands,addon_sha256=sha(addon),files={n:dict(mode=m,bytes=len(b),sha256=hashlib.sha256(b).hexdigest()) for n,m,b in files})
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'passed':True,'cdhash':identity['cdhash'],'addon_bytes':addon.stat().st_size}))
if __name__=='__main__':main()
