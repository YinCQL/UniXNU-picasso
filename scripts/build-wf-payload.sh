#!/usr/bin/env bash
# PR13 experiment entry point; GRID=0 omits the optional scanout sampler.
set -euo pipefail
exec python3 "$(dirname "$0")/build-wf-payload.py" "$@"
