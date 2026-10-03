#!/usr/bin/env python3
"""Run live-ZNC tests; see ../TESTING.md for prerequisites and usage."""
import argparse
import os
import shutil
import time
import traceback
from harness import ZNC, IRCd, listener, wait_for
import symbols

CURRENT = '+r12 '      # build marker of the module under test
VARIANT = '+r12-variant '  # marker of the --variant build (see TESTING.md)


def wait_attempt(z):
    wait_for(lambda: z.a.count('Scheduling join attempt'), description='scheduled retry')


def omitted_joined(module, baseline=False, debug=False):
    with ZNC(module, 'omitted-joined') as z:
        if debug:
            z.a.command('SET debug on')
        z.servers['a'].omitted.add('#gate')
        z.a.command('Add PRIVMSG audit :network-marker', 'perform')
        z.setup(retries=1)
        z.run()
        if baseline:
            wait_for(lambda: z.servers['a'].count('PRIVMSG audit :network-marker'), description='r9 false perform')
        else:
            wait_for(lambda: z.a.count('All expected channels'), description='joined despite WHOIS omission')
            time.sleep(1.4)
            assert z.servers['a'].count('PRIVMSG audit :network-marker') == 0
            assert z.servers['a'].join_count('#gate') == 1


def late_join(module, baseline=False, debug=False):
    with ZNC(module, 'late-join', denied=('#gate',)) as z:
        if debug:
            z.a.command('SET debug on')
        z.a.command('Add PRIVMSG audit :network-marker', 'perform')
        z.setup(retrystep=2, retries=1, stopperformon='#gate')
        z.run()
        wait_attempt(z)
        z.servers['a'].join('#gate')
        z.servers['a'].sync()
        if baseline:
            wait_for(lambda: z.servers['a'].count('PRIVMSG audit :network-marker'), description='r9 stale retry')
        else:
            wait_for(lambda: z.a.count('No missing channels remain'), description='cancelled stale retry')
            assert z.servers['a'].count('PRIVMSG audit :network-marker') == 0
            assert z.servers['a'].join_count('#gate') == 1


def user_perform(module, baseline=False, debug=False):
    with ZNC(module, 'user-perform', denied=('#gate',), network_perform=False, user_perform=True) as z:
        if debug:
            z.a.command('SET debug on')
        z.a.command('Add PRIVMSG audit :user-marker %network%', 'perform')
        z.setup(retries=1)
        z.run()
        wait_for(lambda: z.a.count('perform commands sent'), description='perform confirmation')
        z.b.sync()
        if baseline:
            assert z.b.count('perform commands sent') == 1
            assert z.servers['a'].count('PRIVMSG audit :user-marker') == 0
        else:
            assert z.b.count('perform commands sent') == 0, z.b.texts()
            wait_for(lambda: z.servers['a'].count('PRIVMSG audit :user-marker a') == 1, description='correct network context')
            assert z.servers['b'].count('PRIVMSG audit :user-marker') == 0
            # Context must have been restored: a subsequent invocation from b works there.
            z.b.command('Execute', 'perform')
            wait_for(lambda: z.servers['b'].count('PRIVMSG audit :user-marker b') == 1, description='context restoration')


TESTS = dict(omitted_joined=omitted_joined, late_join=late_join, user_perform=user_perform)


def sentinel_suppression(module, baseline=False):
    with ZNC(module, 'sentinel', channels=('#gate','#other'), denied=('#other',)) as z:
        z.a.command('Add PRIVMSG audit :marker', 'perform')
        z.setup(stopperformon='#gate', retries=1)
        z.run()
        wait_for(lambda: z.servers['a'].join_count('#other') == 2, description='missing channel join')
        assert z.servers['a'].count('PRIVMSG audit :marker') == 0
        assert z.servers['a'].join_count('#gate') == 1
        status = z.a.command('STATUS')
        assert 'yes' in status and 'VerifiedJoined(count)' in status
        assert 'suppressed' in '\n'.join(z.a.texts())


def network_preference(module, baseline=False):
    with ZNC(module, 'network-preference', denied=('#gate',), user_perform=True) as z:
        z.b.command('Add PRIVMSG audit :user-marker', 'perform')
        z.a.command('Add PRIVMSG audit :network-marker', 'perform')
        z.setup(retries=1)
        z.run()
        wait_for(lambda: z.servers['a'].count('PRIVMSG audit :network-marker') == 1)
        assert z.servers['a'].count('PRIVMSG audit :user-marker') == 0
        assert z.b.count('perform commands sent') == 0
        status = z.a.command('STATUS')
        z.a.command('SET debug on')
        status = z.a.command('STATUS')
        assert 'LastPerformSource' in status and 'network' in status and 'UTC' in status


def successful_recovery(module, baseline=False):
    with ZNC(module, 'recovery', denied=('#gate',), chan_options={'#gate':'   Key = test-key\n'}) as z:
        irc = z.servers['a']
        irc.on_line = lambda line: irc.denied.clear() if line == 'PRIVMSG audit :unlock' else None
        z.a.command('Add PRIVMSG audit :unlock', 'perform')
        z.setup()
        z.run()
        wait_for(lambda: '#gate' in irc.joined, description='bot-gated recovery')
        wait_for(lambda: z.a.count('All expected channels'), description='post-join verification')
        assert irc.count('PRIVMSG audit :unlock') == 1
        assert irc.count('JOIN #gate test-key') == 2
        assert z.b.count('perform commands sent') == 0


def pending_settings(module, baseline=False):
    for key, value, expected in [('joinmissing','off',0), ('retryperform','off',1), ('retries','1',0)]:
        with ZNC(module, 'pending-'+key, denied=('#gate',)) as z:
            z.a.command('Add PRIVMSG audit :marker', 'perform')
            z.setup(retrystep=2, retries=2)
            z.run()
            wait_attempt(z)
            if key == 'retries':
                wait_for(lambda: z.a.count('Scheduling join attempt 2/2'), description='second pending retry')
            before_join = z.servers['a'].join_count('#gate')
            before_perf = z.servers['a'].count('PRIVMSG audit :marker')
            z.a.command(f'SET {key} {value}')
            if key == 'retryperform':
                wait_for(lambda: z.servers['a'].join_count('#gate') > before_join)
            else:
                wait_for(lambda: z.a.count('Pending retry cancelled'), description='settings cancel pending retry')
            assert z.servers['a'].join_count('#gate') == before_join + expected
            assert z.servers['a'].count('PRIVMSG audit :marker') == before_perf


def numeric443(module, baseline=False):
    for nick, expected_calls in [('somebodyelse',1), ('tester',0)]:
        with ZNC(module, 'numeric443-'+nick, denied=('#gate',)) as z:
            z.a.command('Add PRIVMSG audit :marker', 'perform')
            z.setup(retrystep=2, retries=1, stopperformon='#gate')
            z.run()
            wait_attempt(z)
            z.servers['a'].numeric('443', f'{nick} #gate :is already on channel')
            z.servers['a'].sync()
            if expected_calls:
                wait_for(lambda: z.servers['a'].count('PRIVMSG audit :marker') == 1)
            else:
                wait_for(lambda: z.a.count('No missing channels remain'))
            assert z.servers['a'].count('PRIVMSG audit :marker') == expected_calls


def prefixes(module, baseline=False):
    chans = ('#plain','#voice','#op','#owner','#admin','#compound','#special')
    with ZNC(module, 'prefixes', channels=chans, denied=chans) as z:
        # Local IsOn is false: success must come from the WHOIS parser itself.
        z.servers['a'].whois_override = ['#PLAIN','+#voice','@#op','~#owner','&#admin','~&@%+#compound','!#special']
        z.setup(retries=1)
        z.run()
        wait_for(lambda: z.a.count('All expected channels'))
        assert z.a.count('Scheduling join attempt') == 0
        assert z.a.count(' 319 ') == 0
        assert z.a.count('perform commands sent') == 0


def whois_clients(module, baseline=False):
    for routing in ((), ('route_replies',)):
        with ZNC(module, 'whois-clients', extra_modules=routing) as z:
            z.a.send('WHOIS tester')
            wait_for(lambda: z.a.count(' 318 ') == 1)
            z.run()
            wait_for(lambda: z.a.count('All expected channels'))
            # Internal WHOIS is hidden. User WHOIS remains visible afterwards.
            assert z.a.count(' 318 ') == 1
            z.a.send('WHOIS tester')
            wait_for(lambda: z.a.count(' 318 ') == 2)
            assert z.a.count(' 319 ') == 2


def whois_multi_error(module, baseline=False):
    with ZNC(module, 'whois-multi') as z:
        z.a.send('WHOIS absent,tester')
        wait_for(lambda: z.a.count(' 318 ') == 2)
        assert z.a.count(' 401 ') == 1
        z.run()
        wait_for(lambda: z.a.count('All expected channels'))
        assert z.a.count(' 318 ') == 2


def whois_401_only(module, baseline=False):
    with ZNC(module, 'whois-401-only') as z:
        irc = z.servers['a']
        irc.whois_end_on_error = False
        z.a.send('WHOIS absent')
        wait_for(lambda: z.a.count(' 401 ') == 1)
        z.run()
        wait_for(lambda: z.a.count('All expected channels') == 1)
        irc.whois_error = True
        z.a.command('RUN')
        wait_for(lambda: z.a.count('WHOIS failed') == 1)
        irc.whois_error = False
        z.a.command('RUN')
        wait_for(lambda: z.a.count('All expected channels') == 2)
        assert z.a.count('Scheduling join attempt') == 0


def whois_overlap(module, baseline=False):
    with ZNC(module, 'whois-overlap') as z:
        server = z.servers['a']
        server.hold_whois = True
        z.run()
        z.a.send('WHOIS tester')
        wait_for(lambda: len(server.pending_whois) == 2)
        server.release_whois()
        wait_for(lambda: z.a.count(' 318 ') == 1)
        assert z.a.count(' 319 ') == 1
        assert z.a.count('All expected channels') == 1


def whois_failure(module, baseline=False):
    with ZNC(module, 'whois-failure', denied=('#gate',)) as z:
        z.setup()
        z.servers['a'].whois_error = True
        z.run()
        wait_for(lambda: z.a.count('WHOIS failed'))
        assert z.a.count('Scheduling join attempt') == 0
        z.servers['a'].whois_error = False
        z.a.command('RUN')
        wait_attempt(z)


def unrelated401(module, baseline=False):
    with ZNC(module, 'unrelated401') as z:
        server = z.servers['a']
        server.hold_whois = True
        z.run()
        wait_for(lambda: len(server.pending_whois) == 1)
        server.numeric('401', 'nobody :No such nick')
        server.release_whois()
        wait_for(lambda: z.a.count('All expected channels'))
        assert z.a.count(' 401 ') == 1


def whois_timeout(module, baseline=False):
    with ZNC(module, 'whois-timeout', denied=('#gate',)) as z:
        z.a.command('Add PRIVMSG audit :marker', 'perform')
        z.setup()
        server = z.servers['a']
        server.hold_whois = True
        z.run()
        wait_for(lambda: z.a.count('WHOIS timed out'), timeout=35)
        assert server.count('PRIVMSG audit :marker') == 0
        assert server.join_count('#gate') == 1
        assert 'Previous WHOIS is incomplete' in z.a.command('RUN')
        assert server.count('WHOIS tester') == 1
        server.release_whois()
        server.sync()
        # The abandoned reply must not trigger repair, and a new run can proceed.
        assert z.a.count('Scheduling join attempt') == 0
        z.a.command('RUN')
        wait_attempt(z)


def manual_restart(module, baseline=False):
    with ZNC(module, 'manual-restart', denied=('#gate',)) as z:
        z.setup(retrystep=2, retries=1)
        z.run()
        wait_attempt(z)
        z.a.command('RUN')
        wait_for(lambda: z.a.count('Scheduling join attempt') == 2)
        wait_for(lambda: z.servers['a'].join_count('#gate') == 2)
        time.sleep(1)
        assert z.servers['a'].join_count('#gate') == 2


def busy_run(module, baseline=False):
    with ZNC(module, 'busy-run') as z:
        z.servers['a'].hold_whois = True
        z.run()
        assert 'Verification already in progress' in z.a.command('RUN')
        assert z.servers['a'].count('WHOIS tester') == 1
        z.servers['a'].release_whois()
        wait_for(lambda: z.a.count('All expected channels'))


def departures(module, baseline=False):
    for event in ('part','kick'):
        with ZNC(module, 'departures-'+event, channels=('#gate','#other'), denied=('#other',)) as z:
            z.a.command('Add PRIVMSG audit :marker', 'perform')
            z.setup(retrystep=2, retries=1)
            z.run()
            wait_attempt(z)
            getattr(z.servers['a'], event)('#gate')
            z.servers['a'].sync()
            wait_for(lambda: z.servers['a'].join_count('#other') == 2)
            # ZNC removes a self-PARTed channel; a KICK leaves a disabled entry.
            assert z.servers['a'].join_count('#gate') == (1 if event == 'part' else 2)
            assert z.servers['a'].count('PRIVMSG audit :marker') == 1


def unrelated_departures(module, baseline=False):
    with ZNC(module, 'other-kick', channels=('#gate','#other'), denied=('#other',)) as z:
        z.setup(retrystep=2, retries=1, stopperformon='#gate')
        z.run()
        wait_attempt(z)
        z.servers['a'].kick('#gate', 'other')
        z.servers['a'].send(':other!u@host.test PART #gate :Leaving')
        z.servers['a'].sync()
        wait_for(lambda: z.servers['a'].join_count('#other') == 2)
        assert z.servers['a'].join_count('#gate') == 1
        assert z.a.count('perform commands sent') == 0


def settings_persistence(module, baseline=False):
    with ZNC(module, 'persistence') as z:
        z.setup(delay=123, retries=7, retrystep=9, expectedmode='enabled', stopperformon='#gate')
        before = z.a.command('STATUS')
        assert '123s' in before and '9s' in before
        out = z.a.command('UnloadMod missingchans', 'status')
        assert 'unloaded' in out.lower(), out
        out = z.a.command('LoadMod missingchans', 'status')
        assert 'loaded' in out.lower(), out
        after = z.a.command('STATUS')
        for value in ('123s','9s','enabled','#gate','ON'):
            assert value in after, after
        assert CURRENT in z.a.command('VERSION')


def reload_pending(module, baseline=False):
    with ZNC(module, 'reload-pending', denied=('#gate',)) as z:
        z.setup(retrystep=2, retries=1)
        z.run()
        wait_attempt(z)
        out = z.a.command('UpdateMod missingchans', 'status')
        assert 'reload' in out.lower(), out
        time.sleep(2.5)
        assert z.servers['a'].join_count('#gate') == 1
        z.a.command('RUN')
        wait_for(lambda: z.servers['a'].join_count('#gate') == 2)


def expected_modes(module, baseline=False):
    with ZNC(module, 'expected-modes', channels=('#gate','#disabled'),
             chan_options={'#disabled':'   Disabled = true\n'}) as z:
        z.servers['a'].join('#ephemeral')
        z.servers['a'].sync()
        for mode, wanted, absent in [('all',('#gate','#disabled','#ephemeral'),()),
                                     ('config',('#gate','#disabled'),('#ephemeral',)),
                                     ('enabled',('#gate',),('#disabled','#ephemeral'))]:
            z.a.command('SET expectedmode '+mode)
            out = z.a.command('SHOW')
            assert all(chan in out for chan in wanted), out
            assert all(chan not in out for chan in absent), out


def no_perform(module, baseline=False):
    with ZNC(module, 'no-perform', denied=('#gate',), network_perform=False) as z:
        z.setup(retries=1)
        z.run()
        wait_for(lambda: z.servers['a'].join_count('#gate') == 2)
        assert z.a.count('perform module is not loaded') == 1
        assert z.a.count('perform commands sent') == 0


def bounded_backoff(module, baseline=False):
    with ZNC(module, 'bounded-backoff', denied=('#gate',)) as z:
        z.setup(retries=3, retrystep=1)
        z.run()
        wait_for(lambda: z.a.count('Reached maximum join attempts'), timeout=20)
        times = [stamp for stamp, line in z.servers['a'].lines if line.startswith('JOIN #gate')]
        assert len(times) == 4, times
        # A 3-second post-JOIN check precedes waits of 2 and 3 seconds.
        assert 4.5 <= times[2]-times[1] < 7, times
        assert 5.5 <= times[3]-times[2] < 8, times
        assert z.a.count('perform commands sent') == 3


def zero_step(module, baseline=False):
    with ZNC(module, 'zero-step', denied=('#gate',)) as z:
        z.setup(retries=1, retrystep=0)
        z.run()
        wait_for(lambda: z.servers['a'].join_count('#gate') == 2)
        assert z.a.count('in 1s (RetryStep=0)') == 1


def diagnostic_readonly(module, baseline=False):
    with ZNC(module, 'readonly', denied=('#gate',)) as z:
        z.setup(retrystep=2, retries=1)
        z.run()
        wait_attempt(z)
        for command in ('STATUS','SHOW','VERSION','HELP'):
            z.a.command(command)
        assert z.servers['a'].count('WHOIS tester') == 1
        wait_for(lambda: z.servers['a'].join_count('#gate') == 2)
        assert z.a.count('Scheduling join attempt') == 1


def command_surface(module, baseline=False):
    with ZNC(module, 'commands') as z:
        for command, fragment in [('SET','Usage:'), ('SET delay 0','Delay must'),
                                  ('SET retries 0','Retries must'), ('SET nonsense 1','Unknown SET'),
                                  ('nonsense','Unknown command'), ('SET expectedmode cfg',"'config'"),
                                  ('SET expectedmode autojoin',"'enabled'"), ('SET expectedmode invalid',"'all'"),
                                  ('SET stopperformon -','OFF'), ('SET joinmissing yes','ON'),
                                  ('SET retryperform true','ON')]:
            assert fragment in z.a.command(command), command
        for command in ('HELP','h','?'):
            out = z.a.command(command)
            for key in ('delay','joinmissing','expectedmode','retryperform','retries','retrystep','stopperformon','debug'):
                assert key in out, (command,key)


def disconnect_reconnect(module, baseline=False):
    with ZNC(module, 'disconnect-reconnect', denied=('#gate',)) as z:
        z.setup(delay=1, retries=1, retrystep=2)
        z.run()
        wait_attempt(z)
        old = z.servers['a']
        z.a.command('Disconnect', 'status')
        wait_for(lambda: old.count('QUIT '))
        old.close()
        assert 'Not connected' in z.a.command('RUN')
        (z.path/'irc-a-before-reconnect.log').write_text('\n'.join(old.texts()))
        new = IRCd(listener(old.port), 'a', ('#gate',))
        z.servers['a'] = new
        z.a.command('Connect', 'status')
        wait_for(new.ready.is_set)
        wait_for(lambda: new.join_count('#gate') == 2, description='new connection recovery')
        wait_for(lambda: z.a.count('Reached maximum join attempts'), timeout=10)
        assert new.join_count('#gate') == 2
        assert old.join_count('#gate') == 1


def defaults(module, baseline=False):
    with ZNC(module, 'defaults', denied=('#gate',)) as z:
        status = z.a.command('STATUS')
        for fragment in ('300s', '20s', 'OFF', 'all'):
            assert fragment in status, status
        z.run()
        wait_for(lambda: z.a.count('JoinMissing is OFF'))
        assert z.servers['a'].join_count('#gate') == 1
        assert z.a.count('Scheduling join attempt') == 0


def differential_commands(new_module, old_module):
    commands = ['STATUS', 'SET delay 0', 'SET delay 123', 'SET retries 0',
                'SET retries 8', 'SET retrystep 12', 'SET joinmissing on',
                'SET retryperform yes', 'SET expectedmode cfg',
                'SET expectedmode autojoin', 'SET expectedmode all',
                'SET expectedmode invalid', 'SET stopperformon #gate',
                'SET stopperformon none', 'SET stopperformon off',
                'SET stopperformon -', 'SET stopperformon',
                'SET nonsense 1', 'unknown', 'SHOW', 'STATUS']
    captures = []
    for label, module in [('baseline',old_module), ('fixed',new_module)]:
        with ZNC(module, 'differential-'+label) as z:
            rows = []
            for command in commands:
                rows.append('\n'.join(line.split(' :',1)[1] for line in z.a.command(command).splitlines()
                                      if line.startswith(':*missingchans!') and ' PRIVMSG ' in line))
            captures.append(rows)
    for command, old, new in zip(commands, *captures):
        assert old == new, (command, old, new)
    print(f'PASS differential_commands ({len(commands)} byte-identical command responses)', flush=True)


def upgrade_from(new_module, old_module, revision, variant=None):
    with ZNC(old_module, 'upgrade-'+revision, denied=('#gate',)) as z:
        z.setup(delay=123, retries=7, retrystep=3, stopperformon='#gate')
        z.run()
        wait_attempt(z)
        z.install(new_module)
        out = z.a.command('UpdateMod missingchans', 'status')
        assert 'reload' in out.lower(), out
        version = z.a.command('VERSION')
        restart_used = '+'+revision+' ' in version
        if restart_used:
            # Expected for pre-r12 binaries in this load order: the old code is
            # what stays resident, and it has no in-place loader.
            print('  '+revision+' remained resident after UpdateMod; testing full restart fallback', flush=True)
            z.restart()
            version = z.a.command('VERSION')
        assert CURRENT in version, version
        assert CURRENT in z.b.command('VERSION')
        status = z.a.command('STATUS')
        assert list(status_rows(status)) == LEGACY_FIELDS, status
        for fragment in ('123s','3s','#gate','ON'):
            assert fragment in status, status
        time.sleep(3.5)
        assert z.servers['a'].join_count('#gate') == 1
        z.a.command('RUN')
        wait_for(lambda: z.servers['a'].join_count('#gate') == 2)
        inplace = ''
        if variant:
            # Once r12 is the resident loader, the next update needs no restart.
            z.install(variant)
            assert 'reload' in z.a.command('UpdateMod missingchans', 'status').lower()
            for client in (z.a, z.b):
                assert VARIANT in client.command('VERSION')
            inplace = '; next update in place'
    print(f'PASS upgrade_from_{revision} (both networks, saved settings, pending timer; restart={restart_used}{inplace})', flush=True)


LEGACY_FIELDS = ['Delay', 'JoinMissing', 'ExpectedMode', 'RetryPerform', 'Retries',
                 'RetryStep', 'StopPerformOn', 'PerformSuppressed(this run)',
                 'VerifiedJoined(count)']
DEBUG_FIELDS = ['Debug', 'Network', 'Phase', 'Attempt', 'LiveJoined(count)',
                'WhoisJoined(last snapshot)', 'Missing(last check)', 'LastAction',
                'LastAttemptMissing', 'LastAttemptTriggeredPerform',
                'PerformCalls(this connection)', 'LastPerformSource', 'LastPerformAt']


def status_rows(output):
    rows = {}
    for line in output.splitlines():
        if not line.startswith(':*missingchans!') or ' PRIVMSG ' not in line:
            continue
        payload = line.split(' :', 1)[1]
        if payload.startswith('|'):
            cells = [cell.strip() for cell in payload.split('|')[1:-1]]
            if len(cells) == 2 and cells[0] != 'Setting':
                rows[cells[0]] = cells[1]
    return rows


def debug_status(module, baseline=False):
    with ZNC(module, 'debug-status') as z:
        legacy = status_rows(z.a.command('STATUS'))
        assert list(legacy) == LEGACY_FIELDS, legacy
        # Same boolean semantics as the existing JoinMissing/RetryPerform settings.
        for value in ('on', '1', 'yes', 'true', 'enable', 'enabled', 'ON'):
            assert 'Debug is now ON.' in z.a.command('SET DeBuG '+value)
            rows = status_rows(z.a.command('STATUS'))
            assert list(rows) == LEGACY_FIELDS + DEBUG_FIELDS, rows
            assert rows['Debug'] == 'ON' and rows['Network'] == 'a'
            assert rows['PerformCalls(this connection)'] == '0'
            assert {key: rows[key] for key in LEGACY_FIELDS} == legacy
            assert list(status_rows(z.b.command('STATUS'))) == LEGACY_FIELDS
        for value in ('off', '0', 'no', 'false', 'disabled', 'OFF', '', 'invalid'):
            assert 'Debug is now OFF.' in z.a.command('SET debug '+value)
            assert status_rows(z.a.command('STATUS')) == legacy
        assert 'debug' in z.a.command('SET')
        assert z.servers['a'].count('WHOIS ') == 0
        assert z.servers['a'].join_count('#gate') == 1
        assert z.a.count('perform commands sent') == 0


def debug_persistence(module, baseline=False):
    with ZNC(module, 'debug-persistence') as z:
        z.setup(delay=123, retries=7, retrystep=9, stopperformon='#gate')
        for value in ('on', 'off'):
            z.a.command('SET debug '+value)
            expected = LEGACY_FIELDS + (DEBUG_FIELDS if value == 'on' else [])
            before = status_rows(z.a.command('STATUS'))
            assert 'reload' in z.a.command('UpdateMod missingchans', 'status').lower()
            assert list(status_rows(z.a.command('STATUS'))) == expected
            assert list(status_rows(z.b.command('STATUS'))) == LEGACY_FIELDS
            z.restart()
            after = status_rows(z.a.command('STATUS'))
            assert list(after) == expected, after
            # Suppression and 443 evidence are volatile, not saved settings.
            assert {key: after[key] for key in LEGACY_FIELDS[:7]} == {key: before[key] for key in LEGACY_FIELDS[:7]}
            assert list(status_rows(z.b.command('STATUS'))) == LEGACY_FIELDS
            assert z.servers['a'].count('WHOIS ') == 0


def debug_pending_recovery(module, baseline=False):
    for initial in ('off', 'on'):
        with ZNC(module, 'debug-pending-'+initial, denied=('#gate',)) as z:
            z.a.command('Add PRIVMSG audit :debug-marker', 'perform')
            z.setup(retries=1, retrystep=2, debug=initial)
            z.run()
            wait_attempt(z)
            for value in ('on', 'off', initial):
                z.a.command('SET debug '+value)
                fields = list(status_rows(z.a.command('STATUS')))
                assert fields == LEGACY_FIELDS + (DEBUG_FIELDS if value == 'on' else [])
            wait_for(lambda: z.a.count('Reached maximum join attempts'), timeout=10)
            assert z.servers['a'].join_count('#gate') == 2
            assert z.servers['a'].count('PRIVMSG audit :debug-marker') == 1
            assert z.a.count('Scheduling join attempt') == 1
            assert z.b.count('perform commands sent') == 0
            # Hidden diagnostics are still recorded; revealing them cannot replay commands.
            z.a.command('SET debug on')
            before = status_rows(z.a.command('STATUS'))
            assert before['PerformCalls(this connection)'] == '1', before
            assert before['LastAttemptMissing'] == '#gate'
            assert before['LastAttemptTriggeredPerform'] == 'yes'
            assert 'network' in before['LastPerformSource'] and 'UTC' in before['LastPerformAt']
            def traffic():
                return [line for line in z.servers['a'].texts() if not line.startswith(('PING ', 'PONG '))]
            before_traffic = traffic()
            z.a.command('SET debug off')
            assert list(status_rows(z.a.command('STATUS'))) == LEGACY_FIELDS
            z.a.command('SET debug on')
            assert status_rows(z.a.command('STATUS')) == before
            # Exclude the harness's own PING/PONG barriers from upstream traffic.
            assert traffic() == before_traffic


def debug_recovery_guards(module, baseline=False):
    for scenario in (omitted_joined, late_join, user_perform):
        scenario(module, debug=True)


def differential_r10_status(new_module, old_module):
    captures = []
    for label, module in [('r10', old_module), ('new', new_module)]:
        with ZNC(module, 'status-'+label) as z:
            if label == 'new':
                z.a.command('SET debug on')
            z.setup(delay=123, retries=7, retrystep=9, stopperformon='#gate')
            rows = status_rows(z.a.command('STATUS'))
            if label == 'new':
                assert rows.pop('Debug') == 'ON'
            captures.append(rows)
    assert captures[0] == captures[1], captures
    print('PASS differential_r10_status (all 21 r10 rows preserved with Debug ON)', flush=True)


def differential_r11(new_module, old_module):
    """r12 changes only VERSION (an added Loader line). Everything else that
    r11 printed, including both STATUS layouts, must be byte-identical."""
    commands = ['HELP', 'STATUS', 'SET debug on', 'STATUS', 'SET delay 77', 'SET retrystep 0',
                'SET stopperformon #gate', 'STATUS', 'SET debug off', 'STATUS', 'SET', 'SHOW',
                'SET nonsense 1', 'unknown']
    captures, versions = [], []
    for label, module in [('r11', old_module), ('r12', new_module)]:
        with ZNC(module, 'differential-r11-'+label) as z:
            captures.append([module_lines(z.a.command(command)) for command in commands])
            versions.append(module_lines(z.a.command('VERSION')).splitlines())
    for capture, marker in zip(captures, ('+r11 ', CURRENT)):
        # HELP names the build; that line is the only intended difference.
        lines = capture[0].splitlines()
        assert lines[1].startswith('Build: ') and marker in lines[1], lines[1]
        capture[0] = '\n'.join(lines[:1] + lines[2:])
    for command, old, new in zip(commands, *captures):
        assert old == new, (command, old, new)
    assert len(versions[0]) == 1 and versions[0][0].startswith('missingchans build: ') and '+r11 ' in versions[0][0]
    assert len(versions[1]) == 2 and versions[1][0].startswith('missingchans build: ') and CURRENT in versions[1][0]
    assert versions[1][1].startswith('Loader: resident loader '), versions[1]
    print(f'PASS differential_r11 ({len(commands)} responses identical apart from the build name; VERSION adds a Loader line)', flush=True)


def module_lines(output):
    return '\n'.join(line.split(' :', 1)[1] for line in output.splitlines()
                     if line.startswith(':*missingchans!') and ' PRIVMSG ' in line)


def loader_line(output):
    for line in module_lines(output).splitlines():
        if line.startswith('Loader: '):
            return line[len('Loader: '):]
    return ''


def update(z):
    out = z.a.command('UpdateMod missingchans', 'status')
    assert 'reload' in out.lower(), out


def expect_build(z, marker, loader_fragment):
    for client in (z.a, z.b):
        out = client.command('VERSION')
        assert marker in out, out
        assert loader_fragment in loader_line(out), out


def inplace_update(module, variant):
    """Worst-case load order for earlier revisions: missingchans is loaded
    first and perform binds symbols into it. Updates must still take effect."""
    with ZNC(module, 'inplace-update', denied=('#gate',)) as z:
        z.a.command('SET debug on')
        z.setup(delay=123, retries=7, retrystep=3, stopperformon='#gate')
        before = status_rows(z.a.command('STATUS'))
        expect_build(z, CURRENT, 'resident loader')
        assert len(z.mapped_builds()) == 1, z.mapped_builds()
        z.run()
        wait_attempt(z)
        builds = [variant, module] * 3
        for build in builds:
            z.install(build)
            update(z)
            expect_build(z, VARIANT if build == variant else CURRENT, 'updated in place by resident loader')
            after = status_rows(z.a.command('STATUS'))
            assert {k: after[k] for k in LEGACY_FIELDS[:7]} == {k: before[k] for k in LEGACY_FIELDS[:7]}, after
            assert after['Debug'] == 'ON', after
            assert list(status_rows(z.b.command('STATUS'))) == LEGACY_FIELDS
            # Only the resident loader and the running build remain mapped.
            assert len(z.mapped_builds()) == 2, z.mapped_builds()
            assert z.leftover_links() == [], z.leftover_links()
        # The retry pending before the first update died with that instance.
        time.sleep(3.5)
        assert z.servers['a'].join_count('#gate') == 1
        z.a.command('RUN')
        wait_for(lambda: z.servers['a'].join_count('#gate') == 2, description='retry from updated build')
    print(f'PASS inplace_update ({len(builds)} consecutive updates on both networks; 2 builds mapped)', flush=True)


def inplace_load_order(module, variant):
    """missingchans loaded after perform/route_replies, so nothing pins it."""
    with ZNC(module, 'inplace-order', network_perform=False, first_modules=('perform', 'route_replies')) as z:
        for build, marker in ((variant, VARIANT), (module, CURRENT)):
            z.install(build)
            update(z)
            expect_build(z, marker, 'updated in place by resident loader')
            assert len(z.mapped_builds()) == 2, z.mapped_builds()
    print('PASS inplace_load_order (missingchans after perform/route_replies)', flush=True)


def inplace_reloadmod(module, variant):
    with ZNC(module, 'inplace-reloadmod') as z:
        z.install(variant)
        out = z.a.command('ReloadMod missingchans', 'status')
        assert 'reload' in out.lower(), out
        assert VARIANT in z.a.command('VERSION')
        out = z.b.command('VERSION')
        assert CURRENT in out and 'resident loader' in loader_line(out), out
        assert len(z.mapped_builds()) == 2, z.mapped_builds()
        z.install(module)  # same code as the resident build, but a new file
        update(z)
        expect_build(z, CURRENT, 'updated in place by resident loader')
        assert len(z.mapped_builds()) == 2, z.mapped_builds()
    print('PASS inplace_reloadmod (per-network reload picks up the installed file)', flush=True)


def inplace_unload_load(module, variant):
    with ZNC(module, 'inplace-unload-load') as z:
        for client in (z.a, z.b):
            assert 'unloaded' in client.command('UnloadMod missingchans', 'status').lower()
        z.install(variant)
        for client in (z.a, z.b):
            out = client.command('LoadMod missingchans', 'status')
            assert 'loaded' in out.lower(), out
            assert VARIANT in client.command('VERSION')
        assert len(z.mapped_builds()) == 2, z.mapped_builds()
        out = z.a.command('ListAvailMods', 'status')
        assert 'missingchans' in out, out
        assert VARIANT in z.a.command('VERSION')
    print('PASS inplace_unload_load (UnloadMod/LoadMod and ListAvailMods)', flush=True)


def inplace_bad_file(module, variant, mismatch=None):
    with ZNC(module, 'inplace-bad-file') as z:
        z.install(variant)
        update(z)
        expect_build(z, VARIANT, 'updated in place')
        bad = z.path/'not-a-module.so'
        bad.write_bytes(b'not an ELF file\n')
        broken = [(bad, 'cannot load')]
        if mismatch:
            broken.append((mismatch, 'is built for ZNC'))
        for path, reason in broken:
            start = len(z.a.texts())
            z.install(path)
            update(z)
            # Both networks keep running the build that ran before, and say so.
            expect_build(z, VARIANT, 'WARNING: the installed missingchans.so was not loaded')
            assert reason in z.a.command('VERSION'), reason
            assert any('WARNING: the installed missingchans.so' in line for line in z.a.texts()[start:])
            assert list(status_rows(z.a.command('STATUS'))) == LEGACY_FIELDS
            assert z.leftover_links() == [], z.leftover_links()
        z.install(module)
        update(z)
        expect_build(z, CURRENT, 'updated in place by resident loader')
        assert 'WARNING' not in z.a.command('VERSION')
        assert len(z.mapped_builds()) == 2, z.mapped_builds()
    print(f'PASS inplace_bad_file ({len(broken)} unusable replacement(s) rejected; previous build kept)', flush=True)


def inplace_link_denied(module, variant):
    with ZNC(module, 'inplace-link-denied') as z:
        z.install(variant)
        moddir = z.path/'modules'
        os.chmod(moddir, 0o555)
        try:
            update(z)
            out = z.a.command('VERSION')
            assert CURRENT in out and 'cannot create a temporary link' in out, out
            assert CURRENT in z.b.command('VERSION')
        finally:
            os.chmod(moddir, 0o755)
        update(z)
        expect_build(z, VARIANT, 'updated in place by resident loader')
    print('PASS inplace_link_denied (unwritable module directory reported; retry succeeds)', flush=True)


def inplace_downgrade(module, r11, variant):
    with ZNC(module, 'inplace-downgrade') as z:
        z.setup(delay=123, retries=7, retrystep=3, stopperformon='#gate')
        before = status_rows(z.a.command('STATUS'))
        z.install(r11)
        update(z)
        for client in (z.a, z.b):
            out = client.command('VERSION')
            assert '+r11 ' in out and loader_line(out) == '', out
        assert status_rows(z.a.command('STATUS')) == before
        z.install(variant)
        update(z)
        expect_build(z, VARIANT, 'updated in place by resident loader')
        # A pre-r12 build cannot report its instances, so it stays mapped.
        assert len(z.mapped_builds()) == 3, z.mapped_builds()
    print('PASS inplace_downgrade (r12 loader runs r11, then updates again)', flush=True)


EXTRA = [sentinel_suppression, network_preference, successful_recovery, pending_settings,
         numeric443, prefixes, whois_clients, whois_multi_error, whois_401_only, whois_overlap,
         whois_failure, unrelated401, whois_timeout, manual_restart, busy_run,
         departures, unrelated_departures, settings_persistence, reload_pending,
         expected_modes, no_perform, bounded_backoff, zero_step, diagnostic_readonly,
         command_surface, disconnect_reconnect, defaults, debug_status, debug_persistence,
         debug_pending_recovery, debug_recovery_guards]
TESTS.update({test.__name__: test for test in EXTRA})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', required=True, help='absolute path to missingchans.so')
    parser.add_argument('--compare-r9', help='also compare unchanged command responses against this r9 .so')
    parser.add_argument('--compare-r10', help='also compare debug status and upgrade from this r10 .so')
    parser.add_argument('--compare-r11', help='also compare responses with, upgrade from, and downgrade to this r11 .so')
    parser.add_argument('--variant', help='same source built with the +r12-variant marker; enables in-place update tests')
    parser.add_argument('--mismatch', help='variant built for a different ZNC version string; extends inplace_bad_file')
    parser.add_argument('--baseline', action='store_true', help='assert r9 bug reproductions instead of fixed behavior')
    parser.add_argument('tests', nargs='*')
    args = parser.parse_args()
    module = os.path.abspath(args.module)
    chosen = args.tests or (['omitted_joined','late_join','user_perform'] if args.baseline else list(TESTS))
    if not os.path.isfile(module):
        parser.error('module file does not exist: ' + module)
    if not shutil.which(os.environ.get('ZNC_BIN', 'znc')):
        parser.error('ZNC_BIN must identify an installed ZNC executable')
    for option in ('compare_r9', 'compare_r10', 'compare_r11', 'variant', 'mismatch'):
        value = getattr(args, option)
        if value and not os.path.isfile(value):
            parser.error(option.replace('_', '-') + ' file does not exist: ' + value)
        if value:
            setattr(args, option, os.path.abspath(value))
    if args.mismatch and not args.variant:
        parser.error('--mismatch requires --variant')
    unknown = set(chosen) - set(TESTS)
    if unknown:
        parser.error('unknown tests: ' + ', '.join(sorted(unknown)))
    failed = 0
    for name in chosen:
        start = time.monotonic()
        try:
            TESTS[name](module, args.baseline)
            print(f'PASS {name} ({time.monotonic()-start:.2f}s)', flush=True)
        except Exception:
            failed += 1
            print(f'FAIL {name}', flush=True)
            traceback.print_exc()
    checks = []
    if not args.baseline:
        def symbol_check():
            znc = shutil.which(os.environ.get('ZNC_BIN', 'znc'))
            built = [module] + ([args.variant] if args.variant else [])
            problems = [p for path in built for p in symbols.check_module(path, znc)]
            assert not problems, '\n'.join(problems)
            print(f'PASS symbols ({len(built)} build(s): no pinning unique symbols, entry points present, private names internal)', flush=True)
        checks.append(('symbols', symbol_check))
    if args.compare_r9:
        old9 = args.compare_r9
        checks += [('differential_commands', lambda: differential_commands(module, old9)),
                   ('upgrade_from_r9', lambda: upgrade_from(module, old9, 'r9', args.variant))]
    if args.compare_r10:
        old10 = args.compare_r10
        checks += [('differential_r10_status', lambda: differential_r10_status(module, old10)),
                   ('upgrade_from_r10', lambda: upgrade_from(module, old10, 'r10', args.variant))]
    if args.compare_r11:
        old11 = args.compare_r11
        checks += [('differential_r11', lambda: differential_r11(module, old11)),
                   ('upgrade_from_r11', lambda: upgrade_from(module, old11, 'r11', args.variant))]
    if args.variant:
        variant = args.variant
        checks += [('inplace_update', lambda: inplace_update(module, variant)),
                   ('inplace_load_order', lambda: inplace_load_order(module, variant)),
                   ('inplace_reloadmod', lambda: inplace_reloadmod(module, variant)),
                   ('inplace_unload_load', lambda: inplace_unload_load(module, variant)),
                   ('inplace_bad_file', lambda: inplace_bad_file(module, variant, args.mismatch)),
                   ('inplace_link_denied', lambda: inplace_link_denied(module, variant))]
        if args.compare_r11:
            checks.append(('inplace_downgrade', lambda: inplace_downgrade(module, args.compare_r11, variant)))
    for name, check in checks:
        start = time.monotonic()
        try:
            check()
        except Exception:
            failed += 1
            print('FAIL ' + name, flush=True)
            traceback.print_exc()
        chosen.append(name)
    print(f'{len(chosen)-failed}/{len(chosen)} passed', flush=True)
    return bool(failed)


if __name__ == '__main__':
    raise SystemExit(main())
