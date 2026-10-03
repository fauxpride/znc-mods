#!/usr/bin/env python3
"""Diagnostic, not a pass/fail test: show whether `UpdateMod missingchans`
picks up a replaced binary for a given module load order, and which loaded
objects bound symbols into the resident missingchans.so (glibc LD_DEBUG).

usage:
  ZNC_BIN=/path/to/znc python3 probe_loadorder.py OLD.so NEW.so \\
      --global webadmin --network missingchans,perform,route_replies \\
      [--extra /path/to/othermodule.so ...]

Network modules are loaded in the order given. Names other than missingchans
are taken from --extra files (by basename) or from ZNC's own module directory.
Uses the same loopback IRC server and synthetic account as the suite.
"""
import argparse
import glob
import os
import shutil
import subprocess
import tempfile
from harness import IRCd, Client, listener, wait_for


def build_marker(output):
    for line in output.splitlines():
        if 'missingchans build: ' in line:
            return line.split('missingchans build: ', 1)[1].split()[0]
    return 'unknown'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('old')
    parser.add_argument('new')
    parser.add_argument('--global', dest='globals_', default='', help='comma-separated global modules')
    parser.add_argument('--network', default='missingchans,perform', help='comma-separated network modules')
    parser.add_argument('--extra', action='append', default=[], help='additional module .so to install')
    args = parser.parse_args()
    znc_bin = shutil.which(os.environ.get('ZNC_BIN', 'znc'))
    if not znc_bin:
        parser.error('ZNC_BIN must identify an installed ZNC executable')
    root = os.environ.get('MC_TEST_ROOT')
    if root:
        os.makedirs(root, exist_ok=True)
    path = tempfile.mkdtemp(prefix='probe-', dir=root)
    os.makedirs(path + '/configs')
    os.makedirs(path + '/modules')
    shutil.copyfile(args.old, path + '/modules/missingchans.so')
    for extra in args.extra:
        shutil.copyfile(extra, path + '/modules/' + os.path.basename(extra))
    server = IRCd(listener(), 'a')
    probe = listener()
    port = probe.getsockname()[1]
    probe.close()
    config = 'Version = 1.10.3\n' + ''.join(f'LoadModule = {m}\n' for m in args.globals_.split(',') if m)
    config += f'''<Listener l>
 Host = 127.0.0.1
 Port = {port}
 IPv4 = true
 IPv6 = false
 SSL = false
</Listener>
<User tester>
 Admin = true
 Nick = tester
 Ident = tester
 RealName = Test
 <Pass password>
  Method = plain
  Hash = test-password
 </Pass>
 <Network a>
  Server = 127.0.0.1 {server.port}
''' + ''.join(f'  LoadModule = {m}\n' for m in args.network.split(',') if m) + ''' </Network>
</User>
'''
    with open(path + '/configs/znc.conf', 'w') as f:
        f.write(config)
    identity = {}
    if os.geteuid() == 0:
        for p in [path, *glob.glob(path + '/**', recursive=True)]:
            os.chown(p, 65534, 65534)
        identity = dict(user=65534, group=65534, extra_groups=[])
    env = dict(os.environ, LD_DEBUG='bindings', LD_DEBUG_OUTPUT=path + '/lddebug')
    with open(path + '/znc.log', 'w') as log:
        proc = subprocess.Popen([znc_bin, '--foreground', '--debug', '--datadir', path],
                                stdout=log, stderr=subprocess.STDOUT, env=env, **identity)
        try:
            wait_for(server.ready.is_set, 15, 'ZNC IRC connection')
            wait_for(lambda: server.count('USER '), 10, 'IRC registration')
            client = Client(port, 'a')
            before = build_marker(client.command('VERSION'))
            staging = path + '/modules/missingchans.so.new'
            shutil.copyfile(args.new, staging)
            if identity:
                os.chown(staging, 65534, 65534)
            os.replace(staging, path + '/modules/missingchans.so')
            client.command('UpdateMod missingchans', 'status')
            after = build_marker(client.command('VERSION'))
            client.close()
        finally:
            proc.terminate()
            proc.wait(timeout=10)
            server.close()
    print(f'global={args.globals_ or "-"} network={args.network}')
    print(f'  before={before} after={after} -> ' + ('updated in place' if before != after else 'STALE: old code still running'))
    binders = {}
    for name in glob.glob(path + '/lddebug.*'):
        with open(name, errors='replace') as f:
            for line in f:
                if 'binding file ' not in line or ' to ' not in line:
                    continue
                source = line.split('binding file ', 1)[1].split(' ', 1)[0].rstrip(':')
                target = line.split(' to ', 1)[1].split()[0].rstrip(':')
                if target.endswith('/missingchans.so') and os.path.basename(source) != 'missingchans.so':
                    symbol = line.rsplit('symbol `', 1)[1].split("'", 1)[0]
                    binders.setdefault(os.path.basename(source), set()).add(symbol)
    for source, symbols in sorted(binders.items()):
        label = 'private load of the replacement' if source.startswith('.missingchans-load-') else source
        print(f'  {label} bound {len(symbols)} symbol(s) into the resident missingchans.so')
    if not binders:
        print('  no other module bound symbols into missingchans.so')
    print('  data: ' + path)


if __name__ == '__main__':
    main()
