#!/usr/bin/env python3
"""Run one kernel compile through ccache, writing clang's -MJ record itself.

  ci-ccache-clang.py REAL_CLANG ARGS...

XNU's compile rules pass -MJ FILE for the JSON compilation database, and ccache refuses every
command that carries it, so nothing was cached. Only IOPlatformExpert.cpo.json is read later (as the
compile recipe for board objects); that compile still runs clang itself with -MJ, so the record keeps
clang's exact form. Every other -MJ is dropped for ccache, and a minimal record in clang's layout
(one JSON object followed by ",\n") is written in its place.
"""
import json
import os
import sys

SOURCE_SUFFIXES = ('.c', '.cc', '.cpp', '.cxx', '.m', '.mm', '.s', '.S')
# Records read by scripts/build-arm-driver.py and scripts/build-picasso-pmgr.py.
KEEP_GENUINE = ('IOPlatformExpert.cpo.json',)
TAKES_VALUE = {'-o', '-MF', '-MT', '-MQ', '-MJ', '-include', '-imacros', '-isysroot', '-arch', '-x',
               '-I', '-D', '-U', '-iquote', '-isystem', '-idirafter', '-Xclang', '-target',
               '--serialize-diagnostics'}


def main():
    real, args = sys.argv[1], sys.argv[2:]
    if '-MJ' in args:
        at = args.index('-MJ')
        record_path = args[at + 1] if at + 1 < len(args) else None
        if record_path and os.path.basename(record_path) in KEEP_GENUINE:
            # The one record the board scripts read: clang writes it, uncached, exactly as before.
            os.execv(real, [real] + args)
        args = args[:at] + args[at + 2:]
        sources, output, skip = [], None, False
        for i, arg in enumerate(args):
            if skip:
                skip = False
                continue
            if arg in TAKES_VALUE:
                skip = True
                if arg == '-o' and i + 1 < len(args):
                    output = args[i + 1]
                continue
            if not arg.startswith('-') and arg.endswith(SOURCE_SUFFIXES):
                sources.append(arg)
        if record_path and len(sources) == 1:
            source = sources[0]
            if output is None:
                output = os.path.splitext(os.path.basename(source))[0] + '.o'
            record = {'directory': os.getcwd(), 'file': source, 'output': output,
                      'arguments': [real] + args}
            with open(record_path, 'w', encoding='utf-8') as out:
                out.write(json.dumps(record) + ',\n')
        elif record_path:
            # Not a single-source compile: keep clang's own -MJ and let ccache pass it through.
            os.execv(real, [real] + sys.argv[2:])
    os.execvp('ccache', ['ccache', real] + args)


if __name__ == '__main__':
    main()
