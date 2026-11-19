#!/usr/bin/env python3
"""Prepare pinned Picasso firmware for existing kernel/media build paths."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import picasso_firmware as firmware


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=firmware.device_root() / 'out/picasso')
    parser.add_argument('--group', action='append', choices=firmware.GROUPS)
    parser.add_argument('--novatek-source', type=Path)
    gpu = parser.add_mutually_exclusive_group()
    gpu.add_argument('--gpu-source', type=Path)
    gpu.add_argument('--gpu-archive', type=Path)
    bluetooth = parser.add_mutually_exclusive_group()
    bluetooth.add_argument('--bluetooth-source', type=Path)
    bluetooth.add_argument('--btfm', type=Path)
    parser.add_argument('--non-hlos', type=Path)
    args = parser.parse_args()
    scripts = Path(__file__).resolve().parent
    groups = args.group or ['novatek', 'adreno620', 'bluetooth']
    results = {}
    # Existing outputs are revalidated; a corrupt local file never triggers a download.
    for group in dict.fromkeys(groups):
        pins = firmware.manifest(group)
        if group == 'novatek':
            command = [sys.executable, '-B', str(scripts / 'fetch-picasso-touch-firmware.py'),
                       '--output', str(args.work / 'novatek/firmware.bin')]
            if args.novatek_source:
                command += ['--source', str(args.novatek_source)]
            subprocess.run(command, check=True)
            results[group] = dict(files=pins['files'], output='novatek/firmware.bin')
        elif group == 'adreno620':
            command = [sys.executable, '-B', str(scripts / 'fetch-picasso-gpu-firmware.py'),
                       '--output', str(args.work / 'gpu-firmware')]
            if args.gpu_source:
                command += ['--source', str(args.gpu_source)]
            if args.gpu_archive:
                command += ['--archive', str(args.gpu_archive)]
            subprocess.run(command, check=True)
            results[group] = dict(files=pins['files'], output='gpu-firmware')
        elif group == 'bluetooth':
            output = args.work / 'bluetooth-firmware'
            source = args.bluetooth_source
            if source is None and args.btfm is None and output.is_dir():
                source = output
            files = firmware.bluetooth_files(source, args.btfm)
            results[group] = dict(files=firmware.stage(output, files, pins), output='bluetooth-firmware')
        else:
            if args.non_hlos is None:
                args.non_hlos = firmware.device_root() / 'firmware/adsp/NON-HLOS.bin'
            from picasso_adsp_firmware import prepare
            image, files, report = prepare(args.non_hlos)
            output = args.work / 'adsp-firmware'
            import hashlib
            contents = dict(files, **{'adsp.mbn': image})
            generated = {'files': {name: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
                                   for name, data in contents.items()}}
            firmware.stage(output, contents, generated)
            (output / 'firmware.json').write_text(json.dumps(report, indent=2) + '\n')
            results[group] = dict(files=report['files'], output='adsp-firmware', runtime_loader_changed=False)
    args.work.mkdir(parents=True, exist_ok=True)
    report = dict(schema='picasso-firmware-prepared-v1', groups=results, hardware_tested=False)
    (args.work / 'firmware-inputs.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: prepared pinned Picasso firmware: ' + ', '.join(results))


if __name__ == '__main__':
    main()
