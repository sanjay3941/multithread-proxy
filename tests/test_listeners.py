"""Check default localhost + eth2 listeners and direct local dashboard access."""
import http.client
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def get(address, port, path):
    client = http.client.HTTPConnection(address, port, timeout=3)
    try:
        client.request('GET', path)
        response = client.getresponse()
        return response.status, response.read()
    finally:
        client.close()

def main(binary):
    interfaces = json.loads(subprocess.check_output(['ip', '-j', '-4', 'addr'], text=True))
    eth2 = [a['local'] for i in interfaces if i['ifname'] == 'eth2' and 'UP' in i['flags']
            for a in i['addr_info'] if a['family'] == 'inet']
    expected = list(dict.fromkeys(['127.0.0.1'] + eth2))
    with tempfile.TemporaryDirectory() as directory:
        proxy_port, dashboard_port = free_port(), free_port()
        env = {k:v for k,v in os.environ.items() if not k.startswith('PROXY_')}
        env['PROXY_DASHBOARD_PORT'] = str(dashboard_port)
        with open(pathlib.Path(directory)/'console.log','w') as out:
            process = subprocess.Popen([str(binary),str(proxy_port)], cwd=directory, env=env, stdout=out, stderr=out)
            try:
                for _ in range(100):
                    assert process.poll() is None, 'Startup failed'
                    log = (pathlib.Path(directory)/'console.log').read_text()
                    if log.count('PROXY_LISTENING ADDRESS=') == len(expected): break
                    time.sleep(.03)
                else: raise AssertionError('Listeners did not start')
                status, body = get('127.0.0.1', dashboard_port, '/api/status')
                assert status == 200
                state = json.loads(body)
                assert state['bindAddresses'] == expected, state
                assert state['proxyPort'] == proxy_port and state['dashboardPort'] == dashboard_port
                assert b'Proxy configuration' in get('127.0.0.1',dashboard_port,'/')[1]
                for address in expected:
                    assert get(address,proxy_port,'http://proxy.internal/')[0] == 200
                    assert get(address,proxy_port,'http://doubleclick.net/')[0] == 403
                for interface in interfaces:
                    for addr in interface['addr_info']:
                        ip = addr['local']
                        if ip in expected: continue
                        with socket.socket() as s:
                            s.settimeout(.5)
                            assert s.connect_ex((ip,proxy_port)) != 0, 'Unexpected listener on '+ip
                print('PASS: direct dashboard without proxy; configuration reflects actual listeners')
                print('PASS: proxy/dashboard routing and blocking on '+', '.join(expected))
                print('PASS: unrelated interfaces are not exposed')
            finally:
                process.terminate()
                try: process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()

if __name__ == '__main__':
    main(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'build/cpp_proxy').resolve())
