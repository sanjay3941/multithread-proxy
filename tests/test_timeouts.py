"""Loopback regression tests. Usage: python3 tests/test_timeouts.py build/cpp_proxy"""
import concurrent.futures
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO = pathlib.Path(__file__).resolve().parents[1]
CONFIG = dict((name, int(value)) for name, value in re.findall(
    r'constexpr int (\w+)\s*=\s*(\d+)',
    (REPO / 'include/proxy/timeouts.hpp').read_text()))
HEADER = CONFIG['CLIENT_HEADER_TIMEOUT']
CONNECT = CONFIG['UPSTREAM_CONNECT_TIMEOUT']
READ = CONFIG['UPSTREAM_READ_TIMEOUT']
IDLE = CONFIG['HTTPS_TUNNEL_IDLE_TIMEOUT']
WRITE = CONFIG['SOCKET_WRITE_TIMEOUT']


def listen(backlog=8):
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    sock.listen(backlog)
    sock.settimeout(10)
    return sock


def serve(handler):
    listener = listen()
    port = listener.getsockname()[1]

    def run():
        try:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(max(IDLE, READ, WRITE * 3) + 20)
                handler(peer)
        except OSError:
            pass  # Disconnects are expected in timeout fixtures.
        finally:
            listener.close()

    threading.Thread(target=run, daemon=True).start()
    return port


def read_headers(sock):
    data = b''
    while b'\r\n\r\n' not in data:
        # Leave any tunnel payload after the header terminator for the caller.
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
    return data


def read_all(sock):
    data = b''
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return data
        data += chunk


def client(small_buffer=False):
    sock = socket.socket()
    if small_buffer:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    sock.settimeout(max(IDLE, READ, WRITE * 3) + 20)
    sock.connect(('127.0.0.1', proxy_port))
    return sock


def request(sock, port):
    sock.sendall(('GET http://127.0.0.1:%d/ HTTP/1.1\r\nHost: localhost\r\n\r\n' % port).encode())


def tunnel(sock, port):
    sock.sendall(('CONNECT 127.0.0.1:%d HTTP/1.1\r\nHost: localhost\r\n\r\n' % port).encode())
    assert b'200 Connection Established' in read_headers(sock)


def elapsed_since(start, expected):
    elapsed = time.monotonic() - start
    assert expected - 1 <= elapsed <= expected + 8, (elapsed, expected)


def logged_failure(client_port, message, wait=3):
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        log = (work / 'logs/proxy.log').read_text()
        match = re.search(r'\[(REQ-\d+)\] CLIENT=127\.0\.0\.1:%d STATUS=ACCEPTED' % client_port, log)
        if match:
            lines = [line for line in log.splitlines() if '[' + match[1] + ']' in line]
            if any('STATUS=FAILED' in line and message in line for line in lines):
                assert not any('STATUS=COMPLETED' in line for line in lines)
                return
        time.sleep(.1)
    raise AssertionError('Missing failure log: ' + message)


def http_ok():
    def handler(peer):
        read_headers(peer)
        peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK')
    with client() as sock:
        request(sock, serve(handler))
        assert read_all(sock).endswith(b'\r\n\r\nOK')


def tunnel_echo():
    def handler(peer):
        peer.sendall(peer.recv(4096))
    with client() as sock:
        tunnel(sock, serve(handler))
        sock.sendall(b'tunnel-test')
        assert read_all(sock) == b'tunnel-test'


def headers_timeout(trickle=False):
    with client() as sock:
        port = sock.getsockname()[1]
        start = time.monotonic()
        sock.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nX-Slow: ')
        if trickle:
            for _ in range(3):
                time.sleep(HEADER / 4)
                sock.sendall(b'x')
        assert read_all(sock).startswith(b'HTTP/1.1 408 ')
        elapsed_since(start, HEADER)
    logged_failure(port, 'CLIENT_TIMEOUT')


def connect_timeout():
    # Saturating a loopback accept queue stalls another TCP handshake.
    with listen(0) as listener:
        port = listener.getsockname()[1]
        with socket.create_connection(('127.0.0.1', port)), client() as sock:
            start = time.monotonic()
            request(sock, port)
            assert read_all(sock).startswith(b'HTTP/1.1 504 ')
            elapsed_since(start, CONNECT)


def http_timeout(partial=False):
    prefix = b'HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nx'
    def handler(peer):
        read_headers(peer)
        if partial:
            peer.sendall(prefix)
        while peer.recv(4096):
            pass
    with client() as sock:
        port = sock.getsockname()[1]
        start = time.monotonic()
        request(sock, serve(handler))
        response = read_all(sock)
        assert response == prefix if partial else response.startswith(b'HTTP/1.1 504 ')
        elapsed_since(start, READ)
    logged_failure(port, 'upstream read timed out')


def tunnel_idle():
    with client() as sock:
        port = sock.getsockname()[1]
        tunnel(sock, serve(read_all))
        start = time.monotonic()
        assert read_all(sock) == b''  # Never inject an HTTP error into the tunnel.
        elapsed_since(start, IDLE)
    logged_failure(port, 'tunnel idle timeout')


def tunnel_active(upload=False):
    duration = IDLE + 5
    received = []
    finished = threading.Event()
    def handler(peer):
        try:
            if upload:
                received.append(len(read_all(peer)))
            else:
                for _ in range(duration):
                    peer.sendall(b'x')
                    time.sleep(1)
        finally:
            finished.set()
    with client() as sock:
        tunnel(sock, serve(handler))
        if upload:
            for _ in range(duration):
                sock.sendall(b'x')
                time.sleep(1)
        else:
            assert len(read_all(sock)) == duration
    assert finished.wait(5)
    if upload:
        assert received == [duration], received


def blocked_write():
    def handler(peer):
        read_headers(peer)
        peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 1073741824\r\n\r\n')
        while True:
            peer.sendall(b'x' * 65536)
    with client(small_buffer=True) as sock:
        port = sock.getsockname()[1]
        request(sock, serve(handler))
        # Do not read. Allow extra time for send-buffer filling and partial sends.
        logged_failure(port, 'socket send timed out', wait=WRITE * 3 + 10)


if __name__ == '__main__':
    binary = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'build/cpp_proxy').resolve()
    work = pathlib.Path(tempfile.mkdtemp(prefix='proxy-timeout-tests-'))
    with listen() as probe:
        proxy_port = probe.getsockname()[1]
    tests = {
        'HTTP forwarding': http_ok,
        'CONNECT echo': tunnel_echo,
        'Idle headers return 408': headers_timeout,
        'Trickled headers enforce deadline': lambda: headers_timeout(True),
        'Connect timeout returns 504': connect_timeout,
        'HTTP read timeout returns 504': http_timeout,
        'Partial response closes without injected error': lambda: http_timeout(True),
        'Idle tunnel closes': tunnel_idle,
        'Active one-way download survives': tunnel_active,
        'Active one-way upload survives': lambda: tunnel_active(True),
        'Blocked client write times out': blocked_write,
    }
    if len(sys.argv) > 2:
        tests = {name: test for name, test in tests.items() if sys.argv[2].lower() in name.lower()}
        if not tests:
            raise SystemExit('No tests matched the supplied filter')
    failures = []
    print('Logs: ' + str(work), flush=True)
    with (work / 'console.log').open('w') as output:
        # Every case runs concurrently and some intentionally occupy a worker
        # for a full timeout. Give each case its own worker so queueing does
        # not distort the timing assertions.
        proxy = subprocess.Popen([str(binary), str(proxy_port), str(len(tests))],
                                 cwd=work, env={**{k: v for k, v in os.environ.items() if not k.startswith("PROXY_")}, "PROXY_DETAILED_LOGS": "1", "PROXY_DASHBOARD_PORT": "0", "PROXY_BIND_ADDRESS": "127.0.0.1"}, stdout=output, stderr=output)
        try:
            for _ in range(50):
                if proxy.poll() is not None:
                    raise RuntimeError('Proxy exited during startup')
                log = work / 'logs/proxy.log'
                if log.exists() and 'PROXY_LISTENING' in log.read_text():
                    break
                time.sleep(.1)
            else:
                raise RuntimeError('Proxy failed to start')
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(tests)) as pool:
                futures = {pool.submit(test): name for name, test in tests.items()}
                for future in concurrent.futures.as_completed(futures):
                    name = futures[future]
                    try:
                        future.result()
                        print('PASS: ' + name, flush=True)
                    except Exception as error:
                        failures.append(name)
                        print('FAIL: ' + name + ': ' + repr(error), flush=True)
            assert proxy.poll() is None, 'Proxy crashed'
        finally:
            proxy.terminate()
            try:
                proxy.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proxy.kill()
                proxy.wait()
    print('%d/%d checks passed' % (len(tests) - len(failures), len(tests)), flush=True)
    sys.exit(bool(failures))
