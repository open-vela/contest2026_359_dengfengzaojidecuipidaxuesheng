#!/usr/bin/env python3
"""JBOX-S3 LAN binary API. No web interface, no code or UART needed to pair.

First use: press GPIO10 button, BLE-provision Wi-Fi with Espressif's phone
app (Security 1, empty PoP). Press GPIO10 again if required, then:
    python client/jbox_api.py info       # discover, auto-claim key, HMAC
    python client/jbox_api.py pair       # optional explicit binding command

The pairing button gates first-key disclosure; the LAN binding exchange is
not encrypted. Use only on a trusted local network and never port-forward.
"""
from __future__ import annotations
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import time
import socket
import struct
import threading
from typing import Sequence

MAGIC=b'JMX1'
HEADER=struct.Struct('<4sBBHII')
MAX_PAYLOAD=8192
PORT=4747
F_REPLY=1
F_MAC=2

CMD_HELLO=0x01;CMD_AUTH=0x02;CMD_PAIR=0x03
CMD_CAPS=0x10;CMD_STATUS=0x11
CMD_ROUTE_SET=0x20;CMD_ROUTE_CONNECT=0x21;CMD_ROUTE_REMOVE=0x22;CMD_ROUTE_CLEAR=0x23
CMD_IO_CONFIG=0x30;CMD_IO_WRITE=0x31;CMD_IO_READ=0x32;CMD_PWM=0x33;CMD_ADC=0x34
CMD_I2C_SETUP=0x40;CMD_I2C_CLOSE=0x41;CMD_I2C_XFER=0x42
CMD_SPI_SETUP=0x50;CMD_SPI_XFER=0x51;CMD_SPI_CLOSE=0x52
CMD_UART_SETUP=0x60;CMD_UART_WRITE=0x61;CMD_UART_READ=0x62;CMD_UART_CLOSE=0x63
CMD_SAFE=0x70;CMD_FACTORY_RESET=0x7f
CMD_FUNCTION_LOAD=0x80;CMD_FUNCTION_CALL=0x81;CMD_FUNCTION_CLEAR=0x82

GPIO_LIST=[1,2,3,4,5,6,7,8,9,18,21,38,39,40,41,42,47,48]

DISCOVERY_PORT=4748
DISCOVERY_REQUEST=b'JBOX-DISCOVER'


def discover_devices(timeout: float=1.5) -> list[dict]:
    """Find devices by unauthenticated UDP broadcast; returns no secrets."""
    devices={}
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1)
        s.bind(('0.0.0.0',0))
        s.sendto(DISCOVERY_REQUEST,('255.255.255.255',DISCOVERY_PORT))
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            s.settimeout(max(.01,deadline-time.monotonic()))
            try: message,(host,_)=s.recvfrom(256)
            except socket.timeout:break
            try:
                tag,ident,paired,window=message.decode('ascii').split('|')
                if tag!='JBOX-HERE' or len(ident)!=12:continue
                int(ident,16)
                if paired not in ('0','1') or window not in ('0','1'):continue
            except (ValueError,UnicodeDecodeError):continue
            devices[ident]=dict(id=ident,host=host,paired=paired=='1',
                                pairing_open=window=='1')
    return list(devices.values())


def _config_path()->Path:
    parent=Path(os.getenv('JBOX_CONFIG_DIR',str(Path.home()/'.jbox')))
    return parent/'keys.json'


def _read_config()->dict:
    path=_config_path()
    if not path.is_file():return {'devices':{},'aliases':{}}
    with path.open('r',encoding='utf-8') as f:result=json.load(f)
    if not isinstance(result,dict):raise ValueError('invalid JBOX key store')
    result.setdefault('devices',{});result.setdefault('aliases',{})
    return result


def _save_key(ident:str,host:str,key:bytes)->None:
    if len(key)!=32:raise ValueError('invalid device key length')
    path=_config_path();path.parent.mkdir(parents=True,exist_ok=True)
    config=_read_config()
    config['devices'][ident]=key.hex()
    config['aliases'][host]=ident
    # Restrict POSIX file mode from the first creation. Windows stores it in
    # the user's home folder; use OS account file ACLs as appropriate.
    tmp=path.with_suffix('.tmp')
    fd=os.open(tmp,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600)
    try:
        with os.fdopen(fd,'w',encoding='utf-8') as f:
            json.dump(config,f,indent=2)
        os.replace(tmp,path)
    finally:
        if tmp.exists():tmp.unlink()
    if os.name=='posix':path.chmod(0o600)


def _load_key(host:str,device_id:str|None=None)->str|None:
    config=_read_config()
    ident=device_id or config['aliases'].get(host,host)
    return config['devices'].get(ident)


def _auto_key(host:str, device_id:str|None=None)->str:
    """Reuse a saved binding, or claim a fresh one in a button-open window."""
    cached=_load_key(host,device_id)
    if cached:return cached
    # Find device identity; this allows DHCP IP changes without re-pairing.
    device=next((d for d in discover_devices(timeout=1.0) if d['host']==host),None)
    if device is not None:
        cached=_load_key(host,device['id'])
        if cached:
            _save_key(device['id'],host,bytes.fromhex(cached))
            return cached
        if device['pairing_open']:
            pair_device(host,device['id'])
            cached=_load_key(host,device['id'])
            if cached:return cached
    raise ValueError('device not paired: short-press GPIO10, then retry within 120 seconds')

def _read_exact(fd:socket.socket,n:int)->bytes:
    data=bytearray()
    while len(data)<n:
        chunk=fd.recv(n-len(data))
        if not chunk:raise EOFError('connection closed')
        data.extend(chunk)
    return bytes(data)


def pair_device(host:str,device_id:str|None=None,port:int=PORT)->dict:
    """One-time physically authorized (button) key claim; key is never printed.

    Warning: pairing sends the device key over LAN without TLS. Physical
    window and first-claim gate prevent permanent unauthenticated access but
    do not protect against malicious peers on the same LAN during pairing.
    """
    with socket.create_connection((host,port),timeout=5) as fd:
        fd.settimeout(5)
        header=HEADER.pack(MAGIC,1,CMD_PAIR,0,1,0)
        fd.sendall(header)
        resp=_read_exact(fd,16)
        magic,version,cmd,flags,seq,size=HEADER.unpack(resp)
        if (magic!=MAGIC or version!=1 or cmd!=CMD_PAIR or
            flags!=F_REPLY or seq!=1 or size>64 or size<4):
            raise IOError('invalid pairing response')
        body=_read_exact(fd,size)
    status=struct.unpack_from('<i',body)[0]
    if status:raise APIError(status,CMD_PAIR)
    key=body[4:]
    if len(key)!=32:raise IOError('invalid paired key size')
    ident=device_id or host
    _save_key(ident,host,key)
    return dict(host=host,id=ident,stored_at=str(_config_path()))

class APIError(Exception):
    def __init__(self, status: int, command: int):
        super().__init__(f'firmware returned {status} to command 0x{command:02x}')
        self.status=status;self.command=command

class MatrixClient:
    def __init__(self, host: str, key_hex: str|None=None, port: int=PORT, timeout: float=5.0):
        key_hex=key_hex or os.getenv('JBOX_KEY') or _auto_key(host)
        if not key_hex:raise ValueError('not paired: physically press button and run jbox_api.py pair')
        if len(key_hex.strip())!=64:raise ValueError('invalid device key (expected 64 hex digits)')
        self.key=bytes.fromhex(key_hex.strip())
        self.host=host;self.port=port;self.timeout=timeout
        self.socket:socket.socket|None=None
        self.session_key:bytes|None=None
        self._seq=0
        self._lock=threading.Lock()

    def __enter__(self):self.connect();return self
    def __exit__(self,*_):self.close()
    def connect(self):
        if self.socket:self.close()
        self.socket=socket.create_connection((self.host,self.port),self.timeout)
        self.socket.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        self.socket.settimeout(self.timeout)
        self._seq=0;self.session_key=None
        nonce=self._rpc(CMD_HELLO)
        if len(nonce)!=16:raise IOError('invalid HELLO response')
        proof=hmac.new(self.key,nonce+b'JMX-AUTH-v1',hashlib.sha256).digest()
        self.session_key=hmac.new(self.key,nonce+b'JMX-SESSION-v1',hashlib.sha256).digest()
        try:self._rpc(CMD_AUTH,proof,unauth=True)
        except Exception:
            self.close();raise
        return self

    def close(self):
        if self.socket:
            try:self.socket.shutdown(socket.SHUT_RDWR)
            except OSError:pass
            self.socket.close();self.socket=None
        self.session_key=None

    def _read_n(self,n:int)->bytes:
        result=bytearray()
        assert self.socket is not None
        while len(result)<n:
            chunk=self.socket.recv(n-len(result))
            if not chunk:raise EOFError('device disconnected')
            result.extend(chunk)
        return bytes(result)

    def _rpc(self,cmd:int,data:bytes=b'',unauth:bool=False)->bytes:
        if self.socket is None:raise ConnectionError('not connected')
        if len(data)>MAX_PAYLOAD:raise ValueError('request too large')
        with self._lock:
            self._seq+=1;seq=self._seq
            # AUTH requests are unsigned, but replies are signed with session_key.
            signing=(not unauth and cmd!=CMD_HELLO)
            flags=F_MAC if signing else 0
            h=HEADER.pack(MAGIC,1,cmd,flags,seq,len(data))
            tag=hmac.new(self.session_key,h+data,hashlib.sha256).digest()[:16] if signing else b''
            self.socket.sendall(h+data+tag)
            header=self._read_n(16)
            magic,version,reply_cmd,rflags,reply_seq,length=HEADER.unpack(header)
            if (magic!=MAGIC or version!=1 or reply_cmd!=cmd or reply_seq!=seq or
                not (rflags&F_REPLY) or length>MAX_PAYLOAD or length<4):
                raise IOError('malformed or mismatched response')
            body=self._read_n(length)
            if rflags&F_MAC:
                expected=hmac.new(self.session_key,header+body,hashlib.sha256).digest()[:16]
                if not hmac.compare_digest(self._read_n(16),expected):
                    raise IOError('response HMAC mismatch')
            elif signing or cmd==CMD_AUTH:
                raise IOError('missing authenticated response')
            status=struct.unpack_from('<i',body)[0]
            if status:raise APIError(status,cmd)
            return body[4:]

    def caps(self)->dict:
        r=self._rpc(CMD_CAPS)
        version,nodes,nets,ios,max_xfer,port=struct.unpack_from('<BBBBHH',r)
        return dict(version=version,nodes=nodes,nets=nets,io_count=ios,
                    max_xfer=max_xfer,port=port,io_gpios=list(r[8:8+ios]))

    def status(self)->dict:
        r=self._rpc(CMD_STATUS)
        ms,heap=struct.unpack_from('<II',r)
        nets=list(struct.unpack_from('<8Q',r,8))
        n=self.caps()['io_count']
        mode=list(r[72:72+n]);lease=list(r[72+n:72+2*n])
        return dict(uptime_ms=ms,heap_free=heap,
                    nets=[[i for i in range(52) if (mask>>i)&1] for mask in nets],
                    io_mode=mode,io_lease=lease)

    def route_set(self,nets:Sequence[Sequence[int]])->None:
        if len(nets)>8:raise ValueError('at most 8 nets')
        masks=[];seen=set()
        for net in nets:
            mask=0
            for n in net:
                if not 0<=n<52 or n in seen:raise ValueError('invalid or duplicated route node')
                seen.add(n);mask|=1<<n
            masks.append(mask)
        self._rpc(CMD_ROUTE_SET,struct.pack('<8Q',*(masks+[0]*(8-len(masks)))))

    def connect_nodes(self,a:int,b:int)->int:
        if not(0<=a<52 and 0<=b<52):raise ValueError('route node 0..51')
        r=self._rpc(CMD_ROUTE_CONNECT,bytes((a,b)))
        return r[0]

    def remove_node(self,n:int)->None:
        self._rpc(CMD_ROUTE_REMOVE,bytes([n]))

    def route_clear(self)->None:self._rpc(CMD_ROUTE_CLEAR)
    def safe(self)->None:self._rpc(CMD_SAFE)

    def upload_function(self, source: str, slot: int=0):
        """Compile and upload JBoxScript; return a callable remote function."""
        try:
            from .jbox_lang import compile_function, RemoteFunction
        except ImportError:
            from jbox_lang import compile_function, RemoteFunction
        if type(slot) is not int or not 0<=slot<4:raise ValueError('function slot 0..3')
        compiled=compile_function(source)
        payload=struct.pack('<BBH',slot,len(compiled.parameters),len(compiled.code)//4)+compiled.code
        self._rpc(CMD_FUNCTION_LOAD,payload)
        return RemoteFunction(self,slot,compiled)

    def call_function(self, slot: int, io_args: Sequence[int])->list[int]:
        if type(slot) is not int or not 0<=slot<4:raise ValueError('function slot 0..3')
        if len(io_args)>4 or any(type(io) is not int or not 0<=io<18 for io in io_args):
            raise ValueError('function GPIO arguments must be API IO IDs 0..17')
        if len(set(io_args))!=len(io_args):raise ValueError('function GPIO arguments must be distinct')
        response=self._rpc(CMD_FUNCTION_CALL,bytes([slot,len(io_args),*io_args]))
        if len(response)<2:raise IOError('truncated function result')
        count=struct.unpack_from('<H',response)[0]
        if count>128 or len(response)!=2+count*4:raise IOError('invalid function result length')
        return list(struct.unpack_from(f'<{count}I',response,2))

    def clear_function(self, slot: int=0)->None:
        if type(slot) is not int or not 0<=slot<4:raise ValueError('function slot 0..3')
        self._rpc(CMD_FUNCTION_CLEAR,bytes([slot]))

    def gpio_config(self,io:int,mode:int=0,pull:int=0,initial:int=0)->None:
        """mode 0=input, 1=output, 2=open drain, 3=ADC; pull 0=none 1=up 2=down."""
        self._rpc(CMD_IO_CONFIG,struct.pack('<BBBB',io,mode,pull,initial))

    def gpio_write(self,values:dict[int,int])->None:
        mask=0;bits=0
        for io,value in values.items():
            if not 0<=io<18 or value not in (0,1):raise ValueError('IO 0..17 / level 0 or 1')
            mask|=1<<io
            if value:bits|=1<<io
        self._rpc(CMD_IO_WRITE,struct.pack('<II',mask,bits))

    def gpio_read(self,io_ids:Sequence[int]=tuple(range(18)))->dict[int,int]:
        mask=0
        for x in io_ids:
            if not 0<=x<18:raise ValueError('IO id 0..17')
            mask|=1<<x
        (values,)=struct.unpack('<I',self._rpc(CMD_IO_READ,struct.pack('<I',mask)))
        return {x:(values>>x)&1 for x in io_ids}

    def adc_read(self,io:int)->int:
        return struct.unpack('<I',self._rpc(CMD_ADC,bytes([io])))[0]

    def pwm(self,channel:int,io:int,frequency:int=1000,duty:int=4096):
        """4 PWM channels 0..3; duty 0..8191; io=255 disables."""
        self._rpc(CMD_PWM,struct.pack('<BBIH',channel,io,frequency,duty))

    def i2c_setup(self,bus:int,sda:int,scl:int,hz:int=50000):
        self._rpc(CMD_I2C_SETUP,struct.pack('<BBBI',bus,sda,scl,hz))
    def i2c_close(self,bus:int):self._rpc(CMD_I2C_CLOSE,bytes([bus]))
    def i2c_xfer(self,bus:int,address:int,write:bytes=b'',read:int=0)->bytes:
        if not 0<=address<128 or len(write)+read>4096:raise ValueError('I2C transfer size/address')
        return self._rpc(CMD_I2C_XFER,struct.pack('<BBHH',bus,address,len(write),read)+write)
    def i2c_scan(self,bus:int)->list[int]:
        found=[]
        for addr in range(8,120):
            try:self.i2c_xfer(bus,addr)
            except APIError:continue
            found.append(addr)
        return found

    def spi_setup(self,bus:int,mosi:int,miso:int,sck:int,cs:int,
                  mode:int=0,hz:int=100000):
        """Use miso=255 if device is write-only."""
        self._rpc(CMD_SPI_SETUP,struct.pack('<BBBBBBI',bus,mosi,miso,sck,cs,mode,hz))
    def spi_xfer(self,bus:int,data:bytes)->bytes:
        if not 1<=len(data)<=4096:raise ValueError('SPI transfer max 4096')
        return self._rpc(CMD_SPI_XFER,struct.pack('<BH',bus,len(data))+data)
    def spi_close(self,bus:int):self._rpc(CMD_SPI_CLOSE,bytes([bus]))

    def uart_setup(self,bus:int,tx:int,rx:int,baud:int=115200,
                   parity:int=0,stopbits:int=1):
        self._rpc(CMD_UART_SETUP,struct.pack('<BBBIBB',bus,tx,rx,baud,parity,stopbits))
    def uart_write(self,bus:int,data:bytes)->int:
        if len(data)>4096:raise ValueError('UART write max 4096')
        return struct.unpack('<H',self._rpc(CMD_UART_WRITE,bytes([bus])+data))[0]
    def uart_read(self,bus:int,count:int=512,timeout_ms:int=100)->bytes:
        if not 0<=count<=4096 or not 0<=timeout_ms<=500:raise ValueError('UART read arguments')
        return self._rpc(CMD_UART_READ,struct.pack('<BHH',bus,count,timeout_ms))
    def uart_close(self,bus:int):self._rpc(CMD_UART_CLOSE,bytes([bus]))
    def factory_reset(self):
        """Erase Wi-Fi and API keys, reboot; tap physical button to start BLE."""
        self._rpc(CMD_FACTORY_RESET,b'YES!')


def _cli():
    p=argparse.ArgumentParser(description='JBOX-S3 API: discover and pair by physical button (no codes)')
    p.add_argument('--host',help='optional device IP; otherwise auto-discover one LAN device')
    p.add_argument('--port',type=int,default=PORT)
    p.add_argument('--key',help='optional advanced override; normally loaded automatically')
    sub=p.add_subparsers(dest='cmd',required=True)
    sub.add_parser('discover');sub.add_parser('pair')
    sub.add_parser('info');sub.add_parser('status');sub.add_parser('clear');sub.add_parser('safe')
    co=sub.add_parser('connect');co.add_argument('a',type=int);co.add_argument('b',type=int)
    cl=sub.add_parser('gpio-read');cl.add_argument('ids',nargs='*',type=int)
    w=sub.add_parser('gpio-write');w.add_argument('io',type=int);w.add_argument('value',type=int)
    ic=sub.add_parser('gpio-config');ic.add_argument('io',type=int);ic.add_argument('mode',type=int)
    sc=sub.add_parser('i2c-scan');sc.add_argument('sda',type=int);sc.add_argument('scl',type=int)
    args=p.parse_args()
    discovered=discover_devices()
    if args.cmd=='discover':
        print(json.dumps(discovered,indent=2,ensure_ascii=False));return
    if args.host:
        matches=[d for d in discovered if d['host']==args.host]
        device=matches[0] if matches else {'host':args.host,'id':args.host}
    else:
        if not discovered:p.error('no LAN device discovered; verify Wi-Fi and LAN broadcasts')
        if len(discovered)>1:p.error('multiple devices found, please specify --host IP')
        device=discovered[0]
    host=device['host']
    if args.cmd=='pair':
        result=pair_device(host,device.get('id'),args.port)
        print(json.dumps(dict(paired=True,**result),indent=2));return
    key=args.key or os.getenv('JBOX_KEY') or _auto_key(host,device.get('id'))
    with MatrixClient(host,key,args.port) as dev:
        if args.cmd=='info':result=dev.caps()
        elif args.cmd=='status':result=dev.status()
        elif args.cmd=='connect':result={'net':dev.connect_nodes(args.a,args.b)}
        elif args.cmd=='clear':dev.route_clear();result={'ok':True}
        elif args.cmd=='safe':dev.safe();result={'ok':True}
        elif args.cmd=='gpio-config':dev.gpio_config(args.io,args.mode);result={'ok':True}
        elif args.cmd=='gpio-write':dev.gpio_write({args.io:args.value});result={'ok':True}
        elif args.cmd=='gpio-read':result=dev.gpio_read(args.ids or range(18))
        elif args.cmd=='i2c-scan':
            dev.i2c_setup(0,args.sda,args.scl)
            try:result={'addresses':dev.i2c_scan(0)}
            finally:dev.i2c_close(0)
        else:raise ValueError(args.cmd)
        print(json.dumps(result,indent=2,ensure_ascii=False))

if __name__=='__main__':_cli()
