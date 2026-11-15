#!/usr/bin/env python3
"""Exercise the actual UART.SK FIFO and channel callbacks with host ring/lock doubles."""
from pathlib import Path
import subprocess, hashlib, json
r=Path(__file__).resolve().parents[1];o=r/'out/transport-test';o.mkdir(parents=True,exist_ok=True)
s=(r/'src/qbt_transport.c').read_text()
# Include the production callbacks unchanged; only kernel APIs are substituted.
start=s.index('#define BT_SK_FIFO');end=s.index('static void bt_sk_destroy')
(o/'transport-unit.inc').write_text(s[start:end])
cmd=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-fsanitize=address,undefined','-pthread','-I'+str(o),'-I'+str(r/'include'),str(r/'tests/transport.c'),'-o',str(o/'transport')]
subprocess.run(cmd,check=True)
p=subprocess.run([o/'transport'],text=True,capture_output=True,timeout=20)
(o/'output.txt').write_text(p.stdout+p.stderr);print(p.stdout+p.stderr,end='');p.check_returncode()
(o/'report.json').write_text(json.dumps(dict(passed=True,kernel_simulated=True,source_sha256=hashlib.sha256(s.encode()).hexdigest()),indent=2)+'\n')
