#!/usr/bin/env python3
"""Measure concrete encoder states and a conservative SD-streaming schedule.

Runs the unmodified official INT8 graph and an all-intermediate-output copy on
the host. This is a resource assessment, not an MCU recognizer implementation.
"""
import argparse, collections, hashlib, json, math, time
from pathlib import Path
import numpy as np
import onnx
from onnx import numpy_helper
import onnxruntime as ort


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def options():
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 1
    opts.inter_op_num_threads = 1
    opts.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    return opts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = next(args.model.glob('encoder*.int8.onnx'))
    model = onnx.load(source)
    session = ort.InferenceSession(str(source), options(), providers=['CPUExecutionProvider'])
    types = {'tensor(float)': np.float32, 'tensor(int64)': np.int64, 'tensor(int32)': np.int32}
    inputs = {}
    rng = np.random.default_rng(714)
    for item in session.get_inputs():
        shape = [1 if dim in ('N', 'batch_size') else dim for dim in item.shape]
        assert all(isinstance(dim, int) and dim > 0 for dim in shape), (item.name, shape)
        inputs[item.name] = np.zeros(shape, dtype=types[item.type])
    inputs['x'][:] = rng.standard_normal(inputs['x'].shape).astype(np.float32)
    began = time.monotonic()
    reference = session.run(None, inputs)
    reference_seconds = time.monotonic() - began
    inferred = onnx.shape_inference.infer_shapes(model)
    info = {item.name: item for item in [*inferred.graph.input, *inferred.graph.output, *inferred.graph.value_info]}
    wanted = list(dict.fromkeys(name for node in model.graph.node for name in node.output if name))
    original_outputs = [item.name for item in model.graph.output]
    del inferred.graph.output[:]
    for name in wanted:
        assert name in info, ('Missing output type', name)
        inferred.graph.output.append(info[name])
    instrumented = out / 'encoder-profile.onnx'
    onnx.save(inferred, instrumented)
    probe = ort.InferenceSession(str(instrumented), options(), providers=['CPUExecutionProvider'])
    values = dict(zip(wanted, probe.run(None, inputs)))
    for name, expected in zip(original_outputs, reference):
        assert np.array_equal(values[name], expected, equal_nan=True), ('Instrumentation changed output', name)
    chunks = []
    for step in range(3):
        if step:
            inputs = {name: reference[original_outputs.index('new_' + name)].copy()
                      for name in inputs if name != 'x'}
            inputs['x'] = rng.standard_normal((1, 39, 80)).astype(np.float32)
            reference = session.run(None, inputs)
            actual = dict(zip(wanted, probe.run(None, inputs)))
            assert all(actual[name].shape == values[name].shape for name in wanted), 'State-dependent shape growth'
            for name, expected in zip(original_outputs, reference):
                assert np.array_equal(actual[name], expected, equal_nan=True), (step, name)
        assert all(np.isfinite(value).all() for value in reference)
        chunks.append(dict(index=step, finite=True, instrumented_outputs_exact=True,
                           encoder_sha256=hashlib.sha256(reference[0].tobytes()).hexdigest(),
                           cached_lengths=[inputs[name].tolist() for name in inputs if name.startswith('cached_len_')]))
    (out / 'CHUNKS.json').write_text(json.dumps(chunks, indent=2) + '\n', encoding='utf-8')
    weights = {item.name: numpy_helper.to_array(item) for item in model.graph.initializer}
    tensors = {**weights, **inputs, **values}
    users = collections.Counter(name for node in model.graph.node for name in node.input if name)
    users.update(original_outputs)
    live = {name: value.nbytes for name, value in inputs.items()}
    peak, peak_node, macs = sum(live.values()), '', 0
    schedule = []
    for index, node in enumerate(model.graph.node):
        # Separate allocations for views, no in-place reuse: conservative.
        for name in node.output:
            if name and name not in weights:
                live[name] = tensors[name].nbytes
        at = sum(live.values())
        if at > peak:
            peak, peak_node = at, node.name or str(index)
        shapes = [list(tensors[name].shape) for name in node.input if name]
        if node.op_type in ('MatMul', 'MatMulInteger'):
            a, b = tensors[node.input[0]], tensors[node.input[1]]
            assert a.ndim >= 2 and b.ndim >= 2
            batch = math.prod(np.broadcast_shapes(a.shape[:-2], b.shape[:-2]))
            macs += batch * a.shape[-2] * a.shape[-1] * b.shape[-1]
        elif node.op_type == 'Conv':
            w = tensors[node.input[1]]
            macs += tensors[node.output[0]].size * math.prod(w.shape[1:])
        schedule.append(dict(index=index, op=node.op_type, name=node.name,
                             input_shapes=shapes, output_shapes=[list(tensors[n].shape) for n in node.output if n],
                             live_bytes_before_input_release=at))
        for name in node.input:
            if not name:
                continue
            users[name] -= 1
            if not users[name]:
                live.pop(name, None)
        for name in node.output:
            if name and not users[name]:
                live.pop(name, None)
    state_bytes = sum(value.nbytes for name, value in inputs.items() if name != 'x')
    next_state_bytes = sum(value.nbytes for name, value in zip(original_outputs, reference) if 'cached' in name)
    referenced = {name for node in model.graph.node for name in node.input if name in weights}
    weight_bytes = sum(weights[name].nbytes for name in referenced)
    budget = 12 * 1024 * 1024
    # Optimistically allow cache to consume every byte not used at peak.
    cache_bytes = max(0, budget - peak)
    min_read = max(0, weight_bytes - cache_bytes)
    metadata = {item.key: item.value for item in model.metadata_props}
    report = dict(passed=True, scope='Host concrete N=1 graph; conservative topological activation schedule, not MCU timing',
                  model_sha256=digest(source), runtime_version=ort.__version__, metadata=metadata,
                  inputs=[dict(name=name, shape=list(value.shape), bytes=value.nbytes) for name, value in inputs.items()],
                  outputs=[dict(name=name, shape=list(value.shape), bytes=value.nbytes) for name, value in zip(original_outputs, reference)],
                  instrumentation_outputs_exact=True, host_chunk_seconds=reference_seconds,
                  seeded_synthetic_feature_chunks=len(chunks), stable_intermediate_shapes=True,
                  state_input_bytes=state_bytes, next_state_output_bytes=next_state_bytes,
                  peak_activation_bytes=peak, peak_node=peak_node,
                  operator_counts=dict(collections.Counter(node.op_type for node in model.graph.node)),
                  matmul_conv_macs_per_chunk=macs, referenced_initializer_bytes=weight_bytes,
                  inference_budget_bytes=budget, optimistic_weight_cache_bytes=cache_bytes,
                  minimum_uncached_int8_weight_bytes_per_chunk=min_read,
                  ideal_20mhz_spi_read_seconds_per_chunk=min_read / (20_000_000 / 8),
                  limitations=['MAC count excludes elementwise, feature extraction and decoder/joiner.',
                               'Activation schedule assumes separate outputs even for view operators; optimized reuse can reduce it.',
                               'SPI figure is a physical wire-rate lower bound for unchanged INT8 weights; filesystem and compute add time.',
                               'Q4 requantization, operator fusion, SD runtime and on-board ASR cancellation are not implemented.'],
                  asr_ready=False, board_tested=False)
    (out / 'SCHEDULE.json').write_text(json.dumps(schedule, indent=2) + '\n', encoding='utf-8')
    (out / 'STREAMING-ASSESSMENT.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: report[key] for key in ('state_input_bytes', 'next_state_output_bytes', 'peak_activation_bytes', 'matmul_conv_macs_per_chunk', 'referenced_initializer_bytes', 'minimum_uncached_int8_weight_bytes_per_chunk', 'ideal_20mhz_spi_read_seconds_per_chunk')}), flush=True)


if __name__ == '__main__':
    main()
