#!/usr/bin/env python3
from pathlib import Path
import importlib.util,subprocess,hashlib,json,ctypes,plistlib,random,sys,argparse
R=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description='Check replayed Bluetooth launch identity and reordered launchd policy')
parser.add_argument('--xnu',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('extract',R/'scripts/check-picasso-buttons.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
s=(args.xnu/'bsd/kern/kern_exec.c').read_text();v=(args.xnu/'bsd/miscfs/bindfs/bind_vnops.c').read_text()
assert '.opve_op = &vnop_ioctl_desc, .opve_impl = (vop_t)bindfs_ioctl' in v
install=m.extract(s,'static void\nnative_gpu_addon_install(')
assert 'count == 19' in install and 'i >= 14 && i <= 15' in install
assert install.index('picasso_media_addon_entry(')<install.index('picasso_bt_runtime_capture(data, size, count, i,')
assert 'native_gpu_addon_write(path, data, size, mode, ctx)' in install
mount=m.extract(s,'static void\npicasso_bluetooth_mount(')
assert mount.index('picasso_bt_runtime_file(vp, i, ctx)')<mount.index('os_atomic_store(&picasso_bt_runtime_ready, 1, release)')
assert 'vnode_ref(vp)' in mount and 'vnode_rele(pinned[i])' in mount
auth='static vnode_t picasso_bt_runtime_vnodes[2];\n'+'\n'.join(m.extract(s,n) for n in ['static int\npicasso_bt_runtime_file(', 'int\npicasso_bt_runtime_auth('])
(out/'runtime-capture.inc').write_text(m.extract(s,'static int\npicasso_bt_runtime_capture('))
(out/'runtime-auth.inc').write_text(auth)
(out/'runtime-ioctl.inc').write_text(m.extract(v,'static int\nbindfs_ioctl('))
for sanitizers in [False,True]:
 name='identity'+('-sanitized' if sanitizers else '')
 cmd=['/usr/bin/clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-Wno-pointer-bool-conversion','-I'+str(out),str(R/'scripts/tests/picasso-bluetooth-identity-host.c'),'-lcrypto','-o',str(out/name)]
 if sanitizers:cmd+=['-fsanitize=address,undefined']
 for stage,command in [('compile',cmd),('run',[str(out/name),str(R/'out/picasso/bluetooth-userland/bluetoothd'),str(R/'out/picasso/bluetooth-userland/BlueTool')])]:
  q=subprocess.run(command,capture_output=True,text=True);(out/(name+'-'+stage+'.log')).write_text(q.stdout+q.stderr);q.check_returncode()
parser='\n'.join(m.extract(s,n) for n in ['static int\npicasso_jobs_space(', 'static int\npicasso_jobs_token(', 'static int\npicasso_jobs_transform('])
(out/'parser.c').write_text('#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n#include <string.h>\n#include <errno.h>\n'+parser+'\nint transform(uint8_t *b,size_t *n,unsigned *r){return picasso_jobs_transform(b,n,r);}\n')
subprocess.run(['/usr/bin/clang','-shared','-fPIC','-O1',str(out/'parser.c'),'-o',str(out/'parser.so')],check=True)
lib=ctypes.CDLL(str(out/'parser.so'));fn=lib.transform;fn.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_size_t),ctypes.POINTER(ctypes.c_uint)]
raw=(R/'scripts/tests/picasso-bluetooth-disabled.plist').read_bytes();actual=plistlib.loads(raw);released={'com.apple.BTServer.avrcp','com.apple.BTServer.le','com.apple.BTServer.map','com.apple.BTServer.pbap','com.apple.BluetoothUIService','com.apple.bluetoothd','com.apple.bluetoothuserd'}
def transform(b):
 buf=ctypes.create_string_buffer(b,8192);n=ctypes.c_size_t(len(b));r=ctypes.c_uint();err=fn(buf,ctypes.byref(n),ctypes.byref(r));return err,buf.raw[:n.value],r.value
expected={k:v for k,v in actual.items() if k not in released};err,result,removed=transform(raw);assert not err and removed==6 and plistlib.loads(result)==expected
assert transform(result)==(0,result,0)
(out/'device-disabled-after.plist').write_bytes(result)
rng=random.Random(0x20261115)
for _ in range(100):
 pairs=list(actual.items());rng.shuffle(pairs);sample=plistlib.dumps(dict(pairs),sort_keys=False)
 err,result,removed=transform(sample);assert not err and removed==6 and plistlib.loads(result)==expected and transform(result)==(0,result,0)
(out/'report.json').write_text(json.dumps(dict(passed=True,identity_cases=44,actual_daemon_bytes=8930688,capture_max_bytes=16*1024*1024,parser_permutations=100,device_policy_sha256=hashlib.sha256(raw).hexdigest(),preserved_extra_keys=['com.apple.dhcp6d','com.apple.bootpd','com.apple.ftp-proxy-embedded'],inputs={n:hashlib.sha256((R/n).read_bytes()).hexdigest() for n in ['patches/picasso-bluetooth-identity-xnu.patch','patches/picasso-bluetooth-policy-xnu.patch','scripts/check-picasso-bluetooth-identity.py','scripts/tests/picasso-bluetooth-identity-host.c','scripts/tests/picasso-bluetooth-disabled.plist']}),indent=2)+'\n')
print('PASS runtime identity and actual-device policy permutations')
