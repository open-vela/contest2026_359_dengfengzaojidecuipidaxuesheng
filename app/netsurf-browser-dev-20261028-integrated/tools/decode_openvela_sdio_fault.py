"""Decode a previously saved first-fault RAM block. Never accesses a device."""
import argparse
import json
import struct
from pathlib import Path

PHASES = ['arm', 'dma_begin', 'dma_ready', 'submit', 'first_irq', 'return']
REASONS = {1: 'watchdog_timeout', 2: 'late_submit_rejected',
           3: 'controller_command_busy', 4: 'dma_setup_failed',
           5: 'interrupt_error', 6: 'receive_cache_sync_failed',
           7: 'watchdog_start_failed'}


def decode(data, layout):
    record = layout['types']['sdio_first_fault_s']
    fields = record['fields']
    if len(data) != record['bytes']:
        raise ValueError('Dump length does not match this candidate ELF')

    def scalar(name, fmt='<I'):
        return struct.unpack_from(fmt, data, fields[name])[0]

    if scalar('magic') != 0x53444631 or scalar('version') != 1:
        raise ValueError('Not a version 1 SDIO fault record')
    if scalar('bytes') != len(data) or scalar('valid') != 2:
        raise ValueError('Record has not been completely frozen')
    out = {}
    arrays = {'phase_us', 'phase_tick', 'phase_cpu', 'sched', 'descriptor_cpu'}
    signed = {'owner_pid', 'fault_pid', 'error'}
    wide = {'off_since_us', 'off_total_us', 'off_max_us', 'fault_us'}
    for name in fields:
        if name not in arrays:
            out[name] = scalar(name, '<Q' if name in wide else
                               '<i' if name in signed else '<I')
    out['reason_name'] = REASONS.get(out['reason'], 'unknown')
    out['phases'] = {}
    for i, name in enumerate(PHASES):
        out['phases'][name] = {
            'us': struct.unpack_from('<Q', data, fields['phase_us'] + 8*i)[0],
            'tick': struct.unpack_from('<I', data, fields['phase_tick'] + 4*i)[0],
            'cpu': struct.unpack_from('<I', data, fields['phase_cpu'] + 4*i)[0]}
    ring = layout['types']['sdio_sched_trace_s']
    events = []
    for sequence in range(max(0, out['sched_count'] - 16), out['sched_count']):
        offset = fields['sched'] + (sequence % 16) * ring['bytes']
        event = {'sequence': sequence}
        for name, field in ring['fields'].items():
            fmt = '<Q' if name == 'us' else '<i' if name == 'pid' else '<B'
            event[name] = struct.unpack_from(fmt, data, offset + field)[0]
        events.append(event)
    out['scheduler_events'] = events
    out['descriptor_cpu'] = list(struct.unpack_from('<4I', data,
                                                     fields['descriptor_cpu']))
    out['descriptor_coherent_dma_readback'] = False
    out['open_off_cpu_us'] = (out['fault_us'] - out['off_since_us']
                             if out['owner_suspended'] else 0)
    out['off_cpu_including_open_us'] = out['off_total_us'] + out['open_off_cpu_us']
    out['notes'] = [
        'Suspend/resume intervals include IRQ handling, blocking and preemption; same-PID events need not be task switches.',
        'First IRQ time is software handler entry, not hardware completion time.',
        'Descriptor words are CPU cache views; the recorder does not invalidate.',
        'Zero phase timestamp means that phase was not recorded before freezing.',
        'This is a decoded saved dump, not a new device observation.']
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--layout', type=Path, required=True)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    value = decode(args.dump.read_bytes(), json.loads(args.layout.read_text()))
    text = json.dumps(value, indent=2, ensure_ascii=False) + '\n'
    if args.output:
        if args.output.exists():
            raise FileExistsError('Refusing to replace an existing decoded record')
        args.output.write_text(text, encoding='utf-8')
    else:
        print(text, end='')
