#!/usr/bin/env python3
"""Bounded LAN HTTP benchmark. Payload lives in RAM; only metadata is saved."""

import argparse
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import secrets
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit
import zlib

PAYLOAD_BYTES = 4 * 1024 * 1024
JSON_LIMIT = 32768


class BenchError(Exception):
    def __init__(self, status, message):
        self.status, self.message = status, message


class BenchState:
    def __init__(self, output, *, duration=60, warmup=10, repeats=3, lifetime=2400, context=None,
                 directions=('download','upload')):
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.payload = os.urandom(PAYLOAD_BYTES)
        self.crc = f'{zlib.crc32(self.payload):08x}'
        self.key = secrets.token_hex(16)
        self.duration, self.warmup, self.repeats = duration, warmup, repeats
        self.started, self.lifetime = time.monotonic(), lifetime
        self.lock = threading.Lock()
        self.runs = {}
        self.context = context or {}
        self.directions = tuple(directions)
        if self.directions not in (('download',),('download','upload')):
            raise ValueError('unsupported directions')
        self.plan = [(direction,index==0) for direction in self.directions for index in range(repeats+1)]
        self.html = Path(__file__).with_suffix('.html').read_text(encoding='utf-8')
        self.source_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in [Path(__file__), Path(__file__).with_suffix('.html')]}

    def save(self, run_id):
        # Caller holds lock; bounded metadata only, never payload or page key.
        run = self.runs[run_id]
        data = {k: v for k, v in run.items() if k not in ('current',)}
        data['stages'] = [{k: v for k, v in s.items() if k != 'started'}
                          for s in run['stages']]
        data.update(source_sha256=self.source_hashes,
                    measurement='LAN HTTP application throughput; not raw TCP/PHY rate',
                    payload_bytes=PAYLOAD_BYTES, payload_crc32=self.crc,
                    context=self.context,
                    directions=list(self.directions),
                    firmware_changed=False, payload_storage='RAM only',
                    complete=run.get('complete', False))
        target = self.output / f'run-{run_id}.json'
        temporary = target.with_suffix('.tmp')
        temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding='utf-8')
        temporary.replace(target)

    def run(self, token, peer):
        run = self.runs.get(token)
        if run is None or run['peer'] != peer:
            raise BenchError(403, 'unknown run')
        if run.get('aborted') or run.get('complete'):
            raise BenchError(409, 'run has ended')
        if time.monotonic() - run['started_monotonic'] > 1800:
            raise BenchError(410, 'run expired')
        return run


class BenchServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False
    request_queue_size = 8

    def __init__(self, address, state, allow):
        self.state = state
        self.allow = {str(ipaddress.ip_address(a)) for a in allow} | {address[0]}
        self.slots = threading.BoundedSemaphore(8)
        super().__init__(address, BenchHandler)

    def verify_request(self, request, address):
        return address[0] in self.allow

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()

    def handle_error(self, request, address):
        # No request/body/key dumps on broken or abandoned browser sockets.
        pass


class BenchHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'LANBench/1'

    def setup(self):
        super().setup()
        # Reading the next keep-alive request must outlast delivery of the
        # previous queued 4 MiB response on a low-rate link.
        self.connection.settimeout(120)
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, *args):
        pass

    def handle_expect_100(self):
        self.send_error(417, 'Expect not supported')
        return False

    def reply(self, status, data, content_type='application/json; charset=utf-8', extra=None):
        body = data if isinstance(data, bytes) else json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Content-Type', content_type)
        self.send_header('Cache-Control', 'no-store, no-transform')
        self.send_header('X-Content-Type-Options', 'nosniff')
        if extra:
            for key, value in extra.items():
                self.send_header(key, value)
        self.end_headers()
        # A single sendall(4 MiB) has one socket timeout for the whole write.
        # Bound each chunk instead, so a slow but progressing link is valid.
        started = time.monotonic()
        view = memoryview(body)
        previous_timeout = self.connection.gettimeout()
        try:
            for offset in range(0, len(view), 65536):
                remaining = 120 - (time.monotonic() - started)
                if remaining <= 0:
                    raise TimeoutError('response deadline')
                self.connection.settimeout(min(previous_timeout, 12, remaining))
                self.wfile.write(view[offset:offset + 65536])
        finally:
            self.connection.settimeout(previous_timeout)

    def guard(self):
        s = self.server.state
        if time.monotonic() - s.started > s.lifetime:
            raise BenchError(410, 'benchmark expired')
        expected = f'{self.server.server_address[0]}:{self.server.server_address[1]}'
        if self.headers.get('Host') != expected:
            raise BenchError(403, 'invalid Host')
        if self.headers.get('Origin') not in (None, f'http://{expected}'):
            raise BenchError(403, 'invalid Origin')
        if self.headers.get('Sec-Fetch-Site') == 'cross-site':
            raise BenchError(403, 'cross-site request')
        if urlsplit(self.path).query or urlsplit(self.path).fragment:
            raise BenchError(400, 'query not supported')
        if self.command == 'GET' and (self.headers.get('Transfer-Encoding') or
                                     self.headers.get('Content-Length') not in (None, '0')):
            raise BenchError(400, 'GET body not supported')
        if urlsplit(self.path).path != '/':
            if not secrets.compare_digest(self.headers.get('X-Bench-Key', ''), s.key):
                raise BenchError(403, 'invalid session key')
        return s

    def length(self, maximum):
        values = self.headers.get_all('Content-Length', [])
        if self.headers.get('Transfer-Encoding') or len(values) != 1:
            raise BenchError(411, 'one Content-Length required')
        if not re.fullmatch(r'[0-9]{1,10}', values[0]):
            raise BenchError(400, 'invalid length')
        size = int(values[0])
        if size > maximum:
            raise BenchError(413, 'body too large')
        return size

    def json_body(self):
        size = self.length(JSON_LIMIT)
        body = self.rfile.read(size)
        if len(body) != size:
            raise BenchError(400, 'short body')
        try:
            result = json.loads(body)
        except (ValueError, UnicodeDecodeError):
            raise BenchError(400, 'invalid JSON')
        if not isinstance(result, dict):
            raise BenchError(400, 'object required')
        return result

    def run_stage(self, state, direction):
        with state.lock:
            run = state.run(self.headers.get('X-Bench-Run', ''), self.client_address[0])
            stage = run.get('current')
            if not stage or stage['id'] != self.headers.get('X-Bench-Stage'):
                raise BenchError(409, 'no matching stage')
            if stage['direction'] != direction or stage.get('finished'):
                raise BenchError(409, 'wrong stage')
            if stage.get('inflight'):
                raise BenchError(409, 'one payload transfer at a time')
            if stage['bytes'] >= 10 * 1024**3:
                raise BenchError(413, 'stage byte limit')
            stage['inflight'] = True
            return run, stage

    def do_GET(self):
        self.dispatch('GET')

    def do_POST(self):
        self.dispatch('POST')

    def dispatch(self, method):
        try:
            s = self.guard()
            path = urlsplit(self.path).path
            if method == 'GET' and path == '/':
                config = dict(key=s.key, bytes=PAYLOAD_BYTES, crc32=s.crc,
                              seconds=s.duration, warmup=s.warmup, repeats=s.repeats,
                              directions=list(s.directions))
                body = s.html.replace('__BENCH_CONFIG__', json.dumps(config)).encode()
                return self.reply(200, body, 'text/html; charset=utf-8')
            if method == 'GET' and path == '/ping':
                return self.reply(200, b'ok', 'text/plain')
            if method == 'POST' and path == '/begin':
                self.json_body()
                with s.lock:
                    if len(s.runs) >= 6:
                        raise BenchError(429, 'run limit; restart server')
                    token = secrets.token_hex(12)
                    s.runs[token] = dict(peer=self.client_address[0], stages=[],
                                         started_monotonic=time.monotonic(), started_utc=time.time())
                    s.save(token)
                return self.reply(200, dict(run=token))
            if method == 'GET' and path == '/seed':
                with s.lock:
                    run = s.run(self.headers.get('X-Bench-Run', ''), self.client_address[0])
                    if run.get('seeded'):
                        raise BenchError(409, 'seed already sent')
                    run['seeded'] = True
                return self.reply(200, s.payload, 'application/octet-stream')
            if method == 'POST' and path == '/stage':
                body = self.json_body()
                with s.lock:
                    run = s.run(self.headers.get('X-Bench-Run', ''), self.client_address[0])
                    if run.get('current') and not run['current'].get('finished'):
                        raise BenchError(409, 'previous stage unfinished')
                    index = len(run['stages'])
                    if index >= len(s.plan):
                        raise BenchError(409, 'all stages done')
                    direction, warmup = s.plan[index]
                    if body.get('direction') != direction:
                        raise BenchError(409, 'wrong direction order')
                    stage = dict(id=secrets.token_hex(8), direction=direction,
                                 warmup=warmup, seconds=s.warmup if warmup else s.duration,
                                 started=time.monotonic(), bytes=0, requests=0, errors=0)
                    run['current'] = stage
                    run['stages'].append(stage)
                    s.save(self.headers['X-Bench-Run'])
                    reply = {k: stage[k] for k in ('id', 'direction', 'warmup', 'seconds')}
                return self.reply(200, reply)
            if method == 'GET' and path == '/payload':
                run, stage = self.run_stage(s, 'download')
                try:
                    self.reply(200, s.payload, 'application/octet-stream')
                except (OSError, TimeoutError):
                    with s.lock:
                        stage['errors'] += 1
                        stage['inflight'] = False
                    raise
                with s.lock:
                    stage['bytes'] += PAYLOAD_BYTES
                    stage['requests'] += 1
                    stage['inflight'] = False
                    s.save(self.headers['X-Bench-Run'])
                return
            if method == 'POST' and path == '/upload':
                if self.length(PAYLOAD_BYTES) != PAYLOAD_BYTES:
                    raise BenchError(400, 'exact payload length required')
                run, stage = self.run_stage(s, 'upload')
                crc, received, started = 0, 0, time.monotonic()
                try:
                    while received < PAYLOAD_BYTES:
                        chunk = self.rfile.read(min(65536, PAYLOAD_BYTES - received))
                        if not chunk or time.monotonic() - started > 120:
                            raise BenchError(408, 'upload incomplete or expired')
                        crc = zlib.crc32(chunk, crc)
                        received += len(chunk)
                    if f'{crc:08x}' != s.crc:
                        raise BenchError(422, 'checksum mismatch')
                except (OSError, TimeoutError, BenchError):
                    with s.lock:
                        stage['errors'] += 1
                        stage['inflight'] = False
                    raise
                with s.lock:
                    stage['bytes'] += received
                    stage['requests'] += 1
                    stage['inflight'] = False
                    s.save(self.headers['X-Bench-Run'])
                return self.reply(200, dict(bytes=received, crc32=s.crc))
            if method == 'POST' and path == '/finish':
                body = self.json_body()
                with s.lock:
                    run = s.run(self.headers.get('X-Bench-Run', ''), self.client_address[0])
                    stage = run.get('current')
                    if not stage or stage.get('finished') or stage['id'] != self.headers.get('X-Bench-Stage'):
                        raise BenchError(409, 'no matching unfinished stage')
                    elapsed = body.get('elapsed_ms')
                    server_elapsed = (time.monotonic() - stage['started']) * 1000
                    if (type(elapsed) not in (int, float) or not math.isfinite(elapsed) or
                        elapsed < stage['seconds'] * 1000 or elapsed > server_elapsed + 1000 or
                        body.get('bytes') != stage['bytes'] or body.get('requests') != stage['requests'] or
                        not stage['bytes'] or stage['errors'] or stage.get('inflight') or body.get('crc_errors') != 0):
                        raise BenchError(422, 'unverified or inconsistent measurement')
                    rtts = body.get('rtt_ms', [])
                    if not isinstance(rtts, list) or len(rtts) > 5000 or any(
                        type(v) not in (int, float) or not math.isfinite(v) or v < 0 or v > 10000 for v in rtts):
                        raise BenchError(422, 'invalid latency data')
                    check_ms = body.get('check_ms', 0)
                    if type(check_ms) not in (int, float) or not math.isfinite(check_ms) or not 0 <= check_ms <= elapsed:
                        raise BenchError(422, 'invalid verification time')
                    if any(type(body.get(k)) is not int or not 0 <= body[k] <= 5000
                           for k in ('ping_failures', 'ping_cancelled')):
                        raise BenchError(422, 'invalid ping counters')
                    ordered = sorted(rtts)
                    percentile = lambda p: ordered[max(0, math.ceil(p * len(ordered)) - 1)] if ordered else None
                    # Control replies can themselves be delayed on a loaded
                    # link. The slower independent clock is conservative and
                    # cannot turn a forged short client time into a fast rate.
                    measured_ms = max(elapsed, server_elapsed)
                    stage.update(finished=True, elapsed_ms=elapsed, server_elapsed_ms=server_elapsed,
                                 rate_elapsed_ms=measured_ms,
                                 mbps=stage['bytes'] * 8 / measured_ms / 1000,
                                 client_mbps=stage['bytes'] * 8 / elapsed / 1000, check_ms=check_ms,
                                 rtt_ms=rtts, rtt_p50=percentile(.5), rtt_p95=percentile(.95),
                                 rtt_p99=percentile(.99), ping_failures=body.get('ping_failures'),
                                 ping_cancelled_at_stage_end=body.get('ping_cancelled'), crc_errors=0)
                    run['complete'] = len(run['stages']) == len(s.plan)
                    s.save(self.headers['X-Bench-Run'])
                    reply = {k: stage[k] for k in ('mbps', 'rtt_p50', 'rtt_p95', 'rtt_p99', 'warmup')}
                return self.reply(200, reply)
            if method == 'POST' and path == '/abort':
                body = self.json_body()
                with s.lock:
                    run = s.run(self.headers.get('X-Bench-Run', ''), self.client_address[0])
                    run['aborted'] = True
                    reason = body.get('reason')
                    if reason in ('user_stop', 'timeout', 'checksum', 'http_error', 'network_error'):
                        run['abort_reason'] = reason
                    diagnostic = body.get('diagnostic')
                    if reason == 'checksum' and isinstance(diagnostic, dict):
                        allowed = ('expected_bytes', 'actual_bytes', 'first_mismatch',
                                   'last_mismatch', 'mismatched_bytes')
                        safe = {k: diagnostic[k] for k in allowed if type(diagnostic.get(k)) is int
                                and -1 <= diagnostic[k] <= PAYLOAD_BYTES * 2}
                        if diagnostic.get('phase') in ('seed', 'download'):
                            safe['phase'] = diagnostic['phase']
                        if type(diagnostic.get('comparison_available')) is bool:
                            safe['comparison_available'] = diagnostic['comparison_available']
                        for k in ('expected_crc32', 'actual_crc32'):
                            if isinstance(diagnostic.get(k), str) and re.fullmatch('[0-9a-f]{8}', diagnostic[k]):
                                safe[k] = diagnostic[k]
                        offsets = diagnostic.get('first_mismatch_offsets')
                        if isinstance(offsets, list) and len(offsets) <= 16 and all(
                            type(v) is int and 0 <= v < PAYLOAD_BYTES * 2 for v in offsets):
                            safe['first_mismatch_offsets'] = offsets
                        run['checksum_diagnostic'] = safe
                    s.save(self.headers['X-Bench-Run'])
                return self.reply(200, dict(aborted=True))
            raise BenchError(404, 'not found')
        except BenchError as exc:
            token = self.headers.get('X-Bench-Run')
            with self.server.state.lock:
                run = self.server.state.runs.get(token)
                if run is not None and run['peer'] == self.client_address[0]:
                    run['last_http_error'] = dict(status=exc.status, reason=exc.message,
                                                   path=urlsplit(self.path).path[:40])
                    self.server.state.save(token)
            self.close_connection = True
            self.reply(exc.status, dict(error=exc.message))
        except (OSError, TimeoutError):
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', required=True, type=ipaddress.IPv4Address)
    parser.add_argument('--allow', action='append', required=True, type=ipaddress.IPv4Address)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--context', type=Path, help='Prepared non-secret test metadata JSON')
    parser.add_argument('--directions', choices=['both','download'], default='both')
    parser.add_argument('--duration', type=int, default=60, choices=range(1, 121))
    parser.add_argument('--warmup', type=int, default=10, choices=range(0, 31))
    parser.add_argument('--repeats', type=int, default=3, choices=range(1, 4))
    parser.add_argument('--lifetime', type=int, default=2400, choices=range(60, 3601))
    args = parser.parse_args()
    state = BenchState(args.output, duration=args.duration, warmup=args.warmup,
                       repeats=args.repeats, lifetime=args.lifetime,
                       directions=('download',) if args.directions=='download' else ('download','upload'),
                       context=json.loads(args.context.read_text(encoding='utf-8')) if args.context else None)
    with BenchServer((str(args.bind), args.port), state, [str(a) for a in args.allow]) as server:
        timer = threading.Timer(args.lifetime, server.shutdown)
        timer.daemon = True
        timer.start()
        print(json.dumps(dict(url=f'http://{args.bind}:{server.server_port}/',
                              allowed=sorted(server.allow), lifetime_seconds=args.lifetime,
                              payload_bytes=PAYLOAD_BYTES, payload_storage='RAM')), flush=True)
        try:
            server.serve_forever(poll_interval=.25)
        finally:
            timer.cancel()


if __name__ == '__main__':
    main()
