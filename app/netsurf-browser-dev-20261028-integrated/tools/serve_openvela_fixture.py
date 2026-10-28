#!/usr/bin/env python3
"""Serve only browser test artifacts and a deterministic webpage on the LAN."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler,ThreadingHTTPServer
import zlib,struct,gzip,time
from urllib.parse import urlsplit,parse_qs
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--directory',required=True)
p.add_argument('--bind',required=True)
p.add_argument('--port',type=int,default=8765)
a=p.parse_args()
def chunk(kind,data):return struct.pack('!I',len(data))+kind+data+struct.pack('!I',zlib.crc32(kind+data))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',2,2,8,2,0,0,0))+chunk(b'IDAT',zlib.compress((b'\x00'+b'\x00\x80\xff'*2)*2))+chunk(b'IEND',b'')
page='''<!doctype html><meta charset="utf-8"><title>P4 local page</title>
<style>body{font-family:sans-serif;background:#ecf5ff;color:#173454;margin:24px}h1{font-size:32px}button{padding:16px}p{line-height:1.6}</style>
<h1>ESP32-P4 独立浏览器</h1><p id="result">HTML、CSS、中文、图片和脚本测试</p>
<img src="/fixture/pixel.png" width="80" height="80"><button onclick="document.title='P4 touch passed'">触摸测试</button>
<p><a href="/fixture/next.html">下一页 / navigation</a></p>
<script>fetch('/fixture/value').then(function(r){return r.text()}).then(function(t){document.getElementById('result').textContent=t;document.title='P4 JS fetch passed';});</script>
'''+''.join('<p>滚动测试第 %d 行 — Independent native browser on openvela.</p>'%i for i in range(1,51))
class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed=urlsplit(self.path)
        if parsed.path=='/fixture/stall-body':
            # Purposefully stalled local peer for the real App Stop button.
            self.send_response(200);self.send_header('Content-Length','53')
            self.send_header('Content-Type','text/html');self.end_headers()
            time.sleep(90);return
        if parsed.path=='/fixture/form-result':
            passed=parse_qs(parsed.query,keep_blank_values=True).get('q')==['P4test']
            data=('<title>P4 form submit '+('passed' if passed else 'unexpected')+'</title><h1>GET form received</h1>').encode()
            self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8')
            self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data);return
        aliases={'/a':'/com.netsurf.browser/app.js','/m':'/com.netsurf.browser/manifest.json',
                 '/n':'/com.netsurf.browser/native/host.abi','/e':'/com.netsurf.browser/native/browser.elf',
                 '/ca':'/ca-bundle.pem'}
        if self.path in aliases:self.path=aliases[self.path]
        compressed={
            '/fixture/gzip.html':('gzip',gzip.compress(b'<meta charset="utf-8"><title>P4 gzip passed</title><h1>Gzip HTML passed</h1><script>fetch("/fixture/deflate-value").then(function(r){return r.text()}).then(function(t){document.title=t;});</script>')),
            '/fixture/deflate-value':('deflate',zlib.compress(b'P4 gzip + deflate fetch passed')),
            '/fixture/deflate.html':('deflate',zlib.compress(b'<title>P4 deflate passed</title><h1>Deflate HTML passed</h1>')),
        }
        if self.path in compressed:
            coding,data=compressed[self.path]
            self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8')
            self.send_header('Content-Length',str(len(data)));self.send_header('Content-Encoding',coding)
            self.end_headers();self.wfile.write(data);return
        pages={'/fixture/index.html':('text/html; charset=utf-8',page.encode()),
               '/fixture/form.html':('text/html; charset=utf-8',b'''<title>P4 form ready</title><body style="margin:0;background:white"><form action="/fixture/form-result" method="get">
<input id="q" name="q" maxlength="32" style="position:absolute;left:20px;top:20px;width:400px;height:50px" oninput="document.title='P4 input '+this.value" onchange="document.title='P4 change '+this.value">
<input type="password" value="fixture-only" style="position:absolute;left:20px;top:100px;width:400px;height:50px">
<button type="submit" style="position:absolute;left:20px;top:190px;width:200px;height:60px">Submit form</button>
<a href="/fixture/gzip.html" style="position:absolute;left:500px;top:20px">Compressed page</a></form></body>'''),
               '/fixture/next.html':('text/html; charset=utf-8',b'<title>P4 next page</title><h1>Navigation passed</h1>'),
               '/fixture/value':('text/plain; charset=utf-8','网页 JavaScript 和 fetch 已在设备运行'.encode()),
               '/fixture/pixel.png':('image/png',png)}
        if self.path in pages:
            mime,data=pages[self.path];self.send_response(200);self.send_header('Content-Type',mime)
            self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        else:super().do_GET()
print('Fixture server ready',a.bind,a.port,flush=True)
ThreadingHTTPServer((a.bind,a.port),partial(Handler,directory=a.directory)).serve_forever()
