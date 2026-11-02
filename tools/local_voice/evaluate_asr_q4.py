#!/usr/bin/env python3
"""Host-only group-32 Q4 feasibility probe for the fixed Chinese Zipformer.

Exports packed SD-format data and dequantized ONNX graphs for quality checks.
The ONNX graphs deliberately use FP32 arithmetic: these measurements do not
claim a working P4 Q4 operator port or its inference memory/latency.
"""
import argparse, hashlib, json, math, tarfile, time, wave
from pathlib import Path
import numpy as np
import onnx
from onnx import numpy_helper
import sherpa_onnx

ARCHIVE_SHA = '2cbd71b640d9c37d3784f29367333a4577b0398b62e9deeed418170b081cba8b'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def distance(a, b):
    row = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        new = [i]
        for j, y in enumerate(b, 1):
            new.append(min(new[-1] + 1, row[j] + 1, row[j - 1] + (x != y)))
        row = new
    return row[-1]


def recognizer(folder):
    return sherpa_onnx.OnlineRecognizer.from_transducer(
        tokens=str(folder / 'tokens.txt'),
        encoder=str(folder / 'encoder-epoch-99-avg-1.onnx'),
        decoder=str(folder / 'decoder-epoch-99-avg-1.onnx'),
        joiner=str(folder / 'joiner-epoch-99-avg-1.onnx'),
        num_threads=1, sample_rate=16000, feature_dim=80,
        decoding_method='greedy_search', provider='cpu')


def decode(engine, file):
    with wave.open(str(file)) as wav:
        assert wav.getnchannels() == 1 and wav.getsampwidth() == 2
        rate = wav.getframerate()
        pcm = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2').astype(np.float32) / 32768
    stream = engine.create_stream()
    began = time.monotonic()
    for at in range(0, len(pcm), 1600):
        stream.accept_waveform(rate, pcm[at:at + 1600])
        while engine.is_ready(stream):
            engine.decode_stream(stream)
    stream.accept_waveform(rate, np.zeros(int(.8 * rate), dtype=np.float32))
    stream.input_finished()
    while engine.is_ready(stream):
        engine.decode_stream(stream)
    return dict(text=engine.get_result(stream), seconds=time.monotonic() - began)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--mode', choices=('q4', 'mixed', 'q8'), default='q4')
    p.add_argument('--layout', choices=('flat', 'output-rows'), default='flat')
    args = p.parse_args()
    baseline, out = args.baseline.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    archive = baseline / 'source.tar.bz2'
    assert sha(archive.read_bytes()) == ARCHIVE_SHA
    original, quant = out / 'fp32', out / 'q4-dequantized'
    original.mkdir(); quant.mkdir()
    with tarfile.open(archive, 'r:bz2') as tar:
        for member in tar.getmembers():
            name = Path(member.name)
            assert not name.is_absolute() and '..' not in name.parts and not member.issym() and not member.islnk()
            if member.isfile() and (name.name in ('tokens.txt', 'README.md') or name.name.endswith('.onnx') and '.int8.' not in name.name):
                (original / name.name).write_bytes(tar.extractfile(member).read())
    (quant / 'tokens.txt').write_bytes((original / 'tokens.txt').read_bytes())
    models = []
    for file in sorted(original.glob('*.onnx')):
        model = onnx.load(file)
        consumers = {}
        matrix_rhs = {node.input[1] for node in model.graph.node if node.op_type == 'MatMul' and len(node.input) > 1}
        for node in model.graph.node:
            for name in node.input:
                consumers.setdefault(name, []).append(node.name)
        blob, rows = bytearray(), []
        for tensor in model.graph.initializer:
            x = numpy_helper.to_array(tensor).copy()
            dtype, payload = str(x.dtype), x.tobytes()
            errors = {};packing = {}
            if x.dtype == np.float32 and x.ndim >= 2 and x.size >= 256:
                assert np.isfinite(x).all()
                transposed = args.layout == 'output-rows' and tensor.name in matrix_rhs and x.ndim == 2
                source = x.T.copy() if transposed else x
                if args.layout == 'output-rows':
                    logical = source.reshape(source.shape[0], -1)
                    padded_columns = math.ceil(logical.shape[1] / 32) * 32
                    padded = np.zeros((logical.shape[0], padded_columns), dtype=np.float32)
                    padded[:, :logical.shape[1]] = logical
                    flat = padded.ravel()
                    packing = dict(transposed_matrix_rhs=transposed, packed_rows=logical.shape[0], logical_columns=logical.shape[1], padded_columns=padded_columns)
                else:
                    flat = np.zeros(math.ceil(x.size / 32) * 32, dtype=np.float32)
                    flat[:x.size] = x.ravel()
                groups = flat.reshape(-1, 32)
                bits = 4 if args.mode == 'q4' or args.mode == 'mixed' and any('feed_forward' in name for name in consumers.get(tensor.name, [])) else 8
                maximum = 7 if bits == 4 else 127
                scales = np.maximum(np.abs(groups).max(1) / maximum, np.float32(1e-12)).astype('<f4')
                q = np.clip(np.rint(groups / scales[:, None]), -maximum, maximum).astype(np.int8)
                packed = ((q[:, 0::2] & 15) | ((q[:, 1::2] & 15) << 4)).astype(np.uint8) if bits == 4 else q.view(np.uint8)
                data = np.empty((len(scales), 20 if bits == 4 else 36), dtype=np.uint8)
                data[:, :4] = scales.view(np.uint8).reshape(-1, 4)
                data[:, 4:] = packed
                dtype, payload = 'q%d_32_%s_padded' % (bits, args.layout), data.tobytes()
                # Decode the actual serialized nibbles, not a second fake path.
                raw = np.frombuffer(payload, dtype=np.uint8).reshape(-1, 20 if bits == 4 else 36)
                if bits == 4:
                    decoded = np.empty((len(raw), 32), dtype=np.int8)
                    decoded[:, 0::2] = raw[:, 4:] & 15
                    decoded[:, 1::2] = raw[:, 4:] >> 4
                    decoded[decoded >= 8] -= 16
                else:
                    decoded = raw[:, 4:].view(np.int8)
                scale = raw[:, :4].copy().view('<f4').reshape(-1, 1)
                restored = (decoded.astype(np.float32) * scale).ravel()
                if args.layout == 'output-rows':
                    y = restored.reshape(padded.shape)[:, :logical.shape[1]].reshape(source.shape)
                    if transposed:y = y.T.copy()
                else:y = restored[:x.size].reshape(x.shape)
                difference = y - x
                errors = dict(max_abs=float(np.abs(difference).max()), relative_l2=float(np.linalg.norm(difference.ravel()) / max(float(np.linalg.norm(x.ravel())), 1e-12)))
                tensor.CopyFrom(numpy_helper.from_array(y, tensor.name))
            while len(blob) % 16:
                blob.append(0)
            rows.append(dict(name=tensor.name, shape=list(x.shape), dtype=dtype, consumers=consumers.get(tensor.name, []),
                             offset=len(blob), bytes=len(payload), source_elements=x.size, **packing, **errors))
            blob.extend(payload)
        stem = file.stem
        packed_file = out / (stem + '.q4.bin')
        packed_file.write_bytes(blob)
        directory = dict(format='experimental-zipformer-%s-1' % args.mode, layout=args.layout, runtime_ready=False,
                         source_sha256=sha(file.read_bytes()), bytes=len(blob), sha256=sha(blob), tensors=rows)
        (out / (stem + '.q4.json')).write_text(json.dumps(directory, indent=2) + '\n', encoding='utf-8')
        onnx.save(model, quant / file.name)
        models.append(dict(file=file.name, source_sha256=directory['source_sha256'], packed_bytes=len(blob), packed_sha256=sha(blob), quantized_tensors=sum(v['dtype'].startswith('q4') for v in rows)))
    engines = {'fp32': recognizer(original), args.mode: recognizer(quant)}
    cases = []
    for file in sorted((baseline / 'model').glob('*.wav')):
        result = {key: decode(engine, file) for key, engine in engines.items()}
        edits = distance(result['fp32']['text'], result[args.mode]['text'])
        cases.append(dict(file=file.name, results=result, reference_edit_distance=edits,
                          reference_characters=len(result['fp32']['text']), relative_cer=edits / max(1, len(result['fp32']['text']))))
        print(file.name, json.dumps(cases[-1], ensure_ascii=False), flush=True)
    total = sum(item['packed_bytes'] for item in models)
    report = dict(passed=True, mode=args.mode, layout=args.layout, scope='Host dequantized FP32 arithmetic and three official clips; not P4 inference',
                  archive_sha256=ARCHIVE_SHA, models=models, cases=cases, total_packed_initializer_bytes=total,
                  inference_budget_bytes=12 * 1024 * 1024, weights_alone_fit=total <= 12 * 1024 * 1024,
                  runtime_ready=False, board_tested=False,
                  limitations=['Three examples are insufficient for recognition-quality acceptance.',
                               'Packed data is experimental and is not installed into the boot-verified SD release.',
                               'P4 feature extraction, operators, streamed state, allocator, cancellation and audio handoff are not ported.'])
    (out / 'Q4-ASSESSMENT.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('Q4_PACKED_INITIALIZERS', total, flush=True)


if __name__ == '__main__':
    main()
