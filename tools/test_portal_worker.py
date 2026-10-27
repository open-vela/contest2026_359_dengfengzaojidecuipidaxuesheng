#!/usr/bin/env python3
"""Real production portal worker: TCP browser flow and platform fault recovery."""
import argparse
import concurrent.futures
import hashlib
import http.client
import json
import os
import socket
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--desktop-source', type=Path)
    parser.add_argument('--expect-baseline-failure', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    desktop = (args.desktop_source or repo / 'app/espdl-quickapp/overlay/apps/system/desktop').resolve()
    fixture = repo / 'app/espdl-quickapp/tests/portal_worker_host.c'
    cjson = args.apps.resolve() / 'netutils/cjson/cJSON'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'nuttx').mkdir(exist_ok=True)
    (out / 'nuttx/config.h').write_text('#pragma once\n')
    binary = out / 'portal-worker'
    command = ['gcc', '-std=gnu11', '-D_GNU_SOURCE', '-O1', '-g', '-pthread',
               '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
               '-I' + str(out), '-I' + str(desktop), '-I' + str(repo / 'tools/c6'),
               '-I' + str(cjson), '-DPORTAL_HTTP_SOURCE="' + str(desktop / 'glass_portal_http.c') + '"',
               str(fixture), str(desktop / 'glass_portal_resource.c'), str(cjson / 'cJSON.c'),
               '-o', str(binary)]
    compiled = subprocess.run(command, text=True, capture_output=True)
    (out / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr)
        raise SystemExit(compiled.returncode)
    results = []
    stderr = (out / 'worker-stderr.log').open('w')
    worker = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=stderr, text=True, bufsize=1,
                              env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                   'UBSAN_OPTIONS': 'halt_on_error=1'})
    first = json.loads(worker.stdout.readline())
    port, code = first['port'], first['code']

    def control(message):
        worker.stdin.write(message + '\n'); worker.stdin.flush()
        line = worker.stdout.readline()
        assert line, 'worker exited: ' + (out / 'worker-stderr.log').read_text()
        return json.loads(line)

    def await_status(predicate, seconds=5):
        deadline = time.monotonic() + seconds
        while True:
            state = control('STATUS')
            if predicate(state) or time.monotonic() >= deadline:
                return state
            time.sleep(.05)

    def check(name, condition, evidence=None):
        results.append({'name': name, 'passed': bool(condition), 'evidence': evidence})
        print(('PASS ' if condition else 'FAIL ') + name)

    def request(path, method='GET', payload=None, token=None, truncated=False):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=8)
        headers = {}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        body = None if payload is None else json.dumps(payload)
        if body is not None:
            headers['Content-Type'] = 'application/json'
            headers['Origin'] = f'http://127.0.0.1:{port}'
        connection.request(method, path, body=body, headers=headers)
        reply = connection.getresponse()
        expected = int(reply.getheader('Content-Length'))
        partial = False
        try:
            data = reply.read()
        except http.client.IncompleteRead as failure:
            data, partial = failure.partial, True
        status, content_type = reply.status, reply.getheader('Content-Type')
        connection.close()
        assert partial == truncated, (path, 'partial', partial, expected, len(data))
        assert (len(data) < expected) if truncated else (len(data) == expected)
        return status, data, content_type

    def asset(path):
        status, data, kind = request(path)
        source = {'/': 'index.html', '/index.html': 'index.html',
                  '/app.js': 'app.js', '/style.css': 'style.css'}[path]
        expected = (desktop / 'portal' / source).read_bytes()
        assert status == 200 and data == expected, (path, status, len(data), len(expected))
        return {'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'type': kind}

    def empty():
        client = socket.create_connection(('127.0.0.1', port), timeout=5)
        await_status(lambda s: s['active'] >= 1)
        return client

    try:
        check('startup_idempotent_and_listener', first['listener'] >= 0)
        check('listener_not_inherited_by_new_task', first['listener_cloexec'])
        # Two browser preconnections must not starve initial HTML/CSS/JS.
        idle = [empty(), empty()]
        start = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            assets = list(pool.map(asset, ['/', '/style.css', '/app.js']))
        check('first_load_parallel_assets_byte_complete_with_empty_preconnects', time.monotonic() - start < 5, assets)
        for client in idle:
            client.close()
        await_status(lambda s: s['active'] == 0)
        paired_status, paired_data, _ = request('/api/pair', 'POST', {'code': code})
        token = json.loads(paired_data)['token']
        assert len(token) == 32 and paired_status == 200
        check('fresh_pair_exchange_real_worker', True)

        def api(path):
            status, data, _ = request(path, token=token)
            assert status == 200, (path, status)
            return json.loads(data)

        paths = ['/api/session', '/api/network'] + ['/api/config?section=' + section
                 for section in ['ai', 'homeassistant', 'music', 'voice', 'desktop', 'agent']]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(api, paths))
        check('parallel_session_network_six_configuration_reads', len(replies) == 8)
        for _ in range(4):
            with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
                jobs = [pool.submit(asset, path) for path in ['/', '/style.css', '/app.js']]
                jobs += [pool.submit(api, path) for path in ['/api/session', '/api/network']]
                for job in jobs:
                    job.result()
        check('four_refreshes_complete_assets_and_existing_session', True)
        control('TRUNCATE')
        status, data, _ = request('/app.js', truncated=True)
        check('injected_partial_asset_is_detectable', status == 200 and len(data) < len((desktop / 'portal/app.js').read_bytes()))
        asset('/app.js')
        api('/api/session')
        again_status, again, _ = request('/api/pair', 'POST', {'code': code})
        check('asset_retry_and_repair_after_partial_response', again_status == 200 and json.loads(again)['token'] == token)

        before = control('STATUS')
        control('LISTENERERR')
        state = await_status(lambda s: s['faults'] > before['faults'])
        time.sleep(.3)
        state = control('STATUS'); asset('/')
        check('healthy_SO_ACCEPTCONN_listener_survives_POLLERR', state['listener_closes'] == before['listener_closes'], state)

        client = empty(); before = control('STATUS')
        check('accepted_client_not_inherited_by_new_task', before['clients_cloexec'] == before['clients'], before)
        control('CLIENTERR')
        state = await_status(lambda s: s['active'] == 0)
        check('per_client_POLLERR_preserves_listener', state['listener_closes'] == before['listener_closes'], state)
        client.close(); api('/api/session')

        client = empty(); before = control('STATUS'); control('POLL_EBADF')
        await_status(lambda s: s['faults'] > before['faults']);time.sleep(.3)
        state = control('STATUS')
        check('whole_poll_EBADF_preserves_healthy_listener', state['listener_closes'] == before['listener_closes'], state)
        client.close(); await_status(lambda s: s['active'] == 0);asset('/style.css')

        client = empty(); control('INVALID_BLOCK')
        state = await_status(lambda s: s['active'] == 0, seconds=3.5)
        check('pending_clients_expire_while_listener_rebind_EADDRINUSE', state['active'] == 0, state)
        control('UNBLOCK'); await_status(lambda s: s['listener'] >= 0)
        client.close(); asset('/');api('/api/network')
        check('invalid_listener_recovers_after_bind_becomes_available', True)

        # A real dup bind reference models NuttX's non-CLOEXEC task clone.
        held = control('HOLD');client = empty();control('INVALID')
        time.sleep(.5);state = control('STATUS')
        check('CLOEXEC_prevents_inherited_listener_bind_retention', not held['held'] and state['listener'] >= 0,
              {'held_reference': held['held'], 'state': state})
        control('RELEASE');await_status(lambda s: s['listener'] >= 0)
        client.close();await_status(lambda s: s['active'] == 0)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            jobs = [pool.submit(asset, path) for path in ['/', '/style.css', '/app.js']]
            jobs.append(pool.submit(api, '/api/session'))
            for job in jobs:
                job.result()
        check('final_full_refresh_and_pair_session_after_all_faults', True)
    finally:
        if worker.poll() is None:
            worker.stdin.write('STOP\n');worker.stdin.flush()
            try:
                worker.wait(timeout=8)
            except subprocess.TimeoutExpired:
                worker.kill();worker.wait()
        stderr.close()
    assert worker.returncode == 0, (out / 'worker-stderr.log').read_text()
    failures = [r['name'] for r in results if not r['passed']]
    expected_names = {'listener_not_inherited_by_new_task', 'accepted_client_not_inherited_by_new_task',
                      'healthy_SO_ACCEPTCONN_listener_survives_POLLERR',
                      'whole_poll_EBADF_preserves_healthy_listener',
                      'pending_clients_expire_while_listener_rebind_EADDRINUSE',
                      'CLOEXEC_prevents_inherited_listener_bind_retention'}
    if args.expect_baseline_failure:
        assert set(failures) == expected_names, failures
    else:
        assert not failures, failures
    files = [fixture, Path(__file__).resolve(), desktop / 'glass_portal_http.c',
             desktop / 'glass_portal.h', desktop / 'glass_network_recovery.h',
             desktop / 'glass_portal_resource.c', repo / 'tools/c6/desktop_worker.h',
             cjson / 'cJSON.c', cjson / 'cJSON.h']
    files += [desktop / 'portal' / name for name in ['index.html', 'app.js', 'style.css']]
    report = {'passed': not failures, 'baseline_failure_expected': args.expect_baseline_failure,
              'baseline_failure_reproduced': args.expect_baseline_failure and bool(failures),
              'scope': 'actual Linux TCP service_start/worker/router/static resources; fake services; injected poll/bind/send errors; no board',
              'descriptor_clone_model': 'real Linux dup reference only when source listener lacks FD_CLOEXEC, matching NuttX clone policy; not a NuttX runtime test',
              'sanitizers': ['address', 'undefined'], 'cases': results,
              'source_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (out / 'RESULT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    (out / 'run.log').write_text('\n'.join(
        ('PASS ' if item['passed'] else 'FAIL ') + item['name'] for item in results) + '\n' +
        ('PORTAL_WORKER_BASELINE_NEGATIVE_OK' if failures else 'PORTAL_WORKER_OK') +
        ' ' + str(len(results)) + ' cases\n')
    print('PORTAL_WORKER_BASELINE_NEGATIVE_OK' if failures else 'PORTAL_WORKER_OK', len(results), 'cases')


if __name__ == '__main__':
    main()
