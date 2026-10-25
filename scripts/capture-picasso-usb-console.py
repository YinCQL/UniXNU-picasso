#!/usr/bin/env python3
"""Capture the Picasso CDC-ACM kernel console on macOS."""
import argparse
from datetime import datetime
import glob
import os
from pathlib import Path
import select
import sys
import termios
import time


def serial_ports():
    return sorted(glob.glob('/dev/cu.usbmodem*'))


def configure_serial(fd):
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='serial device, e.g. /dev/cu.usbmodem1101')
    parser.add_argument('--output', type=Path, help='log file (defaults to out/picasso/logs)')
    parser.add_argument('--duration', type=float, help='stop after this many seconds')
    parser.add_argument('--wait', type=float, default=0,
                        help='wait up to this many seconds for the serial port')
    parser.add_argument('--list', action='store_true', help='list matching serial devices')
    parser.add_argument('--no-stdout', action='store_true', help='write only to the log file')
    args = parser.parse_args()

    ports = serial_ports()
    if args.list:
        print('\n'.join(ports) if ports else 'No /dev/cu.usbmodem* devices found.')
        return 0
    if args.duration is not None and args.duration <= 0:
        parser.error('--duration must be greater than zero')
    if args.wait < 0:
        parser.error('--wait must be zero or greater')
    deadline = time.monotonic() + args.wait
    while not (os.path.exists(args.port) if args.port else ports):
        if time.monotonic() >= deadline:
            parser.error('No USB modem appeared within the wait period')
        time.sleep(min(0.1, max(0, deadline - time.monotonic())))
        ports = serial_ports()
    if args.port:
        port = args.port
    elif len(ports) == 1:
        port = ports[0]
    elif not ports:
        parser.error('No USB modem found; connect the device or pass --port explicitly')
    else:
        parser.error('Multiple USB modems found; choose one with --port')

    script_root = Path(__file__).resolve().parents[1]
    stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
    output = args.output or script_root / 'out/picasso/logs' / f'picasso-usb-console-{stamp}.log'
    output.parent.mkdir(parents=True, exist_ok=True)

    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_serial(fd)
        deadline = time.monotonic() + args.duration if args.duration else None
        print(f'Recording {port} to {output} (Ctrl-C to stop)', file=sys.stderr)
        with output.open('ab') as log:
            while deadline is None or time.monotonic() < deadline:
                timeout = 0.5
                if deadline is not None:
                    timeout = min(timeout, max(0, deadline - time.monotonic()))
                ready, _, _ = select.select([fd], [], [], timeout)
                if not ready:
                    continue
                try:
                    data = os.read(fd, 65536)
                except BlockingIOError:
                    continue
                if not data:
                    continue
                log.write(data)
                log.flush()
                if not args.no_stdout:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        pass
    except OSError as error:
        print(f'USB serial capture stopped: {error}', file=sys.stderr)
        return 1
    finally:
        os.close(fd)
    print(f'Log saved to {output}', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
