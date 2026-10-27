#!/usr/bin/env python3
"""Summarize one frozen run; never stitch partial runs into a baseline."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics


def summarize(path, expected_image):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    context = data.get('context', {})
    if context.get('image_sha256') != expected_image:
        raise ValueError('firmware identity mismatch')
    if data.get('peer') != context.get('expected_peer_for_phone_run'):
        raise ValueError('not the expected NAT peer')
    for name, digest in data.get('source_sha256', {}).items():
        if name not in ('network_benchmark.py', 'network_benchmark.html'):
            raise ValueError('unexpected source name')
        if hashlib.sha256((Path(path).parent/'source'/name).read_bytes()).hexdigest() != digest:
            raise ValueError('frozen tool source mismatch')
    if len(data.get('source_sha256', {})) != 2:
        raise ValueError('missing frozen source evidence')
    stages = data.get('stages', [])
    def verified(stage):
        if not stage.get('finished') or stage.get('crc_errors') != 0 or stage.get('errors'):
            return False
        if type(stage.get('bytes')) is not int or stage['bytes'] <= 0:
            return False
        values=[stage.get('elapsed_ms'),stage.get('server_elapsed_ms',stage.get('elapsed_ms')),stage.get('mbps')]
        if any(type(v) not in (int,float) or not math.isfinite(v) or v <= 0 for v in values):
            return False
        expected=stage['bytes']*8/max(values[0],values[1])/1000
        return math.isclose(values[2],expected,rel_tol=1e-6,abs_tol=1e-9)
    valid = [s for s in stages if verified(s)]
    complete = bool(data.get('complete') and not data.get('aborted') and
                    stages and len(valid) == len(stages))
    directions = {}
    required = data.get('directions',['download','upload'])
    if required not in (['download'],['download','upload']):
        raise ValueError('unsupported test directions')
    is_baseline = (complete and len(stages) == len(required)*4 and
                   [s.get('direction') for s in stages] == [d for d in required for _ in range(4)] and
                   [s.get('warmup') for s in stages] == [True,False,False,False]*len(required))
    for direction in ('download', 'upload'):
        measured = [s for s in valid if s.get('direction') == direction and not s.get('warmup')]
        rates = [s['mbps'] for s in measured]
        rtt = sorted(v for s in measured for v in s.get('rtt_ms', []))
        pct = lambda q: rtt[max(0, math.ceil(q*len(rtt))-1)] if rtt else None
        directions[direction] = dict(windows=len(measured),
            median_mbps=statistics.median(rates) if rates else None,
            min_mbps=min(rates) if rates else None, max_mbps=max(rates) if rates else None,
            bytes=sum(s['bytes'] for s in measured),
            client_seconds=[s['elapsed_ms']/1000 for s in measured],
            rtt_p50_ms=pct(.5), rtt_p95_ms=pct(.95), rtt_p99_ms=pct(.99),
            probe_timeouts=sum(s.get('ping_failures', 0) for s in measured),
            cancelled_probes=sum(s.get('ping_cancelled_at_stage_end', 0) for s in measured))
        if direction in required:
            is_baseline &= len(measured) == 3 and all(s['elapsed_ms'] >= 60000 for s in measured)
            warmups = [s for s in valid if s.get('direction') == direction and s.get('warmup')]
            is_baseline &= len(warmups) == 1 and warmups[0].get('elapsed_ms', 0) >= 10000
    target_met = bool(is_baseline and directions['download']['windows']==3 and
                      directions['download']['min_mbps']>=5)
    return dict(result_sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest(),
        image_sha256=expected_image, peer=data['peer'], complete_single_run=complete,
        complete_three_by_sixty_second_baseline=bool(is_baseline),
        required_directions=required, download_5mbps_each_window_met=target_met,
        scope='complete baseline' if is_baseline else 'diagnostic or incomplete; not a full baseline',
        client_abort_reason=data.get('abort_reason'), checksum_diagnostic=data.get('checksum_diagnostic'),
        download=directions['download'], upload=directions['upload'],
        failed_or_unfinished_stages=len(stages)-len(valid),
        prior_integrity_failure_cleared_by_this_run=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result', type=Path)
    parser.add_argument('--expect-image-sha', required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--require-baseline', action='store_true')
    args = parser.parse_args()
    result = summarize(args.result, args.expect_image_sha)
    args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    if args.require_baseline and not result['complete_three_by_sixty_second_baseline']:
        raise SystemExit(2)


if __name__ == '__main__':
    main()
