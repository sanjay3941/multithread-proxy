"""Loopback integration tests for shared HTTP cache and request coalescing."""
import collections
import concurrent.futures
import email.utils
import os
import pathlib
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time


counts = collections.Counter()
counts_lock = threading.Lock()


class Origin(socketserver.BaseRequestHandler):
    def handle(self):
        data = b''
        while b'\r\n\r\n' not in data:
            part = self.request.recv(4096)
            if not part:
                return
            data += part
        first = data.split(b'\r\n', 1)[0].decode()
        path = first.split(' ')[1]
        with counts_lock:
            counts[path] += 1
            visit = counts[path]
        if path.startswith('/slow'):
            time.sleep(.5)
        if path.startswith('/echo'):
            self.request.sendall(b'HTTP/1.1 200 OK\r\nCache-Control: public, max-age=30\r\n'
                b'Content-Length: 2\r\nConnection: close\r\n\r\nOK')
            return
        body = b'x' * (1024 * 1024 + 1) if path.startswith('/large') else f'{path}:origin:{visit}'.encode()
        control = 'public, max-age=1' if path.startswith('/short') else 'public, max-age=30'
        if path.startswith('/private'):
            control = 'private, max-age=30'
        if path.startswith('/nostore'):
            control = 'public, no-store, max-age=30'
        headers = [
            'HTTP/1.1 200 OK',
            f'Cache-Control: {control}',
            f'Content-Length: {len(body)}',
            'Connection: close',
        ]
        if path.startswith('/vary'):
            headers.append('Vary: Accept-Language')
        if path.startswith('/setcookie'):
            headers.append('Set-Cookie: session=secret')
        if path.startswith('/old'):
            headers.append('Date: ' + email.utils.formatdate(time.time() - 3600, usegmt=True))
        if path.startswith('/date'):
            headers.append('Date: ' + email.utils.formatdate(time.time(), usegmt=True))
        self.request.sendall(('\r\n'.join(headers) + '\r\n\r\n').encode() + body)


class ThreadServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def free_port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def get(path, extra='', method='GET'):
    with socket.create_connection(('127.0.0.1', proxy_port), timeout=5) as client:
        client.settimeout(8)
        client.sendall((f'{method} http://127.0.0.1:{origin_port}{path} HTTP/1.1\r\n'
                        f'Host: 127.0.0.1:{origin_port}\r\n{extra}Connection: close\r\n\r\n').encode())
        data = b''
        while True:
            chunk = client.recv(65536)
            if not chunk:
                return data
            data += chunk


def body(response):
    assert response.startswith(b'HTTP/1.1 200'), response
    return response.split(b'\r\n\r\n', 1)[1]


def requests(path):
    with counts_lock:
        return counts[path]


def run(binary, simulation=False):
    global proxy_port
    proxy_port = free_port()
    with tempfile.TemporaryDirectory(prefix='proxy-cache-test-') as directory:
        work = pathlib.Path(directory)
        env = {k: v for k, v in os.environ.items() if not k.startswith("PROXY_")}
        env['PROXY_DETAILED_LOGS'] = '1'
        env['PROXY_DASHBOARD_PORT'] = '0'
        env['PROXY_BIND_ADDRESS'] = '127.0.0.1'
        if simulation:
            env['PROXY_CACHE_SIMULATE'] = '1'
        with (work / 'console.log').open('w') as output:
            proxy = subprocess.Popen([str(binary), str(proxy_port), '8', '16'], cwd=work,
                                     env=env, stdout=output, stderr=output)
            try:
                log = work / 'logs/proxy.log'
                for _ in range(100):
                    if proxy.poll() is not None:
                        raise RuntimeError('proxy exited during startup')
                    if log.exists() and 'PROXY_LISTENING' in log.read_text():
                        break
                    time.sleep(.02)
                else:
                    raise RuntimeError('proxy startup timed out')
                if simulation:
                    for _ in range(4):
                        get('/simulate')
                    assert requests('/simulate') == 4
                    assert 'CACHE=WOULD_STORE' in log.read_text()
                    print('PASS: simulation predicts eligible storage without serving it')
                    return

                first = [get('/hot') for _ in range(3)]
                assert len({body(r) for r in first}) == 3
                fourth = get('/hot')
                assert body(fourth) == body(first[-1])
                assert b'Age: ' in fourth
                assert requests('/hot') == 3
                print('PASS: popular response served locally with Age header')

                for _ in range(4):
                    get('/hot?different=1')
                assert requests('/hot?different=1') == 3
                print('PASS: cache key includes the query string')

                for path in ('/private', '/nostore', '/vary', '/setcookie', '/old'):
                    for _ in range(4):
                        get(path)
                    assert requests(path) == 4, (path, requests(path))
                print('PASS: private, no-store, varied, cookie, and stale responses bypass')

                for path, extra in (('/auth', 'Authorization: token\r\n'),
                                    ('/cookie', 'Cookie: user=a\r\n'),
                                    ('/range', 'Range: bytes=0-2\r\n'),
                                    ('/nocache', 'Cache-Control: no-cache\r\n')):
                    for _ in range(4):
                        get(path, extra)
                    assert requests(path) == 4, path
                print('PASS: personalized and bypass requests never enter cache')

                for _ in range(4):
                    get('/large')
                assert requests('/large') == 4
                print('PASS: oversized responses are not retained')

                for _ in range(3):
                    get('/mutate')
                assert requests('/mutate') == 3
                get('/mutate', method='POST')
                assert requests('/mutate') == 4
                get('/mutate')
                assert requests('/mutate') == 5
                print('PASS: an unsafe request invalidates the matching resource')

                for _ in range(3):
                    get('/short')
                assert requests('/short') == 3
                time.sleep(1.2)
                get('/short')
                assert requests('/short') == 4
                print('PASS: expired entry is not reused')

                for _ in range(2):
                    get('/slow')
                with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                    responses = list(pool.map(get, ['/slow'] * 4))
                assert requests('/slow') == 3, requests('/slow')
                assert len({body(r) for r in responses}) == 1
                assert 'CACHE=COALESCED' in log.read_text()
                print('PASS: concurrent popular misses share one origin request')

                for _ in range(4):
                    get('/date')
                assert requests('/date') == 3, (requests('/date'),
                    [line for line in log.read_text().splitlines() if 'CACHE=' in line][-12:])
                print('PASS: current Date response can be cached')
            finally:
                proxy.terminate()
                proxy.wait(timeout=5)


if __name__ == '__main__':
    binary = pathlib.Path(sys.argv[1]).resolve()
    with ThreadServer(('127.0.0.1', 0), Origin) as origin:
        origin_port = origin.server_address[1]
        thread = threading.Thread(target=origin.serve_forever, daemon=True)
        thread.start()
        try:
            run(binary)
            run(binary, simulation=True)
        finally:
            origin.shutdown()
            thread.join()
