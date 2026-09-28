"""Loopback check for queue rejection, recovery, and worker reuse."""
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time


def listen():
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    sock.listen(8)
    return sock


def read_all(sock):
    data = b''
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            return data
        data += chunk


def wait_for(log, text):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if log.exists() and text in log.read_text():
            return
        time.sleep(.02)
    raise AssertionError('Missing log entry: ' + text)


def worker_thread(log, port):
    lines = log.read_text().splitlines()
    accepted = next(line for line in lines if f'CLIENT=127.0.0.1:{port} STATUS=ACCEPTED' in line)
    request_id = re.search(r'\[(REQ-\d+)\]', accepted).group(1)
    handled = next(line for line in lines if f'[{request_id}]' in line and 'STATUS=REQUEST_RECEIVED' in line)
    return re.search(r'THREAD=(\d+)', handled).group(1)


def get(sock, upstream):
    sock.sendall(f'GET http://127.0.0.1:{upstream}/ HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
    assert read_all(sock).endswith(b'\r\n\r\nOK')


def main(binary):
    with tempfile.TemporaryDirectory(prefix='proxy-pool-test-') as directory:
        work = pathlib.Path(directory)
        log = work / 'logs/proxy.log'
        upstream_listener = listen()
        upstream = upstream_listener.getsockname()[1]

        def upstream_server():
            for _ in range(3):
                peer, _ = upstream_listener.accept()
                with peer:
                    peer.settimeout(5)
                    while b'\r\n\r\n' not in peer.recv(4096):
                        pass
                    peer.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK')

        server = threading.Thread(target=upstream_server, daemon=True)
        server.start()
        with listen() as probe:
            proxy_port = probe.getsockname()[1]
        with (work / 'console.log').open('w') as output:
            proc = subprocess.Popen([str(binary), str(proxy_port), '1', '1'], cwd=work,
                                    stdout=output, stderr=output)
            clients = []
            try:
                wait_for(log, 'PROXY_LISTENING')

                def connect():
                    sock = socket.create_connection(('127.0.0.1', proxy_port), 5)
                    sock.settimeout(5)
                    clients.append(sock)
                    return sock

                first = connect()
                first_port = first.getsockname()[1]
                first.sendall(f'GET http://127.0.0.1:{upstream}/ HTTP/1.1\r\nHost: '.encode())
                wait_for(log, f'CLIENT=127.0.0.1:{first_port} STATUS=ACCEPTED')
                # The worker has dequeued the first socket before the next is queued.
                time.sleep(.1)
                second = connect()
                second_port = second.getsockname()[1]
                second.sendall(f'GET http://127.0.0.1:{upstream}/ HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
                wait_for(log, f'CLIENT=127.0.0.1:{second_port} STATUS=ACCEPTED')
                third = connect()
                third_port = third.getsockname()[1]
                wait_for(log, f'CLIENT=127.0.0.1:{third_port} STATUS=REJECTED REASON=QUEUE_FULL')
                try:
                    assert third.recv(1) == b''
                except ConnectionResetError:
                    pass  # The close can be reported as a reset after accept.

                first.sendall(b'localhost\r\n\r\n')
                assert read_all(first).endswith(b'\r\n\r\nOK')
                assert read_all(second).endswith(b'\r\n\r\nOK')

                fourth = connect()
                fourth_port = fourth.getsockname()[1]
                get(fourth, upstream)
                threads = {worker_thread(log, p) for p in (first_port, second_port, fourth_port)}
                assert len(threads) == 1, f'Connections used different workers: {threads}'
                print('PASS: full queue rejects and closes the excess client')
                print('PASS: capacity recovers and one worker handles three connections')
            finally:
                for sock in clients:
                    sock.close()
                proc.terminate()
                proc.wait(timeout=5)
                upstream_listener.close()


if __name__ == '__main__':
    main(pathlib.Path(sys.argv[1]).resolve())
