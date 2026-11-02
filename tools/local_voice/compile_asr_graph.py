#!/usr/bin/env python3
"""Export fixed N=1 Zipformer plans and check the complete C graph executor.

ONNX is an export/reference dependency only. Run-time C loads the plan, streams
the real group32 Q4/Q8 rows, executes all dynamic nodes, and reclaims tensors at
last use. These experimental assets never replace the released SD package.
"""
import argparse, collections, ctypes as ct, hashlib, json, struct, subprocess, time
from pathlib import Path
import numpy as np
import onnx
from onnx import numpy_helper
import onnxruntime as ort
from verify_asr_ops import Memory, CANCEL

OPS = {name:i+1 for i,name in enumerate(('Copy','Transpose','Gather','GatherElements','Slice','Concat','Split','Expand','Cast','CumSum','Add','Sub','Mul','Div','Pow','Neg','Relu','Sigmoid','Tanh','Reciprocal','Softmax','ReduceMean','ReduceSum','MatMul','Conv','Gemm'))}
DTYPE = {np.dtype('float32'):1,np.dtype('int32'):6,np.dtype('int64'):7,np.dtype('bool'):9}


def sha(data): return hashlib.sha256(data).hexdigest()


def export_tokens(source,out):
    pieces={}
    for line in source.read_text(encoding='utf-8').splitlines():
        token,index=line.rsplit(' ',1);index=int(index)
        assert index not in pieces and token and all(ord(c)>=32 for c in token)
        pieces[index]=token.encode('utf-8')
    # The source symbol table also contains graph disambiguators #0/#1.
    # They are outside the joiner's 5537 logits and must not become token IDs.
    excluded={k:v.decode('utf-8') for k,v in pieces.items() if k>=5537}
    assert excluded=={5537:'#0',5538:'#1'}
    pieces={k:v for k,v in pieces.items() if k<5537}
    assert sorted(pieces)==list(range(5537))
    offsets=[0];strings=bytearray()
    for index in range(5537):strings.extend(pieces[index]);offsets.append(len(strings))
    payload=struct.pack('<5538I',*offsets)+strings
    encoded=struct.pack('<4s3I32s',b'GLT1',1,5537,len(payload),hashlib.sha256(payload).digest())+payload
    (out/'tokens.bin').write_bytes(encoded)
    (out/'TOKEN-SOURCE.json').write_text(json.dumps(dict(source_sha256=sha(source.read_bytes()),asset_sha256=sha(encoded),format='GLT1',vocab_size=5537,excluded_disambiguators=excluded,bytes=len(encoded)),indent=2)+'\n',encoding='utf-8')


def options():
    opts=ort.SessionOptions();opts.intra_op_num_threads=opts.inter_op_num_threads=1
    opts.graph_optimization_level=ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    return opts


def graph_probe(file):
    model=onnx.load(file);inferred=onnx.shape_inference.infer_shapes(model)
    info={v.name:v for v in [*inferred.graph.input,*inferred.graph.output,*inferred.graph.value_info]}
    wanted=list(dict.fromkeys(n for node in model.graph.node for n in node.output if n))
    del inferred.graph.output[:]
    for name in wanted: inferred.graph.output.append(info[name])
    probe=ort.InferenceSession(inferred.SerializeToString(),options(),providers=['CPUExecutionProvider'])
    inputs={}
    for item in probe.get_inputs():
        shape=[1 if v in ('N','batch_size') else v for v in item.shape]
        assert all(isinstance(v,int) and v>0 for v in shape),(item.name,shape)
        dtype={'tensor(float)':np.float32,'tensor(int64)':np.int64,'tensor(int32)':np.int32}[item.type]
        inputs[item.name]=np.zeros(shape,dtype=dtype)
    values={v.name:numpy_helper.to_array(v) for v in model.graph.initializer}
    values.update(inputs);values.update(zip(wanted,probe.run(None,inputs)))
    return model,probe,inputs,values


def normalize(node,values,constants):
    op=node.op_type;ins=list(node.input);params=[0]*32
    attrs={v.name:onnx.helper.get_attribute_value(v) for v in node.attribute}
    x=values[ins[0]];rank=x.ndim
    def axis(v):
        v=int(v);assert -rank<=v<rank;return v%rank
    if op in ('Reshape','Unsqueeze','Squeeze','Identity'):
        assert all(v in constants for v in ins[1:]);op='Copy';ins=ins[:1]
        assert x.size==values[node.output[0]].size
    elif op=='Transpose':
        perm=attrs.get('perm',list(reversed(range(rank))));assert sorted(perm)==list(range(rank));params[:rank]=perm
    elif op in ('Gather','GatherElements','Concat','Softmax'):
        params[0]=axis(attrs.get('axis',-1 if op=='Softmax' else 0))
    elif op=='Slice':
        assert all(v in constants for v in ins[1:])
        starts=values[ins[1]].ravel();ends=values[ins[2]].ravel()
        axes=values[ins[3]].ravel() if len(ins)>3 and ins[3] else range(len(starts))
        steps=values[ins[4]].ravel() if len(ins)>4 and ins[4] else [1]*len(starts)
        params[:rank]=[0]*rank;params[8:8+rank]=[1]*rank
        for a,s,e,d in zip(axes,starts,ends,steps):
            a=axis(a);start,stop,step=slice(int(s),int(e),int(d)).indices(x.shape[a])
            assert len(range(start,stop,step))==values[node.output[0]].shape[a]
            params[a],params[8+a]=start,step
        ins=ins[:1]
    elif op=='Split':
        assert all(v in constants for v in ins[1:]);params[0]=axis(attrs.get('axis',0));ins=ins[:1]
    elif op=='Expand':
        assert ins[1] in constants;ins=ins[:1]
    elif op=='Cast': pass
    elif op=='CumSum':
        assert ins[1] in constants
        params[:3]=[axis(values[ins[1]].item()),attrs.get('exclusive',0),attrs.get('reverse',0)];ins=ins[:1]
    elif op in ('ReduceMean','ReduceSum'):
        assert all(v in constants for v in ins[1:])
        axes=list(values[ins[1]].ravel()) if len(ins)>1 else attrs.get('axes',list(range(rank)))
        if not axes and not attrs.get('noop_with_empty_axes',0):axes=list(range(rank))
        params[0]=sum(1<<a for a in set(axis(v) for v in axes));ins=ins[:1]
    elif op=='Gemm':
        params[:2]=[attrs.get('transA',0),attrs.get('transB',0)]
        params[30:32]=struct.unpack('<2i',struct.pack('<2f',attrs.get('alpha',1.),attrs.get('beta',1.)))
    elif op=='Conv':
        w=values[ins[1]];assert attrs.get('auto_pad',b'NOTSET')==b'NOTSET'
        d=rank-2;assert d in (1,2) and x.shape[0]==1
        s=attrs.get('strides',[1]*d);di=attrs.get('dilations',[1]*d);p=attrs.get('pads',[0]*(2*d))
        if d==1:h,wi,kh,kw,sh,sw,dh,dw,top,left,bottom,right=1,x.shape[2],1,w.shape[2],1,s[0],1,di[0],0,p[0],0,p[1]
        else:h,wi=x.shape[2:];kh,kw=w.shape[2:];sh,sw=s;dh,dw=di;top,left,bottom,right=p
        params[:15]=[x.shape[1],h,wi,w.shape[0],kh,kw,attrs.get('group',1),sh,sw,dh,dw,top,left,bottom,right]
    assert op in OPS,(op,node.name)
    assert 0<len(ins)<=8 and 0<len(node.output)<=4
    return dict(op=op,name=node.name,inputs=ins,outputs=list(node.output),params=params)


def export(file,packed,out):
    model,probe,inputs,values=graph_probe(file)
    constants={v.name for v in model.graph.initializer};dynamic=[]
    for node in model.graph.node:
        # Shapes are fixed by this plan's exact input descriptors (N=1).
        if node.op_type in ('Constant','Shape') or all(n in constants for n in node.input if n):constants.update(node.output)
        else: dynamic.append(normalize(node,values,constants))
    outputs=[v.name for v in model.graph.output]
    names=list(dict.fromkeys([*inputs,*[v for n in dynamic for v in (*n['inputs'],*n['outputs'])],*outputs]))
    ids={v:i for i,v in enumerate(names)};meta=json.loads((packed/(file.stem+'.q4.json')).read_text(encoding='utf-8'))
    blob=(packed/(file.stem+'.q4.bin')).read_bytes();assert sha(blob)==meta['sha256'] and meta['layout']=='output-rows'
    existing={v['name']:v for v in meta['tensors']};data=bytearray();descs=[]
    for name in names:
        value=values[name];assert value.dtype in DTYPE and value.ndim<=8 and all(d>0 for d in value.shape),(name,value.shape)
        flag=(1 if name in constants else 0)|(2 if name in inputs else 0)|(4 if name in outputs else 0)
        offset=stored=fmt=rows=cols=padded=0
        if flag&1:
            while len(data)%16:data.append(0)
            offset=len(data);e=existing.get(name)
            if e and e['dtype'].startswith(('q4_','q8_')):
                fmt=(1 if e['dtype'].startswith('q4_') else 2)|(256 if e['transposed_matrix_rhs'] else 0)
                rows,cols,padded=e['packed_rows'],e['logical_columns'],e['padded_columns']
                payload=blob[e['offset']:e['offset']+e['bytes']]
            else:
                payload=value.tobytes();rows=value.shape[0] if value.ndim else 1;cols=value.size//rows
            stored=len(payload);data.extend(payload)
        desc=dict(name=name,type=DTYPE[value.dtype],shape=list(value.shape),flags=flag,offset=offset,bytes=stored,format=fmt,rows=rows,columns=cols,padded=padded)
        descs.append(desc)
    plan=bytearray(struct.pack('<4s8I32s12s',b'GLA1',1,72,192,len(descs),len(dynamic),len(inputs),len(outputs),len(data),hashlib.sha256(data).digest(),b'\0'*12))
    for d in descs:
        dims=d['shape']+[0]*(8-len(d['shape']))
        plan.extend(struct.pack('<18I',d['type'],len(d['shape']),d['flags'],*dims,d['offset'],d['bytes'],d['format'],d['rows'],d['columns'],d['padded'],0))
    for n in dynamic:
        ins=[ids[v] for v in n['inputs']];outs=[ids[v] for v in n['outputs']]
        plan.extend(struct.pack('<15I32iI',OPS[n['op']],len(ins),len(outs),*(ins+[0]*(8-len(ins))),*(outs+[0]*(4-len(outs))),*n['params'],0))
    plan.extend(struct.pack('<%dI'%(len(inputs)+len(outputs)),*[ids[v] for v in (*inputs,*outputs)]))
    stem=out/file.stem;(stem.with_suffix('.plan')).write_bytes(plan);(stem.with_suffix('.data')).write_bytes(data)
    report=dict(format='GLA1',asr_ready=False,onnx_sha256=sha(file.read_bytes()),source_packed_sha256=sha(blob),plan_sha256=sha(plan),data_sha256=sha(data),plan_bytes=len(plan),data_bytes=len(data),inputs=list(inputs),outputs=outputs,tensors=descs,nodes=dynamic,op_counts=dict(collections.Counter(n['op'] for n in dynamic)))
    stem.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return stem,report,model,probe,inputs,values,constants


class IO(ct.Structure): _fields_=[('data',ct.c_void_p),('bytes',ct.c_size_t)]


def build_native(root,out,toolchain):
    exp=root/'app/espdl-quickapp/experimental';desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
    sources=[exp/'local_asr_graph.c',exp/'local_asr_ops.c',desktop/'glass_local_model.c',desktop/'glass_local_sha.c']
    lib=out/'asr-graph.so'
    subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fPIC','-shared','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),*[str(v) for v in sources],'-lm','-o',str(lib)],check=True)
    if toolchain:
        for source in sources[:2]:
            target=out/(source.stem+'-rv32.o')
            subprocess.run([str(toolchain/'riscv32-esp-elf-gcc'),'-std=gnu11','-O2','-g','-ffp-contract=off','-march=rv32imafc','-mabi=ilp32f','-fstack-usage','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),'-c',str(source),'-o',str(target)],check=True)
    native=ct.CDLL(str(lib))
    native.glv_asr_graph_open.argtypes=[ct.POINTER(ct.c_void_p),ct.c_char_p,ct.POINTER(Memory)]
    native.glv_asr_graph_close.argtypes=[ct.c_void_p]
    native.glv_asr_graph_run.argtypes=[ct.c_void_p,ct.POINTER(IO),ct.c_uint,ct.POINTER(IO),ct.c_uint]
    native.glv_asr_graph_last_node.argtypes=[ct.c_void_p];native.glv_asr_graph_last_node.restype=ct.c_uint
    native.glv_alloc.argtypes=[ct.POINTER(Memory),ct.c_size_t];native.glv_alloc.restype=ct.c_void_p
    native.glv_free.argtypes=[ct.c_void_p]
    return native


def run(native,handle,memory,report,inputs):
    descriptions={v['name']:v for v in report['tensors']};allocated=[];output={}
    def allocate(names,source):
        ios=[]
        for name in names:
            desc=descriptions[name];dtype={1:np.float32,6:np.int32,7:np.int64,9:np.bool_}[desc['type']]
            shape=tuple(desc['shape']);bytes_=int(np.prod(shape))*np.dtype(dtype).itemsize
            pointer=native.glv_alloc(ct.byref(memory),bytes_);assert pointer,'IO budget exceeded'
            allocated.append(pointer)
            array=np.ndarray(shape,dtype=dtype,buffer=(ct.c_ubyte*bytes_).from_address(pointer))
            if source is not None:array[:]=source[name]
            else:output[name]=array
            ios.append(IO(pointer,bytes_))
        return (IO*len(ios))(*ios)
    try:
        x=allocate(report['inputs'],inputs);y=allocate(report['outputs'],None)
        began=time.monotonic();ret=native.glv_asr_graph_run(handle,x,len(x),y,len(y));elapsed=time.monotonic()-began
        assert ret==0,('C graph failed',ret,native.glv_asr_graph_last_node(handle),report['nodes'][native.glv_asr_graph_last_node(handle)]['name'])
        return {k:v.copy() for k,v in output.items()},elapsed
    finally:
        for ptr in allocated:native.glv_free(ptr)


def compare(actual,expected):
    assert actual.shape==expected.shape and actual.dtype==expected.dtype
    if expected.dtype!=np.float32:
        assert np.array_equal(actual,expected);return dict(exact=True)
    assert np.isfinite(actual).all() and np.isfinite(expected).all()
    diff=(actual-expected).ravel();maximum=float(np.abs(diff).max(initial=0))
    relative=float(np.linalg.norm(diff)/max(float(np.linalg.norm(expected.ravel())),1e-12))
    assert np.allclose(actual,expected,rtol=2e-3,atol=3e-4) and (maximum<1e-4 or relative<5e-5),(maximum,relative)
    return dict(max_abs=maximum,relative_l2=relative)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--packed',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--toolchain',type=Path);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    export_tokens(a.packed/'q4-dequantized/tokens.txt',out)
    native=build_native(root,out,a.toolchain);never=CANCEL(lambda _:False);rng=np.random.default_rng(914)
    reports=[]
    for file in sorted((a.packed/'q4-dequantized').glob('*.onnx')):
        stem,report,model,probe,base,values,constants=export(file,a.packed,out)
        session=ort.InferenceSession(str(file),options(),providers=['CPUExecutionProvider'])
        memory=Memory(0,0,12*1024*1024,never,None);handle=ct.c_void_p()
        ret=native.glv_asr_graph_open(ct.byref(handle),str(stem).encode(),ct.byref(memory));assert ret==0,('open',ret)
        metadata=memory.used;cases=[];reference_inputs={k:v.copy() for k,v in base.items()};native_inputs={k:v.copy() for k,v in base.items()}
        try:
            for chunk in range(3):
                for name in base:
                    if name=='x' or 'cached' not in name and base[name].dtype==np.float32:
                        value=rng.standard_normal(base[name].shape).astype(np.float32);reference_inputs[name]=value;native_inputs[name]=value
                    elif name=='y':
                        value=rng.integers(1,5537,base[name].shape,dtype=np.int64);reference_inputs[name]=value;native_inputs[name]=value
                reference=dict(zip(report['outputs'],session.run(None,reference_inputs)))
                # Ensure all specialized constants and intermediate shapes stay
                # unchanged across independent reference state progression.
                probe_values=dict(zip([v.name for v in probe.get_outputs()],probe.run(None,reference_inputs)))
                for name,value in probe_values.items():
                    assert value.shape==values[name].shape,('shape changed',name)
                    if name in constants:assert np.array_equal(value,values[name],equal_nan=True),('bad constant fold',name)
                actual,seconds=run(native,handle,memory,report,native_inputs)
                errors={name:compare(actual[name],reference[name]) for name in report['outputs']}
                assert memory.used==metadata,('transient leak',memory.used,metadata)
                cases.append(dict(chunk=chunk,seconds=seconds,outputs=errors,peak_bytes=memory.peak,independent_state_progression=True))
                if file.name.startswith('encoder'):
                    for name in base:
                        if name!='x':reference_inputs[name]=reference['new_'+name];native_inputs[name]=actual['new_'+name]
                print(file.name,'C_GRAPH_CHUNK_OK',chunk,round(seconds,3),'peak',memory.peak,flush=True)
        finally:native.glv_asr_graph_close(handle)
        assert memory.used==0
        reports.append(dict(graph=file.name,plan_sha256=report['plan_sha256'],data_sha256=report['data_sha256'],nodes=len(report['nodes']),metadata_bytes=metadata,cases=cases,all_allocations_released=True))
    sources=[root/'app/espdl-quickapp/experimental'/v for v in ('local_asr_graph.c','local_asr_graph.h','local_asr_ops.c','local_asr_ops.h')]
    result=dict(passed=True,scope='Complete C encoder/decoder/joiner graphs with actual SD-layout Q4/Q8 row reads, three independently propagated synthetic-feature chunks; not audio transcription or P4 runtime',asr_ready=False,board_tested=False,rv32_compiled=bool(a.toolchain),graphs=reports,source_sha256={str(v.relative_to(root)):sha(v.read_bytes()) for v in sources})
    (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('ASR_COMPLETE_C_GRAPHS_OK',flush=True)


if __name__=='__main__':main()
