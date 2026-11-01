#!/usr/bin/env python3
"""Execute the source assembly subset to check pipelined loads and dot bounds.

This is not a silicon/encoding test. Native firmware must independently pass
its integer known-answer tests and unchanged whole-waveform hash.
"""
import argparse
import random
import re
from pathlib import Path


def program(source, name):
    source = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
    source = source.split(name + ':', 1)[1].split('.size', 1)[0]
    lines = [x.strip() for x in source.splitlines() if x.strip()]
    expanded = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('.rept '):
            count = int(line.split()[1]); body = []; i += 1
            while lines[i] != '.endr':
                body.append(lines[i]); i += 1
            expanded.extend(body * count)
        else:
            expanded.append(line)
        i += 1
    code = []; labels = {}
    for line in expanded:
        if line.endswith(':'):
            labels[line[:-1]] = len(code)
        elif not line.startswith('.'):
            code.append(re.split(r'[\s,]+', line))
    return code, labels


def execute(prog, weights, inputs, stride, rows=1, patches=1, single=False):
    code, labels = prog
    regions = [(0x1000, weights), (0x100000, inputs)]
    result = {}; stack = {}; reg = dict(sp=0x800000)
    args = [0x1000, 0x100000, stride] if single else [0x1000, 0x100000, stride, rows, patches, 0x200000]
    reg.update({f'a{i}': value for i, value in enumerate(args)})
    vector = {}; acc = 0; pc = steps = 0

    def value(r):
        return 0 if r == 'zero' else reg.get(r, 0)

    def put(r, v):
        reg[r] = v & 0xffffffff

    def load16(address):
        assert address % 16 == 0
        for base, data in regions:
            if base <= address and address + 16 <= base + len(data) * 2:
                return data[(address-base)//2:(address-base)//2+8]
        raise AssertionError(f'out-of-range 128-bit read: {address:#x}')

    while True:
        op, *a = code[pc]; pc += 1; steps += 1
        assert steps < 1000000
        if op == 'ret':
            return value('a0') if single else [result[0x200000+4*i] for i in range(rows*patches*((stride+255)//256))]
        if op == 'li': put(a[0], int(a[1], 0))
        elif op == 'mv': put(a[0], value(a[1]))
        elif op in ('addi', 'add', 'sub', 'slli'):
            left = value(a[1]); right = int(a[2], 0) if op in ('addi', 'slli') else value(a[2])
            put(a[0], left-right if op == 'sub' else left << right if op == 'slli' else left+right)
        elif op in ('lw', 'sw'):
            offset, base = re.fullmatch(r'(-?\d+)\((\w+)\)', a[1]).groups()
            address = value(base)+int(offset)
            if op == 'sw':
                target = result if 0x200000 <= address < 0x300000 else stack
                if target is result: assert address < 0x200000+4*rows*patches*((stride+255)//256)
                target[address] = value(a[0])
            else: put(a[0], stack[address])
        elif op in ('bltu', 'bgeu', 'beqz', 'bnez', 'bgtz'):
            left = value(a[0]); target = a[-1]
            taken = left < value(a[1]) if op == 'bltu' else left >= value(a[1]) if op == 'bgeu' else left == 0 if op == 'beqz' else left != 0 if op == 'bnez' else 0 < left < 0x80000000
            if taken: pc = labels[target[:-1]]
        elif op == 'esp.zero.xacc': acc = 0
        elif op == 'esp.vld.128.ip':
            vector[a[0]] = load16(value(a[1])); put(a[1], value(a[1])+int(a[2], 0))
        elif op == 'esp.vmulas.s16.xacc':
            acc += sum(x*y for x, y in zip(vector[a[0]], vector[a[1]]))
            assert abs(acc) < 2**39
        elif op == 'esp.vmulas.s16.xacc.ld.ip':
            # Fused instruction reads old source registers before replacing qu.
            acc += sum(x*y for x, y in zip(vector[a[3]], vector[a[4]]))
            assert abs(acc) < 2**39
            vector[a[0]] = load16(value(a[1])); put(a[1], value(a[1])+int(a[2], 0))
        elif op == 'esp.srs.s.xacc':
            assert value(a[1]) == 0 and -(2**31) <= acc < 2**31
            put(a[0], acc)
        else: raise AssertionError(op)


def main():
    parser = argparse.ArgumentParser(); parser.add_argument('--source', type=Path)
    args = parser.parse_args()
    source = (args.source or Path(__file__).resolve().parents[2]/'app/espdl-quickapp/experimental/local_tts_dot_p4.S').read_text()
    one = program(source, 'glv_tts_dot_pie'); batch = program(source, 'glv_tts_dot_batch_pie')
    rng = random.Random(37); cases = 0
    for mode in range(8):
        def data(n, limit, bit):
            return [(-limit if mode & bit else limit) if mode < 4 else rng.randint(-limit, limit) for _ in range(n)]
        for n in range(8, 257, 8):
            w = data(n, 127, 1); x = data(n, 32767, 2)
            assert execute(one, w, x, n, single=True) == sum(a*b for a,b in zip(w,x)) & 0xffffffff
            cases += 1
        for stride in (8, 24, 32, 40, 248, 256, 264, 512, 520):
            w = data(2*stride, 127, 1); x = data(3*stride, 32767, 2); expected = []
            for row in range(2):
                for patch in range(3):
                    for at in range(0, stride, 256):
                        expected.append(sum(w[row*stride+i]*x[patch*stride+i] for i in range(at, min(at+256, stride))) & 0xffffffff)
            assert execute(batch, w, x, stride, 2, 3) == expected
            cases += 1
    print('TTS_FUSED_SOURCE_ADDRESS_AND_INTEGER_REFERENCE_OK', cases)


if __name__ == '__main__': main()
