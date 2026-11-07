#!/usr/bin/env python3
"""Read ngp=1 cumulative elapsed-time samples. Nested phases do not add."""
import argparse
import hashlib
import json
import re
from pathlib import Path

PHASES = 'call lock dispatch body validate retire audit publish switch drain ring acquire'.split()
OPTIONAL_PHASES = ['ibpeek']


def analyze(text):
    samples = []
    pending = None
    frame = flips = display_ms = 0
    for line_no, line in enumerate(text.splitlines(), 1):
        m = re.search(r'eagl f(\d+) i', line)
        if m:
            frame = int(m[1])
        m = re.search(r'PICASSO_DISPLAY frame (\d+).*time_ms=(\d+)', line)
        if m:
            flips, display_ms = map(int, m.groups())
        m = re.search(r'PICASSO_GPU prof (\w+) n=(\d+) us=(\d+) max=(\d+)', line)
        if m:
            phase = m[1]
            if phase == 'call':
                if pending and set(PHASES).issubset(pending['phases']):
                    samples.append(pending)
                pending = dict(line=line_no, frame=frame, flips=flips, display_ms=display_ms, phases={})
            if pending is not None:
                pending['phases'][phase] = dict(zip(('count', 'us', 'max_us'), map(int, m.groups()[1:])))
        m = re.search(r'PICASSO_GPU profscan pub=([\d/]+) acq=([\d/]+)', line)
        if m and pending is not None:
            pending['scan'] = {key: list(map(int, value.split('/'))) for key, value in zip(('pub', 'acq'), m.groups())}
        m = re.search(r'PICASSO_GPU profmaint pub=(\d+)/(\d+)K acq=(\d+)/(\d+)K', line)
        if m and pending is not None:
            vals = list(map(int, m.groups()))
            pending['maintenance'] = dict(pub=vals[:2], acq=vals[2:])
    if pending and set(PHASES).issubset(pending['phases']):
        samples.append(pending)
    intervals = []
    for a, b in zip(samples, samples[1:]):
        phases = PHASES + [p for p in OPTIONAL_PHASES if p in a['phases'] and p in b['phases']]
        calls = b['phases']['call']['count'] - a['phases']['call']['count']
        if calls <= 0 or any(b['phases'][p]['us'] < a['phases'][p]['us'] for p in PHASES):
            continue
        delta = dict(from_line=a['line'], to_line=b['line'], frame=b['frame'], frame_delta=b['frame']-a['frame'], calls=calls)
        delta['phases'] = {p: dict(count=b['phases'][p]['count']-a['phases'][p]['count'],
                                  us=b['phases'][p]['us']-a['phases'][p]['us'],
                                  us_per_submit=(b['phases'][p]['us']-a['phases'][p]['us'])/calls) for p in phases}
        for kind in ('scan', 'maintenance'):
            if kind in a and kind in b:
                delta[kind] = {key: [y-x for x, y in zip(a[kind][key], b[kind][key])] for key in ('pub', 'acq')}
        if b['display_ms'] > a['display_ms'] and b['flips'] >= a['flips']:
            delta['dpu_flip_rate'] = (b['flips']-a['flips'])*1000/(b['display_ms']-a['display_ms'])
        intervals.append(delta)
    return dict(samples=samples, intervals=intervals)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    raw = args.log.read_bytes()
    result = analyze(raw.decode(errors='replace'))
    result.update(log=str(args.log), sha256=hashlib.sha256(raw).hexdigest(),
                  warning='Elapsed time includes scheduling and blocking. Phases overlap; do not sum. DPU flips are not unique displayed frames.')
    args.output.write_text(json.dumps(result, indent=2)+chr(10))
    print(f'{len(result["samples"])} samples, {len(result["intervals"])} intervals -> {args.output}')
    for item in result['intervals']:
        if item['frame'] < 600:
            continue
        phases = ' '.join(f'{p}={item["phases"][p]["us_per_submit"]:.1f}' for p in ('lock', 'retire', 'audit', 'publish', 'switch', 'drain', 'ring', 'acquire', 'ibpeek') if p in item['phases'])
        print(f'line={item["to_line"]} f={item["frame"]} df={item["frame_delta"]} calls={item["calls"]} us/submit {phases}')


if __name__ == '__main__':
    main()
