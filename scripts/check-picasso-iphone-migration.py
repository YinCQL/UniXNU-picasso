#!/usr/bin/env python3
"""Host checks of iPhone identity, actual payload flag forwarding and trust pins.

Requires the replayed upstream tree and read-only extracted 20H392 inputs.
Does not build an SDK kernel, format Data, modify firmware or operate a phone.
"""
import argparse
import ast
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib
from types import SimpleNamespace
import zipfile

import picasso_target as target


def load(path):
    spec = importlib.util.spec_from_file_location(path.stem.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--backend-ipsw', type=Path, help='original 21H461 IPSW for separate-donor checks')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    adt_checker = load(root / 'scripts/check-picasso-adt.py')
    trust_builder = load(root / 'scripts/prepare-original-trust.py')
    target_checker = load(root / 'scripts/check-picasso-target.py')
    board = target_checker.check_board(root)
    backend_checks = []
    if args.backend_ipsw:
        backend = load(root / 'scripts/prepare-picasso-kernelcache.py')
        report = backend.prepare(args.tree, str(args.backend_ipsw), out / 'backend')
        assert report['kernelcache_sha256'] == target.BACKEND_KERNEL_SHA256
        backend_checks.append('original-21H461-extraction')
        with zipfile.ZipFile(args.backend_ipsw) as archive:
            backend_manifest = archive.read('BuildManifest.plist')
        member = (out / 'backend' / target.BACKEND_IPSW['kernel_member']).read_bytes()
        for case, manifest, contents in (
                ('wrong-userland-donor', (args.inputs / 'BuildManifest.plist').read_bytes(), member),
                ('corrupt-donor', backend_manifest, member[:-1] + bytes([member[-1] ^ 1]))):
            fixture = out / (case + '.ipsw')
            with zipfile.ZipFile(fixture, 'x') as archive:
                archive.writestr('BuildManifest.plist', manifest)
                archive.writestr(target.BACKEND_IPSW['kernel_member'], contents)
            destination = out / (case + '-output')
            try: backend.prepare(args.tree, str(fixture), destination)
            except ValueError: backend_checks.append(case + '-rejected')
            else: raise AssertionError('Invalid native kernel donor accepted')
            assert not destination.exists()
        try: backend.prepare(args.tree, str(args.backend_ipsw), out / 'backend')
        except FileExistsError: backend_checks.append('existing-output-retained')
        else: raise AssertionError('Existing native kernel donor overwritten')
        assert hashlib.sha256((out / 'backend/kernelcache.decompressed').read_bytes()).hexdigest() == target.BACKEND_KERNEL_SHA256
    policy_rejected = []
    for case in ('manifest', 'split-table', 'assembly', 'uuid', 'old-root'):
        policy = out / ('bad-policy-' + case)
        shutil.copytree(root / 'policy/verified-root-picasso', policy)
        if case in ('manifest', 'split-table', 'assembly'):
            path = policy / {'manifest': 'os/manifest.bin', 'split-table': 'systemos/hashes.bin', 'assembly': 'policy.S'}[case]
            data = path.read_bytes()
            path.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
        else:
            path = policy / 'attestation.json'
            audit = json.loads(path.read_text())
            if case == 'uuid': audit['windows'][0]['volume_uuid'] = '04769D26-2CA0-4825-B443-E2E5F88300F2'
            else: audit['root_image']['sha256'] = 'fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093'
            path.write_text(json.dumps(audit))
        try: target_checker.check_policy(policy)
        except ValueError: policy_rejected.append(case)
        else: raise AssertionError('Invalid root policy accepted: ' + case)
    sys.path.insert(0, str(args.tree.resolve() / 'scripts'))
    from unixnu.ipsw import im4p_payload
    modules = [im4p_payload((args.inputs / name).read_bytes()) for name, _ in target.TRUST_MODULES]
    bundle = trust_builder.bundle_modules(modules)
    assert hashlib.sha256(bundle).hexdigest() == board['payload']['trust_cache_sha256'][0]
    rejected = []
    for name, bad in [('reverse', list(reversed(modules))), ('truncate', [modules[0][:-1], modules[1]]),
                      ('mutation', [modules[0][:100] + bytes([modules[0][100] ^ 1]) + modules[0][101:], modules[1]]),
                      ('count', modules[:1])]:
        try: trust_builder.bundle_modules(bad)
        except ValueError: rejected.append(name)
        else: raise AssertionError('Invalid trust modules accepted: ' + name)

    # Run the actual command-construction statements from the replayed builder;
    # mount/FAT operations are outside this host check. Execute the resulting ADT
    # command for real so a config key with no forwarded CLI flag cannot pass.
    payload_file = args.tree / 'scripts/unixnu/payload.py'
    source = payload_file.read_text()
    parsed = ast.parse(source)
    function = next(n for n in parsed.body if isinstance(n, ast.FunctionDef) and n.name == 'build')
    begin = next(i for i, n in enumerate(function.body) if isinstance(n, ast.Assign)
                 and any(isinstance(v, ast.Name) and v.id == 'command' for v in n.targets)
                 and isinstance(n.value, ast.List))
    end = next(i for i, n in enumerate(function.body) if i > begin and isinstance(n, ast.For)
               and isinstance(n.target, ast.Name) and n.target.id == 'fixup')
    command_code = compile(ast.Module(body=function.body[begin:end], type_ignores=[]), str(payload_file), 'exec')
    cases = []
    for name, identity, packed in [('iphone', 'iphone', False), ('iphone-data', 'iphone', True),
                                   ('ipad-compatibility', 'ipad', False)]:
        adt = copy.deepcopy(board['payload']['adt'])
        adt['iphone_identity'] = identity == 'iphone'; adt['ipad_identity'] = identity == 'ipad'
        directory = out / name; directory.mkdir()
        env = dict(ctx=SimpleNamespace(tree=root), adt=adt, payloads=directory,
                   spec=board['payload'], seed='1', data=packed, trust=True, addon=False)
        exec(command_code, env)
        command = env['command']; command[0] = sys.executable
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        (directory / 'generator.txt').write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr
        nodes = adt_checker.decode((directory / 'kernel.macho.adt').read_bytes())
        if identity == 'iphone':
            assert nodes['']['model'] == b'iPhone10,3\0' and nodes['']['target-type'] == b'D22\0'
            expected = {'product-name': b'iPhone X\0', 'product-description': b'iPhone X\0',
                        'artwork-device-idiom': b'phone\0', 'artwork-device-subtype': struct.pack('<I', 2436),
                        'artwork-scale-factor': struct.pack('<I', 3), 'artwork-display-gamut': b'P3\0',
                        'compatible-device-fallback': b'iPhone9,2\0', 'thin-bezel': struct.pack('<I', 1)}
            assert all(nodes['/product'].get(k) == v for k, v in expected.items())
            assert nodes['/chosen']['display-scale'] == struct.pack('<I', 3)
        else:
            assert nodes['']['model'] == b'iPad7,1\0' and nodes['/product']['artwork-device-idiom'] == b'pad\0'
            assert 'display-scale' not in nodes['/chosen']
        assert nodes['']['compatible'] == b'xiaomi,picasso\0qcom,lito\0qcom,lito-mtp\0'
        assert nodes['/buttons']['button-names'] == b'hold\0voldown\0volup\0'
        assert nodes['/buttons']['home-button-type'] == struct.pack('<I', 2)
        for key, value in [('certificate-production-status', 0), ('effective-production-status-ap', 0),
                           ('certificate-security-mode', 1), ('effective-security-mode-sep', 1)]:
            assert nodes['/chosen'][key] == struct.pack('<I', value)
        assert ('ephemeral-storage' in nodes['/chosen']) == packed
        cases.append(name)
    result = subprocess.run([sys.executable, str(root / 'scripts/make-picasso-adt.py'), '--output',
                             str(out / 'conflicting.adt'), '--ipad-identity', '--iphone-identity'], capture_output=True, text=True)
    assert result.returncode != 0 and not (out / 'conflicting.adt').exists()
    gates = []
    for command in ([sys.executable, str(root / 'scripts/check-picasso-target.py'), '--require-build-ready'],
                    [sys.executable, str(root / 'unixnu'), 'build', 'kernel', 'payload'],
                    [sys.executable, str(root / 'unixnu'), 'build'],
                    [sys.executable, str(root / 'unixnu'), 'build', 'all'],
                    [sys.executable, str(root / 'unixnu'), 'build', '-j', '4'],
                    [sys.executable, str(root / 'unixnu'), 'build', 'prelink'],
                    [sys.executable, str(root / 'unixnu'), 'bootimg']):
        result = subprocess.run(command, capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and 'migration is not build-ready' in result.stderr
        gates.append(command[1:])
    report = dict(passed=True, scope=__doc__, adt_cases=cases, rejected_trust_inputs=rejected,
                  rejected_policy_inputs=policy_rejected,
                  retained_backend_checks=backend_checks,
                  conflicting_identity_rejected=True, incomplete_migration_gates=gates,
                  original_trust_bytes=len(bundle), payload_source_sha256=hashlib.sha256(source.encode()).hexdigest())
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: actual payload flag forwarding, phone/iPad ADTs, original trust inputs and incomplete-migration gates')


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
