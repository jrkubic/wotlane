#!/usr/bin/env python3
# ABOUTME: Tests for the lobby client (tools/net_lobby.py) against the real wotm_server (native/net/build, tools/build_net.bat).
# ABOUTME: Run: python tools/test_net_lobby.py
import re
import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import net_lobby  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
SERVER = REPO / 'native' / 'net' / 'build' / ('wotm_server.exe' if sys.platform == 'win32' else 'wotm_server')


class LobbyTest(unittest.TestCase):
    port = 7861

    @classmethod
    def setUpClass(cls):
        if not SERVER.exists():
            raise unittest.SkipTest(f'{SERVER} does not exist: run tools\\build_net.bat')

    def setUp(self):
        LobbyTest.port += 1
        self.server = subprocess.Popen([str(SERVER), '--port', str(self.port), '--players', '4'], stdout=subprocess.DEVNULL)
        time.sleep(0.3)

    def tearDown(self):
        self.server.kill()
        self.server.wait()

    def test_group_forms_a_match_with_one_env_per_player(self):
        envs = net_lobby.form_group('127.0.0.1', self.port, ['Ana', 'Bia', 'Cai'], [3, 1, 2], stage=5, build='b1', wins=3)
        self.assertEqual(len(envs), 3)
        a, b, c = envs
        self.assertEqual(a['WOTM_NET'], f'127.0.0.1:{self.port}')
        self.assertEqual(a['WOTM_MATCH'], '3,1,2,5')
        self.assertEqual(a['WOTM_PLAYERS'], '3')
        self.assertEqual(a['WOTM_FIXEDSTEP'], '2')     # 3+ players: the game's 30 Hz mode
        self.assertEqual(a['WOTM_SEED'], c['WOTM_SEED'])
        self.assertEqual([e['WOTM_KILLTARGET'] for e in envs], ['3', '3', '3'])
        self.assertEqual([e['WOTM_LOCALPLAYER'] for e in envs], ['1', '2', '3'])
        self.assertEqual([e['WOTM_NETNAME'] for e in envs], ['Ana', 'Bia', 'Cai'])
        self.assertEqual(len({e['WOTM_NETTOKEN'] for e in envs}), 3)

    def test_two_players_run_at_full_rate_and_endless_by_default(self):
        a, b = net_lobby.form_group('127.0.0.1', self.port, ['Ana', 'Bia'], [3, 1], stage=1, build='b1')
        self.assertEqual(a['WOTM_FIXEDSTEP'], '1')
        self.assertEqual(b['WOTM_MATCH'], '3,1,1')
        self.assertEqual(b['WOTM_KILLTARGET'], '0')
        self.assertEqual(b['WOTM_MODIFIERS'], '')       # always set: an unset list in a network match also means none

    def test_group_modifiers_reach_every_player_in_canonical_order(self):
        envs = net_lobby.form_group('127.0.0.1', self.port, ['Ana', 'Bia'], [3, 1], stage=1, build='b1', mods='turbo,jump')
        self.assertEqual([e['WOTM_MODIFIERS'] for e in envs], ['jump,turbo', 'jump,turbo'])
        self.assertEqual(envs[0]['WOTM_FIXEDSTEP'], '1')   # the runtime doubles the step itself during a turbo fight

    def test_unknown_modifier_is_refused(self):
        with net_lobby.Lobby('127.0.0.1', self.port, 'b1') as s:
            s.create('Sala', 'Ana', 3)
            with self.assertRaises(net_lobby.LobbyError):
                s.set(mods='moon')

    def test_list_shows_lobbies_with_code_host_and_lock(self):
        with net_lobby.Lobby('127.0.0.1', self.port, 'b1') as s, net_lobby.Lobby('127.0.0.1', self.port, 'b1') as t, \
                net_lobby.Lobby('127.0.0.1', self.port, 'b1') as watcher:
            self.assertEqual(watcher.list(), [])
            code = s.create('Sala', 'Ana', 4)
            locked = t.create('Fechada', 'Bia', 1, password='abc')
            lobbies = {l['code']: l for l in watcher.list()}
        self.assertEqual(len(code), 4)
        self.assertEqual(set(lobbies), {code, locked})
        self.assertEqual(lobbies[code]['state'], 'waiting')
        self.assertEqual(lobbies[code]['names'], ['Ana'])
        self.assertEqual(lobbies[code]['host'], 'Ana')
        self.assertEqual(lobbies[code]['name'], 'Sala')
        self.assertFalse(lobbies[code]['locked'])
        self.assertTrue(lobbies[locked]['locked'])

    def test_join_by_code_with_password_and_host_only_settings(self):
        with net_lobby.Lobby('127.0.0.1', self.port, 'b1') as a, net_lobby.Lobby('127.0.0.1', self.port, 'b1') as b:
            code = a.create('Sala', 'Ana', 3, password='pw')
            with self.assertRaisesRegex(net_lobby.LobbyError, 'password'):
                b.join(code, 'Bia', 1)
            b.join(code.lower(), 'Bia', 1, password='pw')
            with self.assertRaisesRegex(net_lobby.LobbyError, 'owner'):
                b.set(wins=2)
            self.assertEqual(a.set(stage=4, wins=2), {'stage': '4', 'wins': '2', 'mods': ''})

    def test_server_errors_raise(self):
        with net_lobby.Lobby('127.0.0.1', self.port, 'b1') as a, net_lobby.Lobby('127.0.0.1', self.port, 'other') as b:
            code = a.create('Sala', 'Ana', 3)
            with self.assertRaisesRegex(net_lobby.LobbyError, 'build'):
                b.join(code, 'Bia', 1)
            with self.assertRaisesRegex(net_lobby.LobbyError, 'no such lobby'):
                b.join('QQQQ' if code != 'QQQQ' else 'RRRR', 'Bia', 1)

    def test_cli_prints_env_lines(self):
        # the owner creates from the command line and starts when the second player joins by code
        tool = [sys.executable, str(REPO / 'tools' / 'net_lobby.py'), '--server', f'127.0.0.1:{self.port}', '--build', 'b1']
        first = subprocess.Popen(tool + ['--create', 'Sala', '--name', 'Ana', '--monster', '3', '--stage', '2', '--wins', '4', '--start-with', '2'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        code = ''
        deadline = time.time() + 10
        while not code and time.time() < deadline:
            line = first.stderr.readline()
            m = re.search(r'code ([A-Z0-9]{4})', line)
            code = m.group(1) if m else ''
        self.assertEqual(len(code), 4)
        second = subprocess.run(tool + ['--code', code, '--name', 'Bia', '--monster', '1'], capture_output=True, text=True, timeout=10)
        out1, _ = first.communicate(timeout=10)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn('WOTM_MATCH=3,1,2', second.stdout)
        self.assertIn('WOTM_KILLTARGET=4', second.stdout)
        self.assertIn('WOTM_LOCALPLAYER=2', second.stdout)
        self.assertIn('WOTM_LOCALPLAYER=1', out1)

    # match between two PCs: the second player joins (or the owner starts) after the Lobby's 10 s read timeout
    def test_host_waits_longer_than_the_read_timeout_for_players(self):
        tool = [sys.executable, str(REPO / 'tools' / 'net_lobby.py'), '--server', f'127.0.0.1:{self.port}', '--build', 'b1']
        first = subprocess.Popen(tool + ['--create', 'Sala', '--name', 'Ana', '--monster', '3', '--start-with', '2'],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        m = re.search(r'code ([A-Z0-9]{4})', first.stderr.readline())
        self.assertIsNotNone(m)
        time.sleep(12)
        second = subprocess.run(tool + ['--code', m.group(1), '--name', 'Bia', '--monster', '1'], capture_output=True, text=True, timeout=10)
        out1, err1 = first.communicate(timeout=10)
        self.assertEqual(first.returncode, 0, err1)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn('WOTM_LOCALPLAYER=1', out1)

    def test_joiner_waits_longer_than_the_read_timeout_for_the_start(self):
        tool = [sys.executable, str(REPO / 'tools' / 'net_lobby.py'), '--server', f'127.0.0.1:{self.port}', '--build', 'b1']
        with net_lobby.Lobby('127.0.0.1', self.port, 'b1') as host:
            code = host.create('Sala', 'Ana', 3)
            second = subprocess.Popen(tool + ['--code', code, '--name', 'Bia', '--monster', '1'],
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            host.wait_members(2)
            time.sleep(12)
            host.start()
            out2, err2 = second.communicate(timeout=10)
        self.assertEqual(second.returncode, 0, err2)
        self.assertIn('WOTM_LOCALPLAYER=2', out2)


class BrowseTest(unittest.TestCase):
    """The public list: a master (wotm_server --master) and servers started with --announce."""
    port = 7961

    @classmethod
    def setUpClass(cls):
        if not SERVER.exists():
            raise unittest.SkipTest(f'{SERVER} does not exist: run tools\\build_net.bat')

    def start(self, *args):
        p = subprocess.Popen([str(SERVER), *args], stdout=subprocess.DEVNULL)
        self.addCleanup(lambda: (p.kill(), p.wait()))
        return p

    def setUp(self):
        BrowseTest.port += 3
        self.master = self.port
        self.start('--master', '--port', str(self.master))
        time.sleep(0.3)

    def wait_rows(self, count):
        deadline = time.time() + 5
        while time.time() < deadline:
            rows = net_lobby.browse('127.0.0.1', self.master)
            if len(rows) == count:
                return rows
            time.sleep(0.1)
        self.fail(f'the master never listed {count} server(s)')

    def test_browse_lists_announced_servers_with_counts_and_ping(self):
        self.assertEqual(net_lobby.browse('127.0.0.1', self.master), [])
        self.start('--port', str(self.port + 1), '--announce', f'127.0.0.1:{self.master}', '--name', 'LanesPC', '--lobbies', '8')
        [row] = self.wait_rows(1)
        self.assertEqual({k: row[k] for k in ('ip', 'port', 'protocol', 'open', 'waiting', 'players', 'max', 'name')},
                         {'ip': '127.0.0.1', 'port': self.port + 1, 'protocol': net_lobby.PROTOCOL, 'open': 0, 'waiting': 0,
                          'players': 0, 'max': 8, 'name': 'LanesPC'})
        self.assertIsNotNone(row['ping'])   # the server answers on its port: reachable

    def test_cli_browse_prints_one_row_per_server(self):
        self.start('--port', str(self.port + 1), '--announce', f'127.0.0.1:{self.master}', '--name', 'Alpha')
        self.start('--port', str(self.port + 2), '--announce', f'127.0.0.1:{self.master}', '--name', 'Beta')
        self.wait_rows(2)
        out = subprocess.run([sys.executable, str(REPO / 'tools' / 'net_lobby.py'), '--browse', f'127.0.0.1:{self.master}'],
                             capture_output=True, text=True, timeout=10)
        self.assertEqual(out.returncode, 0, out.stderr)
        # the master lists servers in the order their announcements arrived: the two start together, so either may be first
        lines = sorted(out.stdout.strip().splitlines())
        self.assertEqual(len(lines), 2, out.stdout)
        self.assertRegex(lines[0], rf'^Alpha +127\.0\.0\.1:{self.port + 1} +0 lobbies \(0 waiting\) +0 players +ping \d+ ms$')
        self.assertRegex(lines[1], rf'^Beta +127\.0\.0\.1:{self.port + 2} ')

    def test_unreachable_and_other_version_servers_are_marked(self):
        with socket.create_connection(('127.0.0.1', self.master)) as s:   # a server that announced but no longer answers
            s.sendall(f'ANNOUNCE {net_lobby.PROTOCOL} 1 0 0 0 16 Gone\n'.encode())
            self.assertEqual(s.recv(64), b'OK\n')
        with socket.create_connection(('127.0.0.1', self.master)) as s:
            s.sendall(b'ANNOUNCE 99 2 0 0 0 16 Future\n')
            self.assertEqual(s.recv(64), b'OK\n')
        rows = {r['name']: r for r in self.wait_rows(2)}
        self.assertIsNone(rows['Gone']['ping'])
        self.assertEqual(net_lobby.row_text(rows['Gone']).split()[-1], 'unreachable')
        self.assertIn('needs another version', net_lobby.row_text(rows['Future']))


if __name__ == '__main__':
    unittest.main()
