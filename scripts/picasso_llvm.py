"""Locate LLVM tools used to edit and inspect the qualified Mach-O objects."""
import os
from pathlib import Path
import platform
import shutil
import subprocess


def tool(name):
    override = os.environ.get('UNIXNU_LLVM_BIN')
    if override:
        path = Path(override) / name
    elif platform.system() == 'Darwin':
        prefix = subprocess.check_output(['brew', '--prefix', 'llvm'], text=True).strip()
        path = Path(prefix) / 'bin' / name
    else:
        found = shutil.which(name)
        if not found:
            raise RuntimeError(f'{name} missing; install LLVM or set UNIXNU_LLVM_BIN')
        path = Path(found)
    if not path.is_file():
        raise RuntimeError(f'{path} missing')
    return path
