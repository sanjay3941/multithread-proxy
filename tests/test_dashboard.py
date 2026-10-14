"""Friendly dashboard routing and default worker-capacity checks, all offline."""
import http.client
import json
import os
import pathlib
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while data := self.request.recv(1024):
            self.request.sendall(data)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


def main(binary):
    with tempfile.TemporaryDirectory() as directory, Server(('127.0.0.1', 0), Echo) as echo:
        threading.Thread(target=echo.serve_forever, daemon=True).start()
        proxy_port, dashboard_port = port(), port()
        env = {k: v for k, v in os.environ.items() if not k.startswith('PROXY_')}
        env['PROXY_BIND_ADDRESS'] = '127.0.0.1'
        env['PROXY_DASHBOARD_PORT'] = str(dashboard_port)
        output = pathlib.Path(directory) / 'output.log'
        tunnels = []

        def api(path='/', fields=None, host='proxy.internal', extra=None):
            client = http.client.HTTPConnection('127.0.0.1', proxy_port, timeout=3)
            headers = {'Host': host}
            body = None
            if fields is not None:
                headers.update({'Content-Type': 'application/x-www-form-urlencoded',
                                'X-Proxy-Dashboard': '1', 'Origin': 'http://proxy.internal'})
                body = urllib.parse.urlencode(fields)
            headers.update(extra or {})
            try:
                client.request('POST' if fields is not None else 'GET', 'http://' + host + path, body, headers)
                response = client.getresponse()
                return response.status, response.read()
            finally:
                client.close()

        with output.open('w') as log:
            process = subprocess.Popen([str(binary), str(proxy_port)], cwd=directory, env=env, stdout=log, stderr=log)
            try:
                for _ in range(100):
                    assert process.poll() is None, output.read_text()
                    if 'PROXY_LISTENING' in output.read_text():
                        break
                    time.sleep(.03)
                assert 'WORKERS=16 QUEUE_CAPACITY=64' in output.read_text()
                assert 'ADDRESS=127.0.0.1:' in output.read_text()
                status, body = api()
                assert status == 200 and b'Proxy Server' in body and b'Quiet' not in body
                assert b'Dashboard key' not in body
                assert api('/app.js')[0] == 200 and api('/style.css')[0] == 200
                assert api(host='PROXY.INTERNAL')[0] == 200
                assert api('/missing')[0] == 404
                assert api('/api/status', extra={'Origin': 'https://untrusted.example'})[0] == 403
                assert api('/api/status', extra={'Sec-Fetch-Site': 'cross-site'})[0] == 403
                fields = dict(action='add', list='block', domain='example.test')
                assert api('/api/rules', fields)[0] == 200
                state = json.loads(api('/api/status')[1])
                assert 'example.test' in state['privacy']['blocklist']
                assert state['privacy']['originConnections'] == 0
                assert state['connections']['successful'] == 0, 'Dashboard requests polluted browsing totals'
                assert state['connections']['active'] == 0, 'Dashboard polling counted itself as browsing'
                assert api('/api/rules', dict(fields, action='remove'))[0] == 200
                assert api('/api/rules', fields, extra={'X-Proxy-Dashboard': ''})[0] == 403
                print('PASS: friendly domain, static assets, keyless edits, origin checks and separate traffic counters', flush=True)

                # More than the old four workers can remain occupied without stalling the dashboard.
                for _ in range(8):
                    sock = socket.create_connection(('127.0.0.1', proxy_port), timeout=3)
                    tunnels.append(sock)
                    target = f'127.0.0.1:{echo.server_address[1]}'
                    sock.sendall(f'CONNECT {target} HTTP/1.1\r\nHost: {target}\r\n\r\n'.encode())
                    assert sock.recv(4096).startswith(b'HTTP/1.1 200')
                    sock.sendall(b'echo')
                    assert sock.recv(4) == b'echo'
                assert api('/api/status')[0] == 200
                print('PASS: eight simultaneous tunnels plus dashboard with default worker pool', flush=True)
            finally:
                for sock in tunnels:
                    sock.close()
                process.terminate()
                try:
                    process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                echo.shutdown()


if __name__ == '__main__':
    main(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'build/cpp_proxy').resolve())
