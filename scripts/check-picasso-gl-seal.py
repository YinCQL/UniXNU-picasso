from pathlib import Path
import hashlib,json,sys,copy
import argparse
r=Path(__file__).resolve().parents[1];sys.path.insert(0,str(r/'scripts'))
p=argparse.ArgumentParser();p.add_argument('--work',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
import picasso_nativegl as gl,picasso_media_payload as m
work=a.work.resolve();files=[x for x in m.unpack((work/'gpu72'/m.GPU_ADDON).read_bytes()) if x[0] in m.GPU_FILES]
adapted,report=gl.adapt(work,files);original=dict((n,b) for n,_,b in files);new=dict((n,b) for n,_,b in adapted)
assert sum(original[n]!=new[n] for n in original)==3
slot=report['plugin_resource_digest_offset'];n=gl.BASE+'NativeAdrenoMetal'
assert original[n][:slot]==new[n][:slot] and original[n][slot+32:]==new[n][slot+32:]
checks=0
for mutated in [bytes(bytearray(new[gl.BASE+'libNativeGL.dylib'])[:20])+new[gl.BASE+'libNativeGL.dylib'][21:],
                new[n][:500]+bytes([new[n][500]^1])+new[n][501:]]:
 try:m.identity(mutated)
 except ValueError:checks+=1
 else:raise AssertionError('Corrupted code accepted')
try:gl.replace_resources(original[n],b'bad original',new[gl.BASE+'_CodeSignature/CodeResources'])
except ValueError:checks+=1
else:raise AssertionError('Incorrect original seal accepted')
bad=[(n,mode,b+b'x' if n==gl.BASE+'libOSMesa.8.dylib' else b) for n,mode,b in files]
try:gl.adapt(work,bad)
except ValueError:checks+=1
else:raise AssertionError('Modified pinned library accepted')
addon,trust,combined,*_=m.make(work)
hashes=sorted(bytes.fromhex(i['cdhash']) for i in combined['gpu_identities'].values())
entries=[m.modules(trust)[-1][i:i+24] for i in range(24,len(m.modules(trust)[-1]),24)]
assert entries==[h+bytes([2,0,0,0]) for h in hashes]
checks+=1
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'results.json').write_text(json.dumps(dict(passed=True,checks=checks,changed_files=3,plugin_code_unchanged=True,exact_gpu_entries=4,report=report),indent=2)+'\n')
print('PASS renderer/plugin code pages, exact resource seal, changed-file whitelist and four GPU trust entries')
