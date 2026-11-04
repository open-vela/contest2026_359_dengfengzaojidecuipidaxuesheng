#!/usr/bin/env python3
"""Native C provisioning codec vs an independent protobuf/crypto device.

Only synthetic keys/credentials and in-process callbacks are used. No BLE,
network traffic, household devices or model requests. Windows GCC supported.
"""
import argparse, ctypes as C, errno, json, os, pathlib, random, shutil, subprocess, sys, unittest
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT=pathlib.Path(__file__).resolve().parents[1]
SOURCE=ROOT/'app/espdl-quickapp/overlay/apps/system/desktop'

def messages():
    # Wire fields from Espressif's session.proto/sec1.proto/wifi_config.proto.
    # The peer uses protobuf's decoder, not the production manual field parser.
    schemas={
        'Empty':[], 'Cmd0':[(1,'public','bytes')],
        'Resp0':[(1,'status','uint32'),(2,'public','bytes'),(3,'iv','bytes')],
        'Cmd1':[(2,'verify','bytes')], 'Resp1':[(1,'status','uint32'),(3,'verify','bytes')],
        'Sec1':[(1,'msg','uint32'),(20,'cmd0','Cmd0'),(21,'resp0','Resp0'),(22,'cmd1','Cmd1'),(23,'resp1','Resp1')],
        'Session':[(2,'version','uint32'),(11,'sec1','Sec1')],
        'Set':[(1,'ssid','bytes'),(2,'password','bytes')], 'Status':[(1,'status','uint32')],
        'Connected':[(1,'ip','string')],
        'Get':[(1,'status','uint32'),(2,'state','uint32'),(11,'connected','Connected')],
        'Config':[(1,'msg','uint32'),(10,'get','Empty'),(11,'get_reply','Get'),
                  (12,'set','Set'),(13,'set_reply','Status'),(14,'apply','Empty'),(15,'apply_reply','Status')],
    }
    file=descriptor_pb2.FileDescriptorProto(name='jbox_peer.proto',syntax='proto3')
    types={'bytes':12,'uint32':13,'string':9}
    for name,fields in schemas.items():
        msg=file.message_type.add(name=name)
        for number,name,kind in fields:
            f=msg.field.add(name=name,number=number,label=1,type=types.get(kind,11))
            if kind not in types:f.type_name='.'+kind
    pool=descriptor_pool.DescriptorPool();pool.Add(file)
    return {name:message_factory.GetMessageClass(pool.FindMessageTypeByName(name)) for name in schemas}

PROTO=messages()
def public(key):
    return key.public_key().public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw)

CHECK=C.CFUNCTYPE(C.c_int,C.c_void_p)
KEYPAIR=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p)
START=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_void_p)
CRYPT=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p)
EXCHANGE=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_int,C.c_void_p,C.c_size_t,C.c_void_p,C.POINTER(C.c_size_t))
PAUSE=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint)
class Ops(C.Structure):
    _fields_=[('context',C.c_void_p),('check',CHECK),('keypair',KEYPAIR),('start',START),('crypt',CRYPT),('exchange',EXCHANGE),('pause',PAUSE)]

class Peer:
    def __init__(self,fault=None,raw=None):
        self.fault=fault;self.raw=raw;self.calls=[];self.credentials=[];self.polls=0
        self.client=X25519PrivateKey.from_private_bytes(bytes(range(1,33)))
        self.server=X25519PrivateKey.from_private_bytes(bytes(range(32,64)))
        self.client_cipher=None;self.server_cipher=None;self.cancel=False;self.errors=[];self.ticks=0
    def guard(self,fn):
        def run(*a):
            try:return fn(*a)
            except Exception as error:self.errors.append(repr(error));return -5
        return run
    def check(self,_):
        if self.cancel:return LIB.test_errno(1)
        if self.ticks>=3000 and self.fault=='deadline':return LIB.test_errno(2)
        return 0
    def keypair(self,_,out):C.memmove(out,public(self.client),32);return 0
    def start(self,_,peer,iv):
        shared=self.client.exchange(X25519PublicKey.from_public_bytes(C.string_at(peer,32)))
        self.client_cipher=Cipher(algorithms.AES(shared),modes.CTR(C.string_at(iv,16))).encryptor();return 0
    def crypt(self,_,data,n,out):C.memmove(out,self.client_cipher.update(C.string_at(data,n)),n);return 0
    def pause(self,_,ms):self.ticks+=ms;return self.check(None)
    def exchange(self,_,session,data,n,out,length):
        wire=C.string_at(data,n)
        if session:
            request=PROTO['Session'].FromString(wire);assert request.version==1
            response=PROTO['Session'](version=1)
            if request.sec1.HasField('cmd0'):
                self.calls.append('hello')
                assert request.sec1.msg==0 and len(request.sec1.cmd0.public)==32
                shared=self.server.exchange(X25519PublicKey.from_public_bytes(request.sec1.cmd0.public))
                self.server_cipher=Cipher(algorithms.AES(shared),modes.CTR(bytes(range(16)))).encryptor()
                response.sec1.msg=1;response.sec1.resp0.public=public(self.server);response.sec1.resp0.iv=bytes(range(16))
                if self.fault=='downgrade':response.version=0
                if self.fault=='short-key':response.sec1.resp0.public=b'x'*31
                if self.fault=='short-iv':response.sec1.resp0.iv=b'x'*15
                if self.fault=='zero-key':response.sec1.resp0.public=b'\0'*32
                wire=response.SerializeToString() if self.raw is None else self.raw
            else:
                self.calls.append('proof');assert request.sec1.msg==2
                assert self.server_cipher.update(request.sec1.cmd1.verify)==public(self.server)
                response.sec1.msg=3
                response.sec1.resp1.verify=self.server_cipher.update(public(self.client) if self.fault!='proof' else b'\0'*32)
                wire=response.SerializeToString()
        else:
            request=PROTO['Config'].FromString(self.server_cipher.update(wire));response=PROTO['Config'](msg=request.msg+1)
            if request.msg==2:
                self.calls.append('set');self.credentials.append((bytes(request.set.ssid),bytes(request.set.password)))
                if self.fault=='write-timeout':return LIB.test_errno(2)
                response.set_reply.SetInParent()
                if self.fault=='set-rejected':response.set_reply.status=1
                if self.fault=='cancel-after-set':self.cancel=True
            elif request.msg==4:
                self.calls.append('apply');response.apply_reply.SetInParent()
                if self.fault=='apply-timeout':return LIB.test_errno(2)
            else:
                assert request.msg==0;self.calls.append('get');self.polls+=1
                response.get_reply.state=1 if self.polls<3 or self.fault in ('connecting','deadline') else 0
                if self.fault=='wifi-failed':response.get_reply.state=3
                if response.get_reply.state==0:
                    response.get_reply.connected.ip='192.168.1.72'
                    if self.fault=='missing-ip':response.get_reply.ClearField('connected')
                    if self.fault=='loopback':response.get_reply.connected.ip='127.0.0.1'
                    if self.fault=='multicast':response.get_reply.connected.ip='224.0.0.1'
                    if self.fault=='bad-ip':response.get_reply.connected.ip='999.1.2.3'
                    if self.fault=='duplicate-state':
                        body=response.get_reply.SerializeToString()+b'\x10\x01'
                        # Config msg=1, response tag11; inject duplicate inside status.
                        wire=b'\x08\x01\x5a'+bytes([len(body)])+body
                        wire=self.server_cipher.update(wire)
                        C.memmove(out,wire,len(wire));length[0]=len(wire);return 0
            wire=self.server_cipher.update(response.SerializeToString())
        if len(wire)>length[0]:return LIB.test_errno(3)
        C.memmove(out,wire,len(wire));length[0]=len(wire);return 0
    def run(self,ssid='unit-network',password='unit-password'):
        callbacks=[CHECK(self.guard(self.check)),KEYPAIR(self.guard(self.keypair)),START(self.guard(self.start)),
                   CRYPT(self.guard(self.crypt)),EXCHANGE(self.guard(self.exchange)),PAUSE(self.guard(self.pause))]
        ops=Ops(None,*callbacks);ip=C.create_string_buffer(16)
        result=LIB.jbox_wifi_provision(C.byref(ops),ssid.encode(),password.encode(),ip)
        return result,ip.value.decode()

class Tests(unittest.TestCase):
    def test_success(self):
        p=Peer();self.assertEqual(p.run(),(0,'192.168.1.72'));self.assertFalse(p.errors)
        self.assertEqual(p.calls,['hello','proof','set','apply','get','get','get'])
        self.assertEqual(p.credentials,[(b'unit-network',b'unit-password')])
    def test_maximum_utf8_and_open_network(self):
        for ssid,password in [('界'*10+'ab','x'*64),('open','')]:
            p=Peer();self.assertEqual(p.run(ssid,password)[0],0);self.assertEqual(p.credentials,[(ssid.encode(),password.encode())])
    def test_invalid_credential_bounds(self):
        for ssid,password in [('', ''),('x'*33,''),('x','x'*65),('界'*11,'')]:
            p=Peer();self.assertLess(p.run(ssid,password)[0],0);self.assertEqual(p.calls,[])
    def test_handshake_rejections_never_send_credentials(self):
        for fault in ['downgrade','short-key','short-iv','zero-key','proof']:
            with self.subTest(fault=fault):
                p=Peer(fault);self.assertLess(p.run()[0],0);self.assertEqual(p.credentials,[])
    def test_cancel_before_start(self):
        p=Peer();p.cancel=True;self.assertEqual(p.run()[0],LIB.test_errno(1));self.assertEqual(p.calls,[])
    def test_cancel_after_set_never_applies(self):
        p=Peer('cancel-after-set');self.assertEqual(p.run()[0],LIB.test_errno(1));self.assertEqual(p.calls.count('set'),1);self.assertNotIn('apply',p.calls)
    def test_no_action_replay(self):
        for fault in ['write-timeout','apply-timeout','set-rejected']:
            p=Peer(fault);self.assertLess(p.run()[0],0);self.assertEqual(p.calls.count('set'),1)
            self.assertEqual(p.calls.count('apply'),int(fault=='apply-timeout'))
    def test_status_failures(self):
        for fault in ['wifi-failed','missing-ip','loopback','multicast','bad-ip','duplicate-state']:
            p=Peer(fault);self.assertLess(p.run()[0],0);self.assertEqual(p.calls.count('set'),1);self.assertEqual(p.calls.count('apply'),1)
    def test_poll_limit(self):
        p=Peer('connecting');self.assertEqual(p.run()[0],LIB.test_errno(2));self.assertEqual(p.polls,90)
    def test_absolute_deadline(self):
        p=Peer('deadline');self.assertEqual(p.run()[0],LIB.test_errno(2));self.assertLess(p.polls,90)
    def test_malformed_sessions(self):
        for raw in [b'',b'\x10\x00',b'\x10\x01\x10\x01',b'\x5a\xff\xff\xff\xff\xff',b'\x0f',b'\x00']:
            p=Peer(raw=raw);self.assertLess(p.run()[0],0);self.assertEqual(p.credentials,[])
    def test_bounded_parser_fuzz(self):
        rng=random.Random(20261105)
        for _ in range(2000):
            p=Peer(raw=rng.randbytes(rng.randrange(0,193)));self.assertLess(p.run()[0],0);self.assertEqual(p.credentials,[])

def main():
    global LIB
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    shim=out/'errno.c';shim.write_text('#include <errno.h>\nint test_errno(int i){return i==1?-ECANCELED:i==2?-ETIMEDOUT:-EMSGSIZE;}\n')
    dll=out/('jbox_wifi.dll' if os.name=='nt' else 'jbox_wifi.so')
    command=[shutil.which('gcc') or 'gcc','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-O1','-g','-shared']
    if os.name!='nt':command+=['-fPIC']
    command += [str(SOURCE/'jbox_wifi.c'),str(shim),'-o',str(dll)]
    subprocess.run(command,check=True);LIB=C.CDLL(str(dll))
    LIB.jbox_wifi_provision.argtypes=[C.POINTER(Ops),C.c_char_p,C.c_char_p,C.c_char_p];LIB.jbox_wifi_provision.restype=C.c_int
    LIB.test_errno.argtypes=[C.c_int];LIB.test_errno.restype=C.c_int
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests))
    (out/'RESULT.json').write_text(json.dumps({'passed':result.wasSuccessful(),'tests':result.testsRun,'fuzz_cases':2000,
        'native_C':True,'independent_protobuf_crypto_peer':True,'real_BLE_tested':False,'PSA_backend_executed':False},indent=2))
    return 0 if result.wasSuccessful() else 1
if __name__=='__main__':sys.exit(main())
