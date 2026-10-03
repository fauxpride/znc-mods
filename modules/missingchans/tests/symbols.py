#!/usr/bin/env python3
"""Static symbol checks for a built missingchans.so. Python standard library
plus binutils (readelf, c++filt); ZNC does not need to be running.

Why these matter for in-place updates (see ../TESTING.md):

* An STB_GNU_UNIQUE symbol that the znc executable does not also define makes
  glibc mark the defining object NODELETE. That applies even to the private
  RTLD_LOCAL copies the r12 resident loader creates, so such a symbol would
  leak one mapping per update. It is the one pinning mechanism the loader
  cannot route around, so the build must not contain one.
* ZNC finds the module through ZNCModuleEntry; the resident loader finds a
  privately loaded r12+ build through MissingChansDirectEntry.
* Module-private classes and helpers must have internal linkage. Then they can
  never be bound to by another module, or by a different missingchans build
  loaded alongside, whatever visibility flags the compiler was given.

usage: symbols.py --module missingchans.so [--znc /path/to/znc]
"""
import argparse
import os
import shutil
import subprocess
import sys

PRIVATE_NAMES = ('CMissingChansMod', 'CStringCI', 'CRunTimer', 'CJoinAttemptTimer',
                 'CWhoisTimeout', 'WhoisOrigin', 'LoaderState', 'LoadedBuild',
                 'ResidentLoad', 'DirectLoad', 'LoadPrivate', 'ReleaseIdle')
ENTRY_POINTS = ('ZNCModuleEntry', 'MissingChansDirectEntry')


def dynamic_symbols(path):
    """Defined dynamic symbols as (name, type, binding, visibility)."""
    out = subprocess.run(['readelf', '-W', '--dyn-syms', path], capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(f'readelf failed for {path}: {out.stderr.strip()}')
    symbols = []
    for line in out.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[0].endswith(':') and fields[6] != 'UND':
            symbols.append((fields[7].split('@')[0], fields[3], fields[4], fields[5]))
    return symbols


def demangle(names):
    names = sorted(names)
    if not names:
        return []
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True)
    return out.stdout.splitlines() if out.returncode == 0 else names


def check_module(module, znc=None, report=print):
    """Returns a list of failure descriptions; empty means all checks passed."""
    failures = []
    mod = dynamic_symbols(module)
    exported = [s for s in mod if s[2] in ('GLOBAL', 'WEAK', 'UNIQUE') and s[3] in ('DEFAULT', 'PROTECTED')]

    unique = {s[0] for s in mod if s[2] == 'UNIQUE'}
    if znc:
        unique -= {s[0] for s in dynamic_symbols(znc) if s[2] == 'UNIQUE'}
    report(f'    info: {len(exported)} exported symbols; {len(unique)} unique symbol(s) the core lacks')
    if unique:
        failures.append('unique symbols that pin the object:\n      ' + '\n      '.join(demangle(unique)))

    for entry in ENTRY_POINTS:
        if not any(s[0] == entry and s[1] == 'FUNC' and s[2] == 'GLOBAL' for s in exported):
            failures.append(f'missing exported function {entry}')

    leaked = [name for name in demangle({s[0] for s in exported})
              if any(private in name for private in PRIVATE_NAMES)]
    if leaked:
        failures.append('module-private names are exported:\n      ' + '\n      '.join(leaked))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--module', required=True)
    parser.add_argument('--znc', help='znc executable; unique symbols it defines are harmless')
    args = parser.parse_args()
    for tool in ('readelf', 'c++filt'):
        if not shutil.which(tool):
            parser.error(f'{tool} (binutils) is required')
    for path in filter(None, (args.module, args.znc)):
        if not os.path.isfile(path):
            parser.error('file does not exist: ' + path)
    failures = check_module(args.module, args.znc)
    for failure in failures:
        print('FAIL ' + failure)
    print('PASS symbols' if not failures else f'FAIL symbols ({len(failures)} problem(s))')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
