#!/usr/bin/env python3
"""Compare direct (no proxy) IPv4/TLS paths to a public test site."""
import argparse,concurrent.futures,http.client,json,socket,ssl,time,ipaddress,struct
from pathlib import Path
from urllib.parse import urlsplit
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--url',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--address',action='append',help='known public IPv4 from the device/DNS; SNI and verification keep the URL hostname')
p.add_argument('--gateway',help='Windows only: select the physical outgoing interface used to reach this LAN gateway, per socket')
a=p.parse_args(); url=urlsplit(a.url)
if url.scheme!='https' or not url.hostname or url.username or url.password:p.error('public HTTPS URL required')
addresses=sorted(set(a.address or [item[4][0] for item in socket.getaddrinfo(url.hostname,url.port or 443,socket.AF_INET,socket.SOCK_STREAM)]))
for address in addresses:ipaddress.IPv4Address(address)
interface=None
if a.gateway:
    import ctypes
    index=ctypes.c_uint32()
    get_best=ctypes.WinDLL('iphlpapi').GetBestInterface
    get_best.argtypes=[ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32)]
    get_best.restype=ctypes.c_uint32
    error=get_best(int.from_bytes(socket.inet_aton(a.gateway),'little'),ctypes.byref(index))
    if error:raise OSError(error,'GetBestInterface failed')
    interface=index.value
def probe(item):
    address,version=item;started=time.monotonic(); result={'address':address,'tls':version}
    try:
        context=ssl.create_default_context()
        if version=='1.2':context.minimum_version=context.maximum_version=ssl.TLSVersion.TLSv1_2
        with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as raw:
            raw.settimeout(15)
            if interface:
                # Windows IP_UNICAST_IF uses a network-byte-order interface index.
                # This affects this socket only; never modify global routes/proxy.
                raw.setsockopt(socket.IPPROTO_IP,31,struct.pack('!I',interface))
            raw.connect((address,url.port or 443))
            result['local_address']=raw.getsockname()[0]
            result['tcp_ms']=round((time.monotonic()-started)*1000)
            with context.wrap_socket(raw,server_hostname=url.hostname) as tls:
                result['negotiated_tls']=tls.version();result['handshake_ms']=round((time.monotonic()-started)*1000)
                request='GET '+(url.path or '/')+' HTTP/1.1\r\nHost: '+url.hostname+'\r\nConnection: close\r\n\r\n'
                tls.sendall(request.encode('ascii'))
                response=http.client.HTTPResponse(tls);response.begin();result['status']=response.status
                result['server']=response.getheader('Server');result['bytes_read']=len(response.read(2048))
    except Exception as error:result['error']=type(error).__name__+': '+str(error)
    result['elapsed_ms']=round((time.monotonic()-started)*1000);return result
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    results=list(pool.map(probe,[(address,version) for address in addresses for version in ['default','1.2']]))
report={'url':a.url,'http_proxy_used':False,'outgoing_interface':interface,'ipv4':addresses,'results':results}
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
