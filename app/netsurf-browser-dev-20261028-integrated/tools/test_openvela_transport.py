#!/usr/bin/env python3
"""Test production adapter + real NuttX webclient on loopback HTTP/TLS.

Only glass_https is replaced with OpenSSL on the host. Device mbedTLS still
requires on-board validation. No external network connection is made.
"""
import argparse
import errno
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ssl
import subprocess
import threading
import gzip,zlib,time
import os
import socket

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--core',type=Path,help='NetSurf source root when running an exported app project')
p.add_argument('--sanitize',action='store_true',help='enable host address/undefined-behavior checking')
p.add_argument('--response-headers-only',action='store_true')
p.add_argument('--expect-short-response-lines',action='store_true')
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
apps=a.target/'apps'
flags=['-std=gnu11','-D_GNU_SOURCE','-DOV_HTTP_CONNECT_MS=700','-DOV_HTTP_IDLE_MS=700',
       '-include',str(root/'tests/openvela/host/nuttx/config.h')]
if a.sanitize:flags+=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
for inc in [root/'tests/openvela/host',root/'platform/openvela',apps/'include',apps/'system/desktop',
            (a.core or root)/'deps/netsurf-modern/netsurf']:
    flags+=['-I',str(inc)]
sources=[root/'platform/openvela/openvela_http_nuttx.c',root/'tests/openvela/transport.c',
         root/'tests/openvela/host_tls.c', apps/'netutils/webclient/webclient.c',
         apps/'netutils/netlib/netlib_parseurl.c']
subprocess.run(['gcc',*flags,*map(str,sources),'-o',str(out/'transport'),'-lssl','-lcrypto','-lz','-pthread'],check=True)
for name in ['trusted','untrusted']:
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
        '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost',
        '-keyout',str(out/(name+'.key')),'-out',str(out/(name+'.pem'))],
        check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args): pass
    def do_GET(self):
        if self.path in ['/long-response','/too-long-response','/many-response']:
            self.send_response(200)
            if self.path=='/long-response':
                self.send_header('Content-Security-Policy',"default-src 'self'; report-uri /"+'a'*9400)
            elif self.path=='/too-long-response':self.send_header('X-Pad','a'*20000)
            else:
                for i in range(10):self.send_header('X-Pad-'+str(i),'a'*8000)
            self.send_header('Content-Length','5');self.end_headers()
            try:self.wfile.write(b'hello')
            except (BrokenPipeError,ConnectionResetError):pass
            return
        if self.path=='/long-cookie':
            assert self.headers.get('Cookie')=='session='+'x'*8000
            assert self.headers.get('X-Test')=='y'*7899
        if self.path=='/stall-headers':
            time.sleep(2); return
        if self.path=='/redirect':
            self.send_response(302); self.send_header('Location','/ok');
            self.send_header('Content-Encoding','gzip')
            self.send_header('Content-Length','0'); self.end_headers(); return
        if self.path in ['/no-content','/not-modified']:
            self.send_response(204 if self.path=='/no-content' else 304)
            self.send_header('Content-Encoding','gzip');self.send_header('Content-Length','0')
            self.end_headers();return
        encodings={'/gzip':('gzip',gzip.compress(b'hello')),
                   '/deflate':('deflate',zlib.compress(b'hello')),
                   '/gzip-split':('gzip',gzip.compress(b'hello')),
                   '/gzip-concat':('gzip',gzip.compress(b'he')+gzip.compress(b'llo')),
                   '/gzip-truncated':('gzip',gzip.compress(b'hello')[:-4]),
                   '/gzip-corrupt':('gzip',gzip.compress(b'hello')[:-8]+b'bad-tail'),
                   '/deflate-junk':('deflate',zlib.compress(b'hello')+b'junk'),
                   '/gzip-limit':('gzip',gzip.compress(b'x'*(4*1024*1024+1))),
                   '/gzip-cancel':('gzip',gzip.compress(b'x'*8192)),
                   '/gzip-sink-fail':('gzip',gzip.compress(b'hello')),
                   '/gzip-empty':('gzip',b''),
                   '/gzip-stack':('gzip, deflate',gzip.compress(b'hello')),
                   '/gzip-duplicate':('gzip',gzip.compress(b'hello')),
                   '/br':('br',b'hello')}
        if self.path in encodings:
            coding,body=encodings[self.path];self.send_response(200)
            self.send_header('Content-Length',str(len(body))) # Deliberately before coding.
            self.send_header('Content-Encoding',coding)
            if self.path=='/gzip-duplicate':self.send_header('Content-Encoding','gzip')
            self.send_header('Content-Type','text/plain');self.end_headers()
            try:
                if self.path=='/gzip-split':
                    for byte in body:self.wfile.write(bytes([byte]));self.wfile.flush();time.sleep(.01)
                else:self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError):pass
            return
        self.send_response(200)
        self.send_header('Content-Type','text/html')
        if self.path=='/chunked':
            self.send_header('Transfer-Encoding','chunked'); self.end_headers()
            self.wfile.write(b'5\r\nhello\r\n0\r\n\r\n'); return
        self.send_header('Content-Length','5'); self.end_headers()
        if self.path=='/stall-body':
            time.sleep(2); return
        if self.path=='/progress':
            for byte in b'hello':self.wfile.write(bytes([byte]));self.wfile.flush();time.sleep(.25)
        else:self.wfile.write(b'hello')
http=ThreadingHTTPServer(('127.0.0.1',0),Handler)
tls=ThreadingHTTPServer(('127.0.0.1',0),Handler)
context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(str(out/'trusted.pem'),str(out/'trusted.key'))
tls.socket=context.wrap_socket(tls.socket,server_side=True)
servers=[http,tls]
for server in servers: threading.Thread(target=server.serve_forever,daemon=True).start()
# Fill a loopback listener's accept queue without accepting: the next TCP
# handshake remains pending. This tests a real nonblocking connect deadline,
# without external routing, a fake webclient or changing system firewall rules.
pending=socket.socket(); pending.bind(('127.0.0.1',0)); pending.listen(0)
occupant=socket.create_connection(pending.getsockname(),timeout=1)
def test(url,expected=0,status=200,ca='trusted',env=None):
    subprocess.run([str(out/'transport'),url,str(out/(ca+'.pem')),str(expected),str(status)],
        check=True,timeout=15,env=dict(os.environ,**(env or {})))
try:
    hp=http.server_address[1]; tp=tls.server_address[1]
    for scheme,port in [('http',hp),('https',tp)]:
        test(f'{scheme}://localhost:{port}/long-response',-errno.E2BIG if a.expect_short_response_lines else 0)
        test(f'{scheme}://localhost:{port}/too-long-response',-errno.E2BIG)
        test(f'{scheme}://localhost:{port}/many-response',-errno.E2BIG)
    print('PASS: long CSP kept intact; oversized line and aggregate headers rejected' if not a.expect_short_response_lines else 'CONFIRMED: original webclient truncates long response headers',flush=True)
    if a.response_headers_only:raise SystemExit(0)
    blocked=pending.getsockname()[1]
    started=time.monotonic()
    test(f'http://127.0.0.1:{blocked}/ok',-errno.ETIMEDOUT)
    assert time.monotonic()-started<1.5,'plain TCP connect escaped its initial budget'
    started=time.monotonic()
    test(f'http://127.0.0.1:{blocked}/ok',-errno.ECANCELED,env={'OV_TEST_CANCEL_LATER':'1'})
    assert time.monotonic()-started<.6,'TCP connect cancellation waited for OS retransmission timeout'
    test(f'http://localhost:{hp}/ok')
    test(f'http://localhost:{hp}/long-cookie')
    test(f'http://localhost:{hp}/chunked')
    test(f'http://localhost:{hp}/progress')
    test(f'http://localhost:{hp}/stall-headers',-errno.ETIMEDOUT)
    test(f'http://localhost:{hp}/stall-body',-errno.ETIMEDOUT)
    started=time.monotonic()
    test(f'http://localhost:{hp}/stall-headers',-errno.ECANCELED,env={'OV_TEST_CANCEL_LATER':'1'})
    assert time.monotonic()-started<.6,'plain HTTP cancellation did not interrupt socket wait'
    test(f'http://localhost:{hp}/redirect',status=302)
    test(f'http://localhost:{hp}/no-content',status=204)
    test(f'http://localhost:{hp}/not-modified',status=304)
    for path in ['gzip','deflate','gzip-split','gzip-concat']:test(f'http://localhost:{hp}/{path}')
    for path in ['gzip-truncated','gzip-corrupt','deflate-junk','gzip-empty']:test(f'http://localhost:{hp}/{path}',-errno.EPROTO)
    test(f'http://localhost:{hp}/gzip-limit',-errno.EFBIG)
    test(f'http://localhost:{hp}/gzip-cancel',-errno.ECANCELED)
    test(f'http://localhost:{hp}/gzip-sink-fail',-errno.EIO)
    for path in ['br','gzip-stack','gzip-duplicate']:test(f'http://localhost:{hp}/{path}',-errno.ENOTSUP)
    test(f'https://localhost:{tp}/ok')
    test(f'https://localhost:{tp}/long-cookie')
    test(f'https://localhost:{tp}/ok',-errno.ETIMEDOUT,env={
        'OV_TEST_TLS_HANDSHAKE_TIME':'1','OV_TEST_TLS_LEGACY_PHASE':'1',
        'OV_TEST_TLS_CONNECTS':'1','OV_TEST_NO_SEND':'1'})
    test(f'https://localhost:{tp}/ok',env={'OV_TEST_TLS_HANDSHAKE_TIME':'1','OV_TEST_TLS_CONNECTS':'1'})
    test(f'https://localhost:{tp}/gzip')
    test(f'https://localhost:{tp}/ok',-errno.EACCES,ca='untrusted')
    test(f'https://127.0.0.1:{tp}/ok',-errno.EACCES)
    test(f'https://localhost:{tp}/ok',env={'OV_TEST_TLS_FAULT':'once','OV_TEST_TLS_CONNECTS':'2'})
    test(f'https://localhost:{tp}/ok',-errno.ETIMEDOUT,env={'OV_TEST_TLS_FAULT':'spent-budget','OV_TEST_TLS_CONNECTS':'1','OV_TEST_NO_SEND':'1'})
    test(f'https://localhost:{tp}/ok',env={'OV_TEST_TLS_FAULT':'bio-once','OV_TEST_TLS_CONNECTS':'2'})
    test(f'https://localhost:{tp}/ok',-errno.EPROTO,env={'OV_TEST_TLS_FAULT':'bio-always','OV_TEST_TLS_CONNECTS':'3','OV_TEST_NO_SEND':'1'})
    for fault in ['bio-verify','protocol']:
        test(f'https://localhost:{tp}/ok',-errno.EPROTO,env={'OV_TEST_TLS_FAULT':fault,'OV_TEST_TLS_CONNECTS':'1','OV_TEST_NO_SEND':'1'})
    test(f'https://localhost:{tp}/ok',-errno.ETIMEDOUT,env={'OV_TEST_TLS_FAULT':'always','OV_TEST_TLS_CONNECTS':'3','OV_TEST_NO_SEND':'1'})
    test(f'https://localhost:{tp}/ok',-errno.ETIMEDOUT,env={'OV_TEST_TLS_FAULT':'once','OV_TEST_TLS_CONNECTS':'1','OV_TEST_POST':'1','OV_TEST_NO_SEND':'1'})
    test(f'https://localhost:{tp}/ok',-errno.EACCES,env={'OV_TEST_TLS_FAULT':'certificate','OV_TEST_TLS_CONNECTS':'1','OV_TEST_NO_SEND':'1'})
    test(f'https://localhost:{tp}/ok',-errno.ETIMEDOUT,env={'OV_TEST_TLS_FAULT':'cancel','OV_TEST_TLS_CONNECTS':'1','OV_TEST_NO_SEND':'1'})
    test(f'https://localhost:{tp}/ok',-errno.ETIMEDOUT,env={'OV_TEST_TLS_FAULT':'receive','OV_TEST_TLS_CONNECTS':'1'})
    test(f'https://localhost:{tp}/progress',env={'OV_TEST_TLS_PROGRESS':'1','OV_TEST_TLS_CONNECTS':'1'})
finally:
    occupant.close(); pending.close()
    for server in servers: server.shutdown(); server.server_close()
print('PASS: HTTP/TLS, gzip/deflate/split/concatenated streams, bad/truncated trailers, expansion limit, cancellation/sink errors, unsupported coding, bad CA and wrong hostname')
print('PASS: transient pre-send TLS retry bounded to three attempts and aggregate connection budget; no POST/certificate/cancel/receive replay; progressing TLS stream outlives initial connect budget')
print('PASS: nonblocking plain HTTP stalls time out, progress renews idle deadline, cancellation interrupts wait; handshake BIO retry excludes verification/protocol errors')
print('PASS: expired handshake budget reproduces legacy first-write timeout; bounded TLS phase transition completes HTTP 200')
