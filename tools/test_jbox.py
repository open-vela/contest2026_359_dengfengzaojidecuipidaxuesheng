#!/usr/bin/env python3
"""Exercise the production P4 JMX1 client against a Python loopback peer.

The independent peer uses hashlib/hmac, fragments packets and injects failures.
No discovery broadcasts, physical devices or real credentials are used.
"""
from pathlib import Path
import argparse
import hashlib
import hmac
import json
import os
import select
import socket
import struct
import subprocess
import threading
import time
import unittest

KEY = bytes(range(32))
NONCE = bytes(reversed(range(16)))
SESSION = hmac.digest(KEY, NONCE + b'JMX-SESSION-v1', 'sha256')
HEADER = struct.Struct('<4sBBHII')
GPIOS = bytes([1,2,3,4,5,6,7,8,9,18,21,38,39,40,41,42,47,48])
EXE = None


def exact(fd, n):
    data = b''
    while len(data) < n:
        chunk = fd.recv(n-len(data))
        if not chunk:
            raise EOFError()
        data += chunk
    return data


class Peer:
    def __init__(self, fault=None, remote=0):
        self.fault, self.remote = fault, remote
        self.requests, self.error = [], None
        self.closed = threading.Event()
        self.fd = None
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', 4747))
        self.listener.listen(2)
        self.listener.settimeout(4)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            self.fd, _ = self.listener.accept()
            with self.fd as fd:
                fd.settimeout(3)
                authed = False
                while True:
                    header = exact(fd, 16)
                    magic, version, cmd, flags, seq, n = HEADER.unpack(header)
                    assert magic == b'JMX1' and version == 1
                    data = exact(fd, n)
                    if authed:
                        assert flags == 2
                        assert hmac.compare_digest(exact(fd, 16), hmac.digest(SESSION, header+data, 'sha256')[:16])
                    else:
                        assert flags == 0
                    if cmd == 1:
                        answer = NONCE
                    elif cmd == 2:
                        assert data == hmac.digest(KEY, NONCE+b'JMX-AUTH-v1', 'sha256')
                        answer = b''
                        authed = True
                    elif cmd == 3:
                        answer = KEY
                    else:
                        self.requests.append((cmd, data))
                        if self.fault in ('timeout', 'cancel'):
                            time.sleep(.8)
                            return
                        if cmd == 0x10:
                            answer = struct.pack('<BBBBHH', 1,52,8,18,4096,4747)+GPIOS
                        elif cmd == 0x11:
                            answer = struct.pack('<II8Q',123,456,*([0]*8))+bytes(36)
                        elif cmd in (0x20, 0x22):
                            answer = data if cmd == 0x20 else bytes(64)
                        elif cmd == 0x21:
                            answer = bytes([3])+bytes(64)
                        elif cmd in (0x32, 0x34):
                            answer = struct.pack('<I',5)
                        elif cmd == 0x42:
                            answer = bytes((i & 255 for i in range(struct.unpack_from('<H',data,4)[0])))
                        elif cmd == 0x51:
                            answer = data[3:]
                        elif cmd == 0x61:
                            answer = struct.pack('<H', len(data)-1)
                        elif cmd == 0x62:
                            answer = b'abc'[:struct.unpack_from('<H',data,1)[0]]
                        elif cmd == 0x81:
                            answer = struct.pack('<HI',1,42)
                        else:
                            answer = b''
                    status = self.remote if cmd >= 0x10 else 0
                    body = struct.pack('<i', status)+(answer if not status else b'')
                    rflags = 3 if authed else 1
                    if cmd >= 0x10 and self.fault == 'missing_mac':
                        rflags = 1
                    header = HEADER.pack(b'JMX1',1,cmd,rflags,seq,len(body))
                    if cmd >= 0x10:
                        if self.fault == 'sequence':
                            header = HEADER.pack(b'JMX1',1,cmd,rflags,seq+1,len(body))
                        elif self.fault == 'oversize':
                            header = HEADER.pack(b'JMX1',1,cmd,rflags,seq,8193)
                        elif self.fault == 'flags':
                            header = HEADER.pack(b'JMX1',1,cmd,rflags|4,seq,len(body))
                        elif self.fault == 'malformed_body':
                            body = struct.pack('<i',0)+b'bad'
                            header = HEADER.pack(b'JMX1',1,cmd,rflags,seq,len(body))
                    tag = hmac.digest(SESSION,header+body,'sha256')[:16] if rflags & 2 else b''
                    if self.fault == 'auth_mac' and cmd == 2:
                        tag = bytes(16)
                    if self.fault == 'mac' and cmd >= 0x10:
                        tag = bytes(16)
                    frame = header+body+tag
                    if cmd >= 0x10 and self.fault == 'truncated':
                        fd.sendall(frame[:19])
                        return
                    if cmd >= 0x10 and self.fault == 'drip':
                        for value in frame:
                            fd.sendall(bytes([value]))
                            time.sleep(.04)
                        return
                    # Half headers, payloads and MACs must all be reassembled.
                    for offset in range(0,len(frame),7):
                        fd.sendall(frame[offset:offset+7])
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        except Exception as error:
            self.error = error
        finally:
            self.closed.set()

    def close(self):
        if self.fd:
            try:
                self.fd.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.listener.close()
        self.thread.join(4)
        if self.thread.is_alive():
            raise AssertionError('peer thread leaked')
        if self.error:
            raise self.error


class Client:
    def __init__(self):
        self.p = subprocess.Popen([str(EXE)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,text=True,bufsize=1)

    def call(self, op, **arguments):
        cancel = arguments.pop('_cancelAfterMs',None)
        cancelled = arguments.pop('_cancelBefore',False)
        obj = dict(operation=op,arguments=arguments)
        if cancelled:
            obj['cancelBefore'] = True
        if cancel:
            obj['cancelAfterMs'] = cancel
        self.p.stdin.write(json.dumps(obj)+'\n')
        self.p.stdin.flush()
        if not select.select([self.p.stdout],[],[],6)[0]:
            raise AssertionError('C client exceeded deadline')
        line = self.p.stdout.readline()
        if not line:
            raise AssertionError(self.p.stderr.read())
        return json.loads(line)

    def close(self):
        self.p.stdin.close()
        self.p.wait(5)
        errors = self.p.stderr.read()
        self.p.stdout.close()
        self.p.stderr.close()
        if self.p.returncode or errors:
            raise AssertionError(f'C client rc={self.p.returncode}: {errors}')


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.clients, self.peers = [], []

    def tearDown(self):
        for client in self.clients:
            client.close()
        for peer in self.peers:
            peer.close()

    def connect(self, fault=None, remote=0, timeout=500):
        peer = Peer(fault,remote)
        self.peers.append(peer)
        client = Client()
        self.clients.append(client)
        result = client.call('connect',timeoutMs=timeout)
        self.assertEqual(result['error'],0)
        self.assertEqual(result['generation'],1)
        return client,peer

    def test_all_peripherals_and_function_commands(self):
        client,peer = self.connect()
        g = dict(generation=1)
        self.assertEqual(client.call('caps',**g)['ioGpios'],list(GPIOS))
        self.assertEqual(client.call('status',**g)['heapFree'],456)
        self.assertEqual(client.call('routeSet',nets=[[40,0],[41,1]],**g)['nets'][:2],[[0,40],[1,41]])
        self.assertEqual(client.call('connectNodes',a=40,b=0,**g)['net'],3)
        for op,args in [('removeNode',dict(node=0)),('routeClear',{}),
                        ('gpioConfig',dict(io=9,mode=1)),('gpioWrite',dict(mask=512,levels=512)),
                        ('pwm',dict(channel=0,io=9,hz=1000,duty=8191)),('pwm',dict(channel=0,io=255)),
                        ('i2cSetup',dict(bus=0,sda=0,scl=1)),('i2cClose',dict(bus=0)),
                        ('spiSetup',dict(bus=1,mosi=9,miso=255,sck=10,cs=11)),('spiClose',dict(bus=1)),
                        ('uartSetup',dict(bus=0,tx=9,rx=10)),('uartClose',dict(bus=0)),
                        ('functionLoad',dict(slot=0,arguments=0,code=[0,0,0,0])),('functionClear',dict(slot=0)),('safe',{})]:
            self.assertEqual(client.call(op,**args,**g)['error'],0,op)
        self.assertEqual(client.call('gpioRead',mask=5,**g)['levels'],5)
        self.assertEqual(client.call('adcRead',io=0,**g)['raw'],5)
        self.assertEqual(client.call('i2cTransfer',bus=0,address=0x40,data=[1,2],read=2,**g)['data'],[0,1])
        self.assertEqual(client.call('spiTransfer',bus=1,data=[1,2,3],**g)['data'],[1,2,3])
        self.assertEqual(client.call('uartWrite',bus=0,data=[65,66],**g)['bytes'],2)
        self.assertEqual(client.call('uartRead',bus=0,length=3,**g)['data'],list(b'abc'))
        self.assertEqual(client.call('functionCall',slot=0,ios=[],**g)['values'],[42])
        self.assertEqual(client.call('disconnect',**g)['error'],0)
        self.assertTrue(peer.closed.wait(1))
        self.assertEqual(peer.requests[2][1],struct.pack('<8Q',(1<<40)|1,(1<<41)|2,*([0]*6)))

    def test_maximum_transfer(self):
        client,_ = self.connect(timeout=2000)
        data = [i&255 for i in range(4096)]
        self.assertEqual(client.call('spiTransfer',bus=0,data=data,generation=1)['data'],data)

    def test_invalid_arguments_never_send(self):
        client,peer = self.connect()
        for op,args in [('routeSet',dict(nets=[[0],[0]])),('routeSet',dict(nets=[[52]])),
                        ('gpioConfig',dict(io=True)),('gpioWrite',dict(mask=1,levels=2)),('gpioWrite',dict(mask=1)),
                        ('spiSetup',dict(bus=0,mosi=1,miso=2,sck=3,cs=4,hz=3000000)),
                        ('uartSetup',dict(bus=0,tx=0,rx=0)),('uartRead',dict(bus=0,timeoutMs=501)),
                        ('functionCall',dict(ios=[1,1])),('factoryReset',{}),('pair',dict(host='bad',deviceId='bad')),
                        ('caps',dict(extra=1))]:
            self.assertLess(client.call(op,generation=1,**args)['error'],0,op)
        self.assertLess(client.call('caps',generation=2)['error'],0)
        self.assertEqual(peer.requests,[])

    def test_unknown_device_identity_and_live_connection(self):
        client,peer = self.connect()
        self.assertLess(client.call('connect')['error'],0)
        self.assertEqual(client.call('caps',generation=1)['error'],0)
        self.assertEqual(len(peer.requests),1)

    def test_reconnect_rejects_previous_generation(self):
        client,peer = self.connect()
        self.assertEqual(client.call('disconnect',generation=1)['error'],0)
        self.assertTrue(peer.closed.wait(1))
        peer.close()
        self.peers.remove(peer)
        other = Peer()
        self.peers.append(other)
        self.assertEqual(client.call('connect')['generation'],2)
        self.assertLess(client.call('gpioWrite',generation=1,mask=512,levels=512)['error'],0)
        self.assertEqual(other.requests,[])
        self.assertEqual(client.call('status',generation=2)['error'],0)

    def test_mismatched_binding_identity_never_connects(self):
        client = Client()
        self.clients.append(client)
        self.assertLess(client.call('connect',deviceId='112233445566')['error'],0)

    def check_fault(self, fault):
        client,peer = self.connect(fault)
        result = client.call('status',generation=1)
        self.assertLess(result['error'],0)
        self.assertFalse(result['connected'])
        self.assertLess(client.call('status',generation=1)['error'],0)
        self.assertEqual(len(peer.requests),1,'control operation must never be replayed')

    def test_bad_mac(self): self.check_fault('mac')
    def test_unsigned_reply(self): self.check_fault('missing_mac')
    def test_wrong_sequence(self): self.check_fault('sequence')
    def test_unknown_flags(self): self.check_fault('flags')
    def test_oversized_frame(self): self.check_fault('oversize')
    def test_truncated_frame(self): self.check_fault('truncated')
    def test_bad_status_body(self): self.check_fault('malformed_body')
    def test_timeout(self): self.check_fault('timeout')
    def test_slow_drip_cannot_extend_deadline(self): self.check_fault('drip')

    def test_cancel_closes_connection(self):
        client,peer = self.connect('cancel')
        started = time.monotonic()
        result = client.call('status',generation=1,_cancelAfterMs=50)
        self.assertLess(time.monotonic()-started,.4)
        self.assertLess(result['error'],0)
        self.assertFalse(result['connected'])
        self.assertEqual(len(peer.requests),1)

    def test_cancel_before_dispatch_releases_owned_connection(self):
        client,peer = self.connect()
        self.assertLess(client.call('discover',timeoutMs=100,_cancelBefore=True)['error'],0)
        self.assertTrue(peer.closed.wait(1))
        self.assertEqual(peer.requests,[])

    def test_invalid_pair_does_not_drop_existing_connection(self):
        client,_ = self.connect()
        self.assertLess(client.call('pair',host='255.255.255.255',deviceId='aabbccddeeff')['error'],0)
        self.assertEqual(client.call('status',generation=1)['error'],0)

    def test_remote_failure_separate_from_errno(self):
        client,_ = self.connect(remote=-4)
        result = client.call('safe',generation=1)
        self.assertLess(result['error'],0)
        self.assertEqual(result['remoteStatus'],-4)
        self.assertTrue(result['connected'])

    def test_invalid_auth_reply(self):
        peer = Peer('auth_mac')
        self.peers.append(peer)
        client = Client()
        self.clients.append(client)
        self.assertLess(client.call('connect',timeoutMs=500)['error'],0)
        self.assertLess(client.call('caps',generation=1)['error'],0)
        self.assertEqual(peer.requests,[])

    def test_binding_does_not_disclose_key(self):
        peer = Peer()
        self.peers.append(peer)
        client = Client()
        self.clients.append(client)
        reply = client.call('pair',host='127.0.0.1',deviceId='AABBCCDDEEFF')
        self.assertEqual(reply,dict(saved=True,error=0))
        self.assertTrue(peer.closed.wait(1))

    def test_discovery_validates_and_deduplicates(self):
        fd = socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        fd.bind(('127.0.0.1',4748))
        fd.settimeout(2)
        def reply():
            data,peer = fd.recvfrom(128)
            assert data == b'JBOX-DISCOVER'
            for response in (b'bad',b'JBOX-HERE|nothexdevice|1|1',b'JBOX-HERE|aabbccddeeff|1|1',b'JBOX-HERE|aabbccddeeff|1|1'):
                fd.sendto(response,peer)
        thread = threading.Thread(target=reply)
        thread.start()
        client = Client()
        self.clients.append(client)
        try:
            result = client.call('discover',timeoutMs=150)
            self.assertEqual(result['error'],0)
            self.assertEqual(result['devices'],[dict(deviceId='aabbccddeeff',host='127.0.0.1',paired=True,pairingOpen=True)])
        finally:
            thread.join(3)
            fd.close()


def main():
    global EXE
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apps',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    d = root/'app/espdl-quickapp/overlay/apps/system/desktop'
    a.output.mkdir(parents=True,exist_ok=True)
    EXE = a.output.resolve()/'jbox-client'
    flags = ['-std=gnu11','-D_GNU_SOURCE','-O1','-g','-Wall','-Wextra','-Werror',
                    '-pthread','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-fno-pie','-no-pie','-DJBOX_DISCOVERY_ADDRESS="127.0.0.1"',
                    '-I'+str(d),'-I'+str(a.apps/'netutils/cjson/cJSON')]
    # Keep legacy SHA formatting diagnostics isolated; new JBOX sources are
    # compiled with all warnings promoted to errors, including indentation.
    sha_object = a.output.resolve()/'legacy-sha.o'
    subprocess.run(['gcc',*flags,'-Wno-misleading-indentation','-c',
                    str(d/'qpk_espdl_sha256.c'),'-o',str(sha_object)],check=True)
    subprocess.run(['gcc',*flags,
                    str(root/'app/espdl-quickapp/tests/jbox_client_host.c'),str(d/'qpk_jbox.c'),
                    str(d/'qpk_jbox_transport.c'),str(sha_object),
                    str(a.apps/'netutils/cjson/cJSON/cJSON.c'),'-o',str(EXE)],check=True)
    os.environ['ASAN_OPTIONS']='detect_leaks=1'
    os.environ['UBSAN_OPTIONS']='halt_on_error=1'
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(ProtocolTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    print(f'JBOX_OK: {result.testsRun} loopback protocol tests; ASan/UBSan; no board access')


if __name__ == '__main__':
    main()
