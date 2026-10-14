"""Offline privacy/dashboard integration tests. No root or internet required."""
import http.client
import json
import os
import pathlib
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse



def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def exchange(port, request):
    with socket.create_connection(('127.0.0.1', port), 4) as s:
        s.sendall(request.encode())
        result = b''
        while True:
            part = s.recv(65536)
            if not part:
                return result
            result += part


class Origin(socketserver.BaseRequestHandler):
    def handle(self):
        data = b''
        while b'\r\n\r\n' not in data:
            part = self.request.recv(4096)
            if not part:
                return
            data += part
        self.server.received.append(data)
        self.request.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n'
                            b'Cache-Control: public, max-age=60\r\nConnection: close\r\n\r\nOK')


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main(binary):
    with tempfile.TemporaryDirectory(prefix='proxy-privacy-test-') as directory, Server(('127.0.0.1', 0), Origin) as origin:
        work = pathlib.Path(directory)
        origin.received = []
        threading.Thread(target=origin.serve_forever, daemon=True).start()
        origin_port = origin.server_address[1]
        proxy_port, admin_port = free_port(), free_port()
        env = {k: v for k, v in os.environ.items() if not k.startswith('PROXY_')}
        env['PROXY_BIND_ADDRESS'] = '127.0.0.1'
        env.update(PROXY_DASHBOARD_PORT=str(admin_port))
        output = (work / 'console.txt').open('w')
        process = None

        def api(path='/api/status', fields=None, headers=None):
            h = {'X-Proxy-Dashboard': '1'}
            if headers:
                h.update(headers)
            body = urllib.parse.urlencode(fields) if fields is not None else None
            if body is not None:
                h['Content-Type'] = 'application/x-www-form-urlencoded'
            c = http.client.HTTPConnection('127.0.0.1', admin_port, timeout=5)
            try:
                c.request('POST' if fields is not None else 'GET', path, body, h)
                r = c.getresponse()
                return r.status, r.read()
            finally:
                c.close()

        def state():
            status, data = api()
            assert status == 200, data
            return json.loads(data)

        def rule(domain, action='add', kind='block', expected=200):
            status, data = api('/api/rules', dict(action=action, list=kind, domain=domain))
            assert status == expected, (status, data)

        def request(host, method='CONNECT', extra='', path='/private-marker'):
            target = f'{host}:{origin_port}' if method == 'CONNECT' else f'http://{host}:{origin_port}{path}'
            # Blocked CONNECT tests return immediately; use HTTP for permitted requests.
            return exchange(proxy_port, f'{method} {target} HTTP/1.1\r\nHost: {host}:{origin_port}\r\n{extra}\r\n')

        def start():
            nonlocal process
            process = subprocess.Popen([str(binary), str(proxy_port), '4', '16'], cwd=work,
                                       env=env, stdout=output, stderr=output)
            for _ in range(100):
                assert process.poll() is None, 'Proxy failed to start'
                try:
                    state()
                    if 'PROXY_LISTENING' in (work / 'console.txt').read_text():
                        return
                except OSError:
                    pass
                time.sleep(.03)
            raise AssertionError('Dashboard did not start')

        def stop(sig=signal.SIGTERM):
            process.send_signal(sig)
            process.wait(timeout=6)
            assert process.returncode == 0, process.returncode

        try:
            start()
            assert state()['privacy']['connectMs'] is None
            assert api()[0] == 200
            assert api('/api/rules', dict(action='add', list='block', domain='bad.example'), headers={'X-Proxy-Dashboard': ''})[0] == 403
            assert api(headers={'Origin': 'https://evil.example'})[0] == 403
            assert api(headers={'Host': f'evil.example:{admin_port}'})[0] == 403
            assert api('/')[0] == 200 and api('/app.js')[0] == 200
            for invalid in ['https://example.com', '*.example.com', '127.0.0.1', '<script>.com', 'a..com', 'x.com:80', 'proxy.internal']:
                rule(invalid, expected=400)
            oversized = exchange(admin_port, f'POST /api/rules HTTP/1.1\r\nHost: 127.0.0.1:{admin_port}\r\n'
                f'X-Proxy-Dashboard: 1\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 5000\r\n\r\n')
            assert oversized.startswith(b'HTTP/1.1 400')
            print('PASS: keyless dashboard, origin/host protection and input limits', flush=True)

            assert request('Sub.DoubleClick.NET.').startswith(b'HTTP/1.1 403')
            assert request('google-analytics.com', 'GET').startswith(b'HTTP/1.1 403')
            rule(' Private.Invalid. ')
            assert request('child.private.invalid').startswith(b'HTTP/1.1 403')
            assert state()['privacy']['blocklist'] == ['private.invalid']
            # .invalid is reserved and fails locally without reaching an origin.
            rule('boundary.invalid')
            assert not request('notboundary.invalid', 'GET').startswith(b'HTTP/1.1 403')
            rule('boundary.invalid', action='remove')
            rule('localtest.me')  # Rules persist independently of domain resolution.
            rule('private.invalid', kind='allow')
            assert not request('private.invalid', 'GET').startswith(b'HTTP/1.1 403')
            rule('private.invalid', action='remove', kind='allow')
            status, _ = api('/api/settings', dict(category='ads', enabled='0'))
            assert status == 200 and not state()['privacy']['adsEnabled']
            print('PASS: category blocking, normalization, label boundaries, allowlist and toggles', flush=True)

            # localhost is explicitly accepted for local policy testing.
            local = 'localhost'
            resolved = {i[4][0] for i in socket.getaddrinfo(local, origin_port)}
            assert '127.0.0.1' in resolved, 'Test requires localhost -> loopback'
            for _ in range(4):
                assert request(local, 'GET').endswith(b'OK')
            assert len(origin.received) == 3
            assert state()['privacy']['cacheHits'] == 1
            rule(local)
            assert request(local, 'GET').startswith(b'HTTP/1.1 403')
            assert len(origin.received) == 3
            rule(local, kind='allow')
            assert request(local, 'GET').endswith(b'OK')
            rule(local, action='remove', kind='allow')
            rule(local, action='remove')
            assert request(local, 'GET', extra='Forwarded: secret\r\nX-Forwarded-For: secret\r\n'
                'X-Real-IP: secret\r\nProxy-Authorization: secret\r\nConnection: x-private\r\n'
                'X-Private: secret\r\n', path='/headers').endswith(b'OK')
            assert b'secret' not in origin.received[-1]
            assert request(local, 'GET', extra='Connection: content-length\r\nContent-Length: 0\r\n').startswith(b'HTTP/1.1 400')
            assert request(local, 'GET', extra='Content-Length: 0\r\nContent-Length: 0\r\n').startswith(b'HTTP/1.1 400')
            assert request(local, 'GET', extra='Transfer-Encoding: chunked\r\n').startswith(b'HTTP/1.1 400')
            print('PASS: policy precedes cached responses; identifying headers and unsafe framing rejected', flush=True)

            # A failed save must not activate an unsaved policy.
            os.chmod(work / 'data', 0o500)
            try:
                rule('unsaved.example', expected=500)
                assert 'unsaved.example' not in state()['privacy']['blocklist']
            finally:
                os.chmod(work / 'data', 0o700)
            s = state()
            assert s['privacy']['adsBlocked'] == 1
            assert s['privacy']['trackersBlocked'] == 1
            assert s['privacy']['customBlocked'] == 2
            assert s['privacy']['originConnections'] == 4
            assert s['privacy']['bytesSaved'] > 0
            stop(signal.SIGINT)
            logs = (work / 'console.txt').read_text()
            assert 'private-marker' not in logs and 'TARGET=localhost' not in logs
            assert 'CLIENT=127.0.0.1' not in logs
            assert not (work / 'logs/proxy.log').exists()
            start()
            assert not state()['privacy']['adsEnabled']
            assert 'private.invalid' in state()['privacy']['blocklist']
            assert state()['privacy']['customBlocked'] == 0
            assert request('private.invalid').startswith(b'HTTP/1.1 403')
            stop()
            print('PASS: atomic persistence, counters, private default logs, restart and both shutdown signals', flush=True)
        finally:
            if process and process.poll() is None:
                process.kill()
                process.wait()
            output.close()
            origin.shutdown()


if __name__ == '__main__':
    main(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'build/cpp_proxy').resolve())
