#!/usr/bin/env python3
"""Check exact stock inputs, generated C bytes and missing-input rejection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import picasso_gpu_firmware as firmware

ROOT = Path(__file__).resolve().parents[1]


def check_handoff(out, generated):
    """Compile the generated TU against the real public firmware table."""
    shutil.copyfile(ROOT / 'drivers/NativeAdrenoPicasso/Adreno620Probe.h',
                    generated / 'Adreno620Probe.h')
    source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "generated/Adreno620Probe.h"
static struct native_adreno620_firmware captured;
static unsigned calls;
void native_adreno_set_firmware(const struct native_adreno620_firmware *fw) {
    captured = *fw; calls++;
}
#include "generated/adreno620_embedded_firmware.c"
static void check_blob(const uint8_t *blob, size_t bytes, const char *name) {
    uint8_t buffer[1024]; size_t at = 0;
    FILE *f = fopen(name, "rb"); assert(f && blob && bytes);
    while (at < bytes) {
        size_t n = bytes - at; if (n > sizeof buffer) n = sizeof buffer;
        assert(fread(buffer, 1, n, f) == n);
        assert(!memcmp(buffer, blob + at, n)); at += n;
    }
    assert(fgetc(f) == EOF && !ferror(f)); assert(!fclose(f));
}
int main(void) {
    native_adreno620_set_embedded_firmware(); assert(calls == 1);
    check_blob(captured.gmu, captured.gmu_bytes, "firmware/a650_gmu.bin");
    check_blob(captured.sqe, captured.sqe_bytes, "firmware/a650_sqe.fw");
    check_blob(captured.zap_mdt, captured.zap_mdt_bytes, "firmware/a620_zap.mdt");
    check_blob(captured.zap_segment[0], captured.zap_segment_bytes[0], "firmware/a620_zap.b00");
    check_blob(captured.zap_segment[1], captured.zap_segment_bytes[1], "firmware/a620_zap.b01");
    check_blob(captured.zap_segment[2], captured.zap_segment_bytes[2], "firmware/a620_zap.b02");
    check_blob(native_adreno620_fw_sqe_alt, native_adreno620_fw_sqe_alt_size, "firmware/a630_sqe.fw");
    const uint8_t *gmu = captured.gmu;
    native_adreno620_set_embedded_firmware();
    assert(calls == 2 && captured.gmu == gmu);
    puts("PASS: generated C setter, seven stock arrays and static buffer lifetime");
    return 0;
}
'''
    (out / 'handoff.c').write_text(source, newline='\n')
    compiler = runpy.run_path(str(ROOT / 'scripts/check-picasso-user-fault.py'))
    vcvars = compiler['find_vcvars']() if os.name == 'nt' else None
    checks = []
    for asan in (False, True):
        name = 'handoff-' + ('asan' if asan else 'plain')
        result, exe = compiler['compile_case'](out, name, 'handoff.c', asan,
                                             os.environ.get('CC', 'clang'), vcvars)
        assert result['exit'] == 0, result
        log = out / (name + '-run.log')
        if vcvars:
            run = compiler['batch_run'](out, name + '-run',
                    f'{exe} > {log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
        else:
            run = subprocess.run([str(out / exe)], cwd=out, capture_output=True, text=True)
            log.write_text(run.stdout + run.stderr)
        assert run.returncode == 0 and 'PASS: generated C setter' in log.read_text()
        checks.append(dict(case=name, result='pass', compile=result, run_exit=run.returncode))
    return checks


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    staged = out / 'firmware'
    command = [sys.executable, str(ROOT / 'scripts/fetch-picasso-gpu-firmware.py'), '--output', str(staged)]
    if args.source: command += ['--source', str(args.source.resolve())]
    subprocess.run(command, check=True)
    report = firmware.verify_directory(staged)
    cases = []

    def rejected(name, operation):
        try: operation()
        except ValueError: cases.append(dict(case=name, result='rejected'))
        else: raise AssertionError(name)

    board = (ROOT / 'drivers/NativeAdrenoPicasso/Adreno620Board.cpp').read_text()
    for name, (size, sha, crc) in firmware.FILES.items():
        m = re.search('"' + re.escape(name) + r'", (\d+)u, 0x([0-9a-f]+)u,\s*\{([^}]+)\}', board)
        assert m and int(m[1]) == size and int(m[2], 16) == crc
        assert bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m[3])).hex() == sha
        blob = (staged / name).read_bytes()
        cases.append(dict(case='stock descriptor: ' + name, result='pass'))
        rejected('changed bytes: ' + name, lambda n=name, b=blob: firmware.verify(n, bytes([b[0] ^ 1]) + b[1:]))
    rejected('changed Release archive', lambda: firmware.unpack(b'wrong'))
    generated = out / 'generated'

    def generate(directory, output):
        env = dict(os.environ, ADRENO620_FW_DIR=str(directory), ADRENO620_FW_OUT=str(output))
        return subprocess.run([sys.executable, str(ROOT / 'scripts/build-adreno-picasso-firmware.py')],
                              env=env, capture_output=True, text=True)

    result = generate(staged, generated)
    (out / 'generate.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0
    c = (generated / 'adreno620_embedded_firmware.c').read_text()
    names = dict(gmu='a650_gmu.bin', sqe='a650_sqe.fw', sqe_alt='a630_sqe.fw', zap_mdt='a620_zap.mdt',
                 zap_b00='a620_zap.b00', zap_b01='a620_zap.b01', zap_b02='a620_zap.b02')
    for symbol, name in names.items():
        m = re.search(r'const uint8_t native_adreno620_fw_' + symbol + r'\[\] = \{([^}]+)\};', c)
        blob = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m[1]))
        assert hashlib.sha256(blob).hexdigest() == firmware.FILES[name][1]
        assert f'native_adreno620_fw_{symbol}_size = {len(blob)}u;' in c
        cases.append(dict(case='generated C bytes: ' + name, result='pass'))
    cases.extend(check_handoff(out, generated))
    empty = out / 'empty'; empty.mkdir()
    wrong = out / 'wrong'; shutil.copytree(staged, wrong)
    (wrong / 'a650_gmu.bin').write_bytes(bytes(firmware.FILES['a650_gmu.bin'][0]))
    for name, directory in (('missing', empty), ('modified', wrong)):
        target = out / ('failed-' + name)
        result = generate(directory, target)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode != 0 and not target.exists()
        cases.append(dict(case='generator rejects ' + name + ' before writing output', result='pass'))
    (out / 'results.json').write_text(json.dumps(dict(cases=cases,stock=report,hardware_validated=False),indent=2)+'\n')
    print(f'PASS: {len(cases)} exact stock firmware and generated C checks')


if __name__ == '__main__':
    main()
