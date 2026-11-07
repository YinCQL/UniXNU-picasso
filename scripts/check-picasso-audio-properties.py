#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the production HAL property/mix and guarded route tracing bodies on a host.

Enum values come from the same real SDK headers as the production HAL. This
checks behavior under a narrow host API model, not iOS routing qualification.
"""
from pathlib import Path
import argparse,hashlib,json,os,re,subprocess
ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--reference',type=Path);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);runs=[]
    def run(name,command):
        r=subprocess.run(command,capture_output=True,text=True,timeout=120)
        (out/(name+'.log')).write_text(r.stdout+r.stderr)
        if r.returncode:raise RuntimeError(r.stdout+r.stderr)
        runs.append(dict(name=name,passed=True));print(name,'PASS',flush=True)
    source=(ROOT/'hal/PicassoAudioPlugIn.c').read_text()
    body=source[:source.index('static AudioServerPlugInDriverInterface gInterface=')]
    body=re.sub(r'^#include <(?:CoreAudio/|CoreFoundation/|IOKit/|mach/)[^\n]+\n','',body,flags=re.M)
    body=body.replace('static HRESULT QueryInterface(void *,REFIID,LPVOID *);','')
    trace=(ROOT/'hal/PicassoAudioRouteTrace.h').read_text()
    trace=re.sub(r'^#include <[^\n]+\n','',trace,flags=re.M)
    body=body.replace('#include "PicassoAudioRouteTrace.h"',trace)
    names=sorted(set(re.findall(r'\bk(?:Audio|IOReturn)[A-Za-z0-9_]+\b',body)))
    c=out/'sdk-constants.c';c.write_text('#include <CoreAudio/AudioServerPlugIn.h>\n#include <IOKit/IOKitLib.h>\n'+''.join('const unsigned long long value_'+n+' = '+n+';\n' for n in names))
    sdk=os.environ.get('PICASSO_AUDIO_SDK','/home/y/toolchains/audio-sdks/iPhoneOS16.5.sdk')
    headers=os.environ.get('PICASSO_AUDIO_HEADERS','/home/y/toolchains/audio-hal-headers')
    legacy_header=Path(headers)/'CoreAudio/AudioHardware.h'
    legacy=legacy_header.read_text()
    for local,official in [('VolumeScalar','VolumeScalar'),('VolumeDecibels','VolumeDecibels'),('VolumeRangeDecibels','VolumeRangeDecibels'),('Mute','Mute')]:
        sdk_value=re.search(r'\bkAudioDeviceProperty'+official+r"\s*=\s*'([^']+)'",legacy)
        local_value=re.search(r'\bkDevice'+local+r"\s*=\s*'([^']+)'",source)
        assert sdk_value and local_value and sdk_value[1]==local_value[1],local
    runs.append(dict(name='device-alias-sdk-values',passed=True))
    cc=os.environ.get('PICASSO_AUDIO_CC','/usr/bin/clang-18')
    ir=out/'sdk-constants.ll'
    run('sdk-constants',[cc,'-target','arm64-apple-ios16.0','-isysroot',sdk,'-I',headers,'-S','-emit-llvm',str(c),'-o',str(ir)])
    values=dict(re.findall(r'@value_(\w+) =[^\n]*constant i64 (-?\d+)',ir.read_text()))
    assert set(values)==set(names)
    (out/'sdk-constants.h').write_text(''.join('#define '+n+' '+('((int32_t)'+v+'U)' if n.startswith('kIOReturn') else v+'U')+'\n' for n,v in values.items()))
    (out/'plugin-body.inc').write_text(body)
    for sanitized in [False,True]:
        suffix='-asan' if sanitized else '';exe=out/('properties'+suffix)
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if sanitized else []
        run('compile'+suffix,[cc,'-std=c11','-D_DEFAULT_SOURCE','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',*flags,'-I',str(out),'-I',str(ROOT/'package'),'-I',str(ROOT/'hal'),str(ROOT/'scripts/tests/picasso-audio-properties-host.c'),'-pthread','-lm','-o',str(exe)])
        run('properties'+suffix,[str(exe)]+([str(a.reference)] if a.reference else []))
    paths=['hal/PicassoAudioPlugIn.c','hal/PicassoAudioRouteTrace.h','hal/PicassoAudioMix.h','package/PicassoAudioAbi.h','scripts/check-picasso-audio-properties.py','scripts/tests/picasso-audio-properties-host.c']
    (out/'results.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,runs=runs,legacy_header=dict(path=str(legacy_header),sha256=hashlib.sha256(legacy_header.read_bytes()).hexdigest()),reference_sha256=hashlib.sha256(a.reference.read_bytes()).hexdigest() if a.reference else None,inputs={n:hashlib.sha256((ROOT/n).read_bytes()).hexdigest() for n in paths}),indent=2)+'\n')
if __name__=='__main__':main()
