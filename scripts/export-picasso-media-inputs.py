#!/usr/bin/env python3
"""Export the unchanged Data seed after the full media payload audit."""
import argparse
from pathlib import Path
from types import SimpleNamespace
import picasso_linux_inputs as inputs
import picasso_media_payload as media
import picasso_audio_payload as audio


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = inputs.load_script

    def load(name):
        module = original(name)
        if name != 'check-picasso-data-artifacts':
            return module
        return SimpleNamespace(payload_seed=module.payload_seed,
            check=lambda work, tree, output: audio.check(work, tree, output, profile=media))

    inputs.load_script = load
    try:
        inputs.export(args.tree.resolve(), args.work.resolve(), args.output.resolve())
    finally:
        inputs.load_script = original


if __name__ == '__main__':
    main()
