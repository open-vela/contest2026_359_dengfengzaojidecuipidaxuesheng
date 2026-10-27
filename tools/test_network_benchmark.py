#!/usr/bin/env python3
"""Exercise real HTTP transfers, corruption, framing and forged result guards."""

import http.client
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import zlib

from network_benchmark import BenchServer, BenchState, PAYLOAD_BYTES


class BenchmarkTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.state = BenchState(self.tmp.name, duration=.001, warmup=0, repeats=1)
        self.server = BenchServer(('127.0.0.1', 0), self.state, ['127.0.0.1'])
        self.worker = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.worker.start()
        self.run = self.stage = ''

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.worker.join()
        self.tmp.cleanup()

    def request(self, method, path, data=None, headers=None):
        conn = http.client.HTTPConnection(*self.server.server_address, timeout=5)
        extra = {'X-Bench-Key': self.state.key, 'X-Bench-Run': self.run, 'X-Bench-Stage': self.stage}
        extra.update(headers or {})
        if isinstance(data, dict):
            data = json.dumps(data).encode()
            extra['Content-Type'] = 'application/json'
        conn.request(method, path, body=data, headers=extra)
        reply = conn.getresponse()
        body = reply.read()
        result = body if reply.getheader('Content-Type') == 'application/octet-stream' else json.loads(body)
        status = reply.status
        conn.close()
        return status, result

    def begin(self):
        status, result = self.request('POST', '/begin', {})
        self.assertEqual(status, 200)
        self.run = result['run']

    def start(self, direction):
        status, result = self.request('POST', '/stage', dict(direction=direction))
        self.assertEqual(status, 200)
        self.stage = result['id']
        self.began = time.monotonic()

    def finish(self, *, bad_bytes=None):
        stage = self.state.runs[self.run]['current']
        return self.request('POST', '/finish', dict(bytes=stage['bytes'] if bad_bytes is None else bad_bytes,
            requests=stage['requests'], elapsed_ms=(time.monotonic()-self.began)*1000,
            check_ms=0, crc_errors=0, rtt_ms=[1, 2, 3, 10], ping_failures=0, ping_cancelled=0))

    def test_full_bidirectional_result_is_linked_to_server_bytes(self):
        self.begin()
        status, payload = self.request('GET', '/seed')
        self.assertEqual(status, 200)
        self.assertEqual(len(payload), PAYLOAD_BYTES)
        self.assertEqual(f'{zlib.crc32(payload):08x}', self.state.crc)
        self.assertEqual(self.request('GET', '/seed')[0], 409)
        for direction in ['download', 'download', 'upload', 'upload']:
            self.start(direction)
            if direction == 'download':
                status, body = self.request('GET', '/payload')
                self.assertEqual(status, 200)
                self.assertEqual(body, payload)
            else:
                status, body = self.request('POST', '/upload', payload)
                self.assertEqual((status, body['bytes'], body['crc32']), (200, PAYLOAD_BYTES, self.state.crc))
            self.assertEqual(self.finish(bad_bytes=1)[0], 422)
            status, result = self.finish()
            self.assertEqual(status, 200)
            self.assertGreater(result['mbps'], 0)
            self.assertEqual(result['rtt_p95'], 10)
        saved = json.loads(next(Path(self.tmp.name).glob('run-*.json')).read_text())
        self.assertTrue(saved['complete'])
        self.assertEqual(len(saved['stages']), 4)
        self.assertNotIn(self.state.key, json.dumps(saved))
        self.assertTrue(all(s['bytes'] == PAYLOAD_BYTES and s['crc_errors'] == 0 for s in saved['stages']))

    def test_corruption_never_produces_a_successful_stage(self):
        self.begin()
        for _ in range(2):
            self.start('download')
            self.assertEqual(self.request('GET', '/payload')[0], 200)
            self.assertEqual(self.finish()[0], 200)
        self.start('upload')
        self.assertEqual(self.request('POST', '/upload', bytes(PAYLOAD_BYTES))[0], 422)
        self.assertEqual(self.finish()[0], 422)
        self.assertEqual(self.request('POST', '/abort', {})[0], 200)
        saved = json.loads(next(Path(self.tmp.name).glob('run-*.json')).read_text())
        self.assertFalse(saved['complete'])
        self.assertTrue(saved['aborted'])

    def test_access_and_framing_guards(self):
        self.assertEqual(self.request('POST', '/begin', {}, {'X-Bench-Key': 'wrong'})[0], 403)
        self.assertEqual(self.request('GET', '/ping', headers={'Origin': 'http://unrelated.example'})[0], 403)
        self.assertEqual(self.request('GET', '/ping', headers={'Host': 'unrelated.example'})[0], 403)
        self.assertEqual(self.request('GET', '/ping?x=1')[0], 400)
        self.begin()
        self.assertEqual(self.request('POST', '/stage', dict(direction='upload'))[0], 409)
        self.start('download')
        self.assertEqual(self.request('POST', '/upload', b'wrong')[0], 400)
        self.assertEqual(self.request('GET', '/payload', headers={'X-Bench-Stage':'stale'})[0], 409)
        self.assertEqual(self.request('POST', '/stage', dict(direction='download'))[0], 409)
        self.assertEqual(self.finish()[0], 422)
        self.assertEqual(self.request('POST', '/abort', {})[0], 200)
        self.assertEqual(self.request('GET', '/seed')[0], 409)

    def test_oversized_and_duplicate_content_length_are_rejected_before_read(self):
        for lengths, expected in [('Content-Length: 999999999\r\n', 413),
                                  ('Content-Length: 0\r\nContent-Length: 1\r\n', 411),
                                  ('Transfer-Encoding: chunked\r\n', 411)]:
            with socket.create_connection(self.server.server_address, timeout=3) as conn:
                host, port = self.server.server_address
                conn.sendall((f'POST /begin HTTP/1.1\r\nHost: {host}:{port}\r\n'
                              f'X-Bench-Key: {self.state.key}\r\n{lengths}\r\n').encode())
                self.assertIn(f' {expected} '.encode(), conn.recv(1024))

    def test_slow_progressing_reader_does_not_hit_whole_payload_socket_timeout(self):
        original_setup = self.server.RequestHandlerClass.setup

        def short_timeout(handler):
            original_setup(handler)
            handler.connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
            handler.connection.settimeout(.2)

        self.server.RequestHandlerClass.setup = short_timeout
        try:
            self.begin()
            self.start('download')
            conn = http.client.HTTPConnection(*self.server.server_address, timeout=5)
            conn.connect()
            conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32768)
            conn.request('GET', '/payload', headers={'X-Bench-Key': self.state.key,
                'X-Bench-Run': self.run, 'X-Bench-Stage': self.stage})
            response = conn.getresponse()
            total, crc, began = 0, 0, time.monotonic()
            while True:
                chunk = response.read(16384)
                if not chunk:
                    break
                total += len(chunk)
                crc = zlib.crc32(chunk, crc)
                time.sleep(.003)
            self.assertGreater(time.monotonic() - began, .2)
            self.assertEqual(total, PAYLOAD_BYTES)
            self.assertEqual(f'{crc:08x}', self.state.crc)
            conn.close()
            self.assertEqual(self.finish()[0], 200)
        finally:
            self.server.RequestHandlerClass.setup = original_setup

    def test_slow_control_exchange_cannot_inflate_rate_or_reject_valid_data(self):
        self.begin()
        self.start('download')
        self.assertEqual(self.request('GET', '/payload')[0], 200)
        # Model a slow control reply before the browser started its clock.
        self.state.runs[self.run]['current']['started'] -= 6
        status, result = self.finish()
        self.assertEqual(status, 200)
        stored = self.state.runs[self.run]['current']
        self.assertGreater(stored['rate_elapsed_ms'], stored['elapsed_ms'] + 5000)
        self.assertLessEqual(result['mbps'], PAYLOAD_BYTES * 8 / 6 / 1e6)

    def test_checksum_diagnostic_retains_offsets_and_never_payload(self):
        self.begin()
        status, _ = self.request('POST', '/abort', dict(reason='checksum', diagnostic={
            'expected_bytes': PAYLOAD_BYTES, 'actual_bytes': PAYLOAD_BYTES,
            'expected_crc32': '1234abcd', 'actual_crc32': '5678abcd',
            'first_mismatch': 512, 'last_mismatch': 1500, 'mismatched_bytes': 2,
            'phase': 'seed', 'comparison_available': False,
            'first_mismatch_offsets': [512,1500], 'payload': 'must-not-be-saved'}))
        self.assertEqual(status, 200)
        saved = json.loads(next(Path(self.tmp.name).glob('run-*.json')).read_text())
        self.assertEqual(saved['abort_reason'], 'checksum')
        self.assertEqual(saved['checksum_diagnostic']['first_mismatch_offsets'], [512,1500])
        self.assertEqual(saved['checksum_diagnostic']['phase'], 'seed')
        self.assertIs(saved['checksum_diagnostic']['comparison_available'], False)
        self.assertNotIn('must-not-be-saved', json.dumps(saved))

    def test_actual_browser_crc_and_offset_checker_with_node(self):
        if not shutil.which('node'):
            self.skipTest('Node is required for the actual browser CRC/offset functions')
        html = Path(__file__).with_name('network_benchmark.html').read_text(encoding='utf-8')
        functions = html[html.index('const table ='):html.index('const pause =')]
        payload = bytes((i*31+7)%256 for i in range(8192))
        cfg = json.dumps(dict(bytes=len(payload), crc32=f'{zlib.crc32(payload):08x}'))
        script = 'const assert=require("node:assert/strict"); const cfg='+cfg+';\n'+functions+r'''
const seed=new Uint8Array(8192);for(let i=0;i<seed.length;i++)seed[i]=(i*31+7)%256;
verifyDownload(seed.buffer.slice(0),seed.buffer);
const bad=seed.slice();for(const i of [511,512,1500,4096])bad[i]^=1;
let caught;try{verifyDownload(bad.buffer,seed.buffer);}catch(e){caught=e.benchDiagnostic;}
assert.equal(caught.mismatched_bytes,4);assert.equal(caught.first_mismatch,511);
assert.equal(caught.last_mismatch,4096);assert.deepEqual(caught.first_mismatch_offsets,[511,512,1500,4096]);
assert.notEqual(caught.actual_crc32,cfg.crc32);
try{verifyDownload(seed.buffer.slice(0,8191),seed.buffer);assert.fail();}
catch(e){assert.equal(e.benchDiagnostic.actual_bytes,8191);
assert.equal(e.benchDiagnostic.first_mismatch,8191);assert.equal(e.benchDiagnostic.mismatched_bytes,1);}
const extra=new Uint8Array(8194);extra.set(seed);
try{verifyDownload(extra.buffer,seed.buffer);assert.fail();}
catch(e){assert.equal(e.benchDiagnostic.last_mismatch,8193);assert.equal(e.benchDiagnostic.mismatched_bytes,2);}
verifyDownload(seed.buffer,null,'seed');
try{verifyDownload(bad.buffer,null,'seed');assert.fail();}
catch(e){assert.equal(e.benchDiagnostic.phase,'seed');assert.equal(e.benchDiagnostic.comparison_available,false);
assert.equal(e.benchDiagnostic.first_mismatch,-1);assert.equal(e.benchDiagnostic.mismatched_bytes,null);
assert.notEqual(e.benchDiagnostic.actual_crc32,cfg.crc32);}
console.log('BROWSER_CRC_OFFSET_OK');
'''
        tested = subprocess.run(['node'], input=script, text=True, capture_output=True, timeout=10)
        self.assertEqual(tested.returncode, 0, tested.stderr)
        self.assertIn('BROWSER_CRC_OFFSET_OK', tested.stdout)

    def test_download_only_finishes_without_upload_and_does_not_allow_extra_stages(self):
        self.state.directions=('download',)
        self.state.plan=[('download',True),('download',False)]
        self.begin()
        for _ in range(2):
            self.start('download')
            self.assertEqual(self.request('GET','/payload')[0],200)
            self.assertEqual(self.finish()[0],200)
        self.assertTrue(self.state.runs[self.run]['complete'])
        self.assertEqual(self.request('POST','/stage',dict(direction='upload'))[0],409)


if __name__ == '__main__':
    unittest.main(verbosity=2)
