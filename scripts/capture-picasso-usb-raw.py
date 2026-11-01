#!/usr/bin/env python3
"""Capture and control the Picasso CDC console through libusb directly.

macOS's AppleUSBACMData can block indefinitely when the device side has not
finished tty setup. The XNU console itself only needs the two CDC bulk pipes,
so this tool avoids the tty driver entirely.
"""
import argparse
from datetime import datetime
from pathlib import Path
import sys
import time

import usb.core
import usb.util


VENDOR = 0x1209
PRODUCT = 0x0001
DATA_INTERFACE = 1
BULK_IN = 0x81
BULK_OUT = 0x01


def monotonic_now():
    return time.monotonic()


def open_device(timeout):
    deadline = monotonic_now() + timeout
    while True:
        device = usb.core.find(idVendor=VENDOR, idProduct=PRODUCT)
        if device is not None:
            try:
                device.set_configuration(1)
                usb.util.claim_interface(device, DATA_INTERFACE)
                return device
            except usb.core.USBError:
                try:
                    usb.util.dispose_resources(device)
                except Exception:
                    pass
        if monotonic_now() >= deadline:
            return None
        time.sleep(0.2)


def close_device(device):
    if device is None:
        return
    try:
        usb.util.release_interface(device, DATA_INTERFACE)
    except Exception:
        pass
    try:
        usb.util.dispose_resources(device)
    except Exception:
        pass


def write_all(device, data, timeout_ms):
    view = memoryview(data)
    while view:
        written = device.write(BULK_OUT, view, timeout=timeout_ms)
        if written <= 0:
            raise usb.core.USBError('bulk OUT accepted zero bytes')
        view = view[written:]


def parse_timed_command(value):
    try:
        at, command = value.split('=', 1)
        at = float(at)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            'command must use SECONDS=COMMAND') from error
    if at < 0 or not command:
        raise argparse.ArgumentTypeError('invalid timed command')
    return at, command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        help='capture file (default: timestamped log)')
    parser.add_argument('--duration', type=float, required=True)
    parser.add_argument('--wait', type=float, default=60,
                        help='seconds to wait for the CDC device')
    parser.add_argument('--send', default='status',
                        help='repeating heartbeat command (default: status)')
    parser.add_argument('--send-interval', type=float, default=15)
    parser.add_argument('--timed-command', action='append', default=[],
                        type=parse_timed_command,
                        metavar='SECONDS=COMMAND')
    parser.add_argument('--reboot-fastboot', action='store_true',
                        help='send reboot-fastboot at the end of the capture')
    parser.add_argument('--fastboot-timeout', type=float, default=30)
    parser.add_argument('--fastboot-serial', default='52cc791d')
    args = parser.parse_args()

    if args.duration <= 0 or args.wait < 0 or args.send_interval <= 0:
        parser.error('invalid timing argument')
    for at, _ in args.timed_command:
        if at > args.duration:
            parser.error('timed command is after the capture deadline')

    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    output = args.output or Path(
        f'/tmp/picasso-usb-raw-{stamp}.log')
    output.parent.mkdir(parents=True, exist_ok=True)
    started = monotonic_now()
    deadline = started + args.duration
    heartbeat_at = started
    timed = sorted(args.timed_command)
    timed_index = 0
    device = None
    reconnect_deadline = started + args.wait

    with output.open('ab') as log:
        while monotonic_now() < deadline:
            now = monotonic_now()
            if device is None:
                if now >= reconnect_deadline:
                    print('No Picasso CDC device appeared', file=sys.stderr)
                    return 1
                device = open_device(min(1.0, reconnect_deadline - now))
                if device is None:
                    continue
                heartbeat_at = now
                print(f'Connected to {VENDOR:04x}:{PRODUCT:04x}',
                      file=sys.stderr)

            try:
                now = monotonic_now()
                if args.send and now >= heartbeat_at:
                    write_all(device, (args.send + '\r\n').encode('ascii'), 1000)
                    heartbeat_at = now + args.send_interval
                while timed_index < len(timed) and now >= started + timed[timed_index][0]:
                    command = timed[timed_index][1]
                    write_all(device, (command + '\r\n').encode('ascii'), 1000)
                    print(f'Sent timed command: {command}', file=sys.stderr)
                    timed_index += 1
                try:
                    data = device.read(BULK_IN, 65536, timeout=250)
                except usb.core.USBTimeoutError:
                    continue
                if data:
                    payload = bytes(data)
                    log.write(payload)
                    log.flush()
            except usb.core.USBError as error:
                print(f'USB device disconnected: {error}', file=sys.stderr)
                close_device(device)
                device = None
                reconnect_deadline = min(deadline, monotonic_now() + args.wait)

        if args.reboot_fastboot:
            if device is None:
                device = open_device(args.wait)
            if device is not None:
                write_all(device, b'reboot-fastboot\r\n', 1000)
                print('Sent reboot-fastboot', file=sys.stderr)

    close_device(device)
    print(f'Log saved to {output}', file=sys.stderr)

    if args.reboot_fastboot:
        fastboot_deadline = monotonic_now() + args.fastboot_timeout
        while monotonic_now() < fastboot_deadline:
            import subprocess
            result = subprocess.run(
                ['fastboot', 'devices'], capture_output=True, text=True,
                check=False, timeout=3)
            for line in result.stdout.splitlines():
                if line.split()[:2] == [args.fastboot_serial, 'fastboot']:
                    print(result.stdout.strip(), file=sys.stderr)
                    return 0
            time.sleep(0.25)
        print('Fastboot did not return before timeout', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
