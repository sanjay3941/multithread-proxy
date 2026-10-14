"""Per-client accounting with two distinct loopback source IPs, no internet."""
import http.client
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from test_privacy import Server, Origin, free_port

def main(binary):
    with tempfile.TemporaryDirectory() as directory, Server(('127.0.0.1',0),Origin) as origin:
        origin.received=[]
        threading.Thread(target=origin.serve_forever,daemon=True).start()
        port, admin=free_port(),free_port()
        env={k:v for k,v in os.environ.items() if not k.startswith('PROXY_')}
        env.update(PROXY_BIND_ADDRESS='127.0.0.1',PROXY_DASHBOARD_PORT=str(admin))
        def api(path='/api/status',fields=None):
            c=http.client.HTTPConnection('127.0.0.1',admin,timeout=3)
            try:
                c.request('POST' if fields else 'GET',path,urllib.parse.urlencode(fields) if fields else None,
                          {'Content-Type':'application/x-www-form-urlencoded','X-Proxy-Dashboard':'1'})
                response=c.getresponse(); assert response.status==200
                return json.loads(response.read())
            finally: c.close()
        def request(ip,target):
            c=http.client.HTTPConnection('127.0.0.1',port,timeout=3,source_address=(ip,0))
            try:
                c.request('GET',target); response=c.getresponse(); response.read(); return response.status
            finally: c.close()
        def settled():
            for _ in range(100):
                state=api()
                if state['connections']['active']==0 and all(d['nameStatus']!='pending' for d in state['devices']['items']): return state
                time.sleep(.02)
            raise AssertionError('Counters did not settle')
        with open(pathlib.Path(directory)/'log','w') as log:
            proc=subprocess.Popen([str(binary),str(port)],cwd=directory,env=env,stdout=log,stderr=log)
            try:
                for _ in range(100):
                    assert proc.poll() is None
                    try: api(); break
                    except OSError: time.sleep(.03)
                assert api()['devices']['items']==[]
                assert request('127.0.0.2','http://proxy.internal/')==200
                assert settled()['devices']['items']==[], 'Dashboard-only client should not appear'
                api('/api/rules',dict(action='add',list='block',domain='blocked.invalid'))
                for domain in ['doubleclick.net','google-analytics.com','blocked.invalid']:
                    assert request('127.0.0.2','http://'+domain+'/')==403
                assert request('127.0.0.3',f'http://127.0.0.1:{origin.server_address[1]}/')==200
                state=settled(); devices={d['ip']:d for d in state['devices']['items']}
                assert set(devices)=={'127.0.0.2','127.0.0.3'}
                blocked=devices['127.0.0.2']; completed=devices['127.0.0.3']
                assert (blocked['total'],blocked['blocked'],blocked['ads'],blocked['trackers'],blocked['custom'])==(3,3,1,1,1)
                assert completed['total']==completed['successful']==1 and completed['blocked']==0
                assert completed['received']>0 and completed['sent']>0
                assert all(d['name']==socket.gethostname() and d['nameStatus']=='resolved' for d in devices.values())
                assert sum(d['total'] for d in devices.values())==state['connections']['total']
                # An open connection appears active and a malformed request becomes a failure.
                with socket.create_connection(('127.0.0.1',port),timeout=3,source_address=('127.0.0.3',0)) as client:
                    for _ in range(50):
                        rows={d['ip']:d for d in api()['devices']['items']}
                        if rows['127.0.0.3']['active']==1: break
                        time.sleep(.02)
                    else: raise AssertionError('Missing active connection')
                    client.sendall(b'BAD\r\n\r\n'); assert client.recv(1024).startswith(b'HTTP/1.1 400')
                rows={d['ip']:d for d in settled()['devices']['items']}
                assert rows['127.0.0.3']['failed']==1 and rows['127.0.0.3']['active']==0
                print('PASS: two client IPs split totals, block categories, successes, failures, active connections and traffic')
                print('PASS: dashboard exclusion and local hostname lookup')
            finally:
                proc.terminate()
                try: proc.wait(timeout=5)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                origin.shutdown()

if __name__=='__main__':
    main(pathlib.Path(sys.argv[1] if len(sys.argv)>1 else 'build/cpp_proxy').resolve())
