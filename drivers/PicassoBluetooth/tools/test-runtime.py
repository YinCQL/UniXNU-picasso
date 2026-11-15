#!/usr/bin/env python3
"""Run the original btqca startup and native H4/IBS executor against a controller model."""
from pathlib import Path
import hashlib,json,subprocess,sys
root=Path(__file__).resolve().parents[1]
subprocess.run([sys.executable,root/'tools/prepare-linux.py'],check=True)
out=root/'out/runtime-test';out.mkdir(parents=True,exist_ok=True)
sources=['src/qbt_core.c','src/qbt_linux.c','linux/btqca.c','tests/runtime.c']
command=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-fsanitize=address,undefined',
    *['-I'+str(root/d) for d in ['include','src','shim','linux']],*[str(root/f) for f in sources],'-o',str(out/'runtime')]
subprocess.run(command,check=True)
result=subprocess.run([out/'runtime'],text=True,capture_output=True)
(out/'output.txt').write_text(result.stdout+result.stderr)
if result.returncode:raise SystemExit((result.stdout+result.stderr)[-5000:])
print(result.stdout,end='')
(out/'report.json').write_text(json.dumps(dict(passed=True,radio_simulated=True,hardware_verified=False,
    inputs={s:hashlib.sha256((root/s).read_bytes()).hexdigest() for s in sources},output=result.stdout),indent=2)+'\n')
