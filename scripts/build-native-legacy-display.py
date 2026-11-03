#!/usr/bin/env python3
"""Compatibility entry point: build the qualified iOS16 display and PR13 GOP handoff."""
from pathlib import Path
import runpy

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('build-picasso-display.py')), run_name='__main__')
