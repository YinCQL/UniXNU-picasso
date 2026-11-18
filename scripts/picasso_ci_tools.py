"""Prepare native ldid and patched, pinned XNU headers before media compilation."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

LDID = 'af86971ae72ec3ed3d0a699107c4e882324c941b'
XNU = 'd8b80295118ef25ac3a784134bcf95cd8e88109f'


def prepare_tools(work, root):
    tools = work / 'bluetooth-tools'
    source = tools / 'ldid-source'
    if not source.exists():
        subprocess.run(['git', 'clone', 'https://github.com/ProcursusTeam/ldid.git', str(source)], check=True)
        subprocess.run(['git', '-C', str(source), 'checkout', '--detach', LDID], check=True)
    if subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip() != LDID:
        raise ValueError('ldid source revision differs')
    env = dict(os.environ)
    prefixes = [subprocess.check_output(['brew', '--prefix', name], text=True).strip() for name in ('openssl@3', 'libplist')]
    env['PKG_CONFIG_PATH'] = ':'.join(p + '/lib/pkgconfig' for p in prefixes)
    plist = next((n for n in ('libplist-2.0', 'libplist') if subprocess.run(
        ['pkg-config', '--exists', n], env=env).returncode == 0), None)
    if not plist:
        raise ValueError('Native libplist pkg-config input is missing')
    def pkg(option):
        return subprocess.check_output(['pkg-config', option, plist], env=env, text=True).strip()
    subprocess.run(['make', '-C', str(source), '-j3', 'VERSION=2.1.5-procursus7',
                    'LIBPLIST_INCLUDES=' + pkg('--cflags'), 'LIBPLIST_LIBS=' + pkg('--libs')], env=env, check=True)
    signer = tools / 'ldid'
    shutil.copyfile(source / 'ldid', signer)
    signer.chmod(0o755)
    # Prepare only source inputs; kernel bootstrap retains its own source/cache.
    tree = Path(subprocess.check_output(['python3', '-B', str(root / 'unixnu'), '--prepare'], text=True).strip())
    reference = tree / 'refer/xnu'
    if not (reference / '.git').exists():
        subprocess.run(['git', 'clone', '--depth', '1', '--branch', 'xnu-10063.141.1',
                        'https://github.com/apple-oss-distributions/xnu.git', str(reference)], check=True)
    if subprocess.check_output(['git', '-C', str(reference), 'rev-parse', 'HEAD'], text=True).strip() != XNU:
        raise ValueError('XNU reference revision differs')
    target = work / 'ci-xnu'
    if target.exists():
        raise ValueError('CI media XNU source already exists')
    subprocess.run(['git', 'clone', '--shared', str(reference), str(target)], check=True)
    patches = sorted((tree / 'patches/xnu').glob('*.patch')) + sorted((tree / 'kits/XnuPrelink/patches').glob('*.patch'))
    for patch in patches:
        subprocess.run(['git', '-C', str(target), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(target), 'apply', str(patch)], check=True)
    proof = dict(ldid_revision=LDID, ldid_sha256=hashlib.sha256(signer.read_bytes()).hexdigest(),
                 xnu_revision=XNU, patches={str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest() for p in patches})
    (tools / 'input-report.json').write_text(json.dumps(proof, indent=2) + '\n')
    return signer, target
