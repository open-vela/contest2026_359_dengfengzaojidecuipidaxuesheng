#!/usr/bin/env python3
"""Check experimental C MatMul/Conv against every actual fixed-graph node.

Inputs/outputs come from host ONNX probes, not the C graph executor (which is
not implemented). Python owns the tensors; scratch accounting is separate.
"""
import argparse, ctypes as ct, hashlib, json, subprocess
from pathlib import Path
import numpy as np
import onnx
from onnx import numpy_helper
import onnxruntime as ort

F = ct.POINTER(ct.c_float)
READ = ct.CFUNCTYPE(ct.c_int, ct.c_void_p, ct.c_uint, F, ct.c_uint)
CANCEL = ct.CFUNCTYPE(ct.c_bool, ct.c_void_p)


class Memory(ct.Structure):
    _fields_ = [('used',ct.c_size_t),('peak',ct.c_size_t),('limit',ct.c_size_t),('cancelled',CANCEL),('opaque',ct.c_void_p)]


class Rows(ct.Structure):
    _fields_ = [('read',READ),('opaque',ct.c_void_p)]


class Conv(ct.Structure):
    _fields_ = [(key,ct.c_uint) for key in ('channels','height','width','outputs','kernel_h','kernel_w','groups','stride_h','stride_w','dilation_h','dilation_w','top','left','bottom','right')]


class Shape(ct.Structure):
    _fields_ = [('rank',ct.c_uint),('dims',ct.c_uint32*8)]


def shape(array):
    assert array.ndim<=8
    return Shape(array.ndim,(ct.c_uint32*8)(*array.shape))


def ptr(array):
    return array.ctypes.data_as(F) if array is not None else F()


def reader(matrix):
    matrix = np.ascontiguousarray(matrix, dtype=np.float32)
    @READ
    def read(_, row, dest, count):
        if row >= len(matrix) or count != matrix.shape[1]: return -22
        np.ctypeslib.as_array(dest,shape=(count,))[:] = matrix[row]
        return 0
    return Rows(read,None), read


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--graphs',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--toolchain',type=Path);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop';experimental=root/'app/espdl-quickapp/experimental'
    source=experimental/'local_asr_ops.c';lib=out/'asr-ops.so'
    unit=out/'asr-ops-lifecycle'
    subprocess.run(['gcc','-std=gnu11','-O1','-g','-ffp-contract=off','-fno-pie','-no-pie','-fsanitize=address,undefined','-I'+str(desktop),'-I'+str(experimental),str(source),str(desktop/'glass_local_model.c'),str(desktop/'glass_local_sha.c'),str(root/'app/espdl-quickapp/tests/local_asr_ops_host.c'),'-lm','-o',str(unit)],check=True)
    result=subprocess.run([str(unit)],check=True,capture_output=True,text=True);(out/'lifecycle.log').write_text(result.stdout+result.stderr,encoding='utf-8');print(result.stdout.strip(),flush=True)
    if a.toolchain:
        target=out/'asr-ops-rv32.o'
        subprocess.run([str(a.toolchain/'riscv32-esp-elf-gcc'),'-std=gnu11','-march=rv32imafc','-mabi=ilp32f','-O2','-g','-ffp-contract=off','-fstack-usage','-I'+str(desktop),'-c',str(source),'-o',str(target)],check=True)
        disassembly=subprocess.check_output([str(a.toolchain/'riscv32-esp-elf-objdump'),'-d',str(target)],text=True)
        assert 'fmul.s' in disassembly and 'fadd.s' in disassembly and 'fmadd.s' not in disassembly
        (out/'RV32-DISASSEMBLY.txt').write_text(disassembly,encoding='utf-8')
    subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fPIC','-shared','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),str(source),str(desktop/'glass_local_model.c'),str(desktop/'glass_local_sha.c'),'-lm','-o',str(lib)],check=True)
    native=ct.CDLL(str(lib));native.glv_asr_matmul.argtypes=[ct.POINTER(Memory),F,ct.c_size_t,F,ct.c_size_t,ct.c_uint,ct.c_uint,ct.c_uint,ct.POINTER(Rows)]
    native.glv_asr_conv.argtypes=[ct.POINTER(Memory),F,ct.c_size_t,F,ct.c_size_t,F,ct.c_size_t,ct.POINTER(Conv),ct.POINTER(Rows)]
    native.glv_asr_unary.argtypes=[ct.POINTER(Memory),ct.c_int,F,F,ct.c_size_t]
    native.glv_asr_binary.argtypes=[ct.POINTER(Memory),ct.c_int,F,ct.c_size_t,ct.POINTER(Shape),F,ct.c_size_t,ct.POINTER(Shape),F,ct.c_size_t]
    native.glv_asr_softmax.argtypes=[ct.POINTER(Memory),F,ct.c_size_t,ct.POINTER(Shape),ct.c_uint,F,ct.c_size_t]
    native.glv_asr_reduce.argtypes=[ct.POINTER(Memory),F,ct.c_size_t,ct.POINTER(Shape),ct.c_uint,ct.c_bool,F,ct.c_size_t]
    native.glv_asr_gemm.argtypes=[ct.POINTER(Memory),F,ct.c_size_t,F,ct.c_size_t,ct.c_uint,ct.c_uint,ct.c_uint,ct.POINTER(Rows),ct.c_float,ct.c_float,F,ct.c_size_t,ct.POINTER(Shape)]
    never=CANCEL(lambda _:False);memory=Memory(0,0,12*1024*1024,never,None)
    results=[];rng=np.random.default_rng(714)
    for file in sorted(a.graphs.glob('*.onnx')):
        model=onnx.load(file);inferred=onnx.shape_inference.infer_shapes(model)
        info={v.name:v for v in [*inferred.graph.input,*inferred.graph.output,*inferred.graph.value_info]}
        wanted=list(dict.fromkeys(n for node in model.graph.node for n in node.output if n));del inferred.graph.output[:]
        for name in wanted:inferred.graph.output.append(info[name])
        opts=ort.SessionOptions();opts.intra_op_num_threads=opts.inter_op_num_threads=1;opts.graph_optimization_level=ort.GraphOptimizationLevel.ORT_DISABLE_ALL
        session=ort.InferenceSession(inferred.SerializeToString(),opts,providers=['CPUExecutionProvider']);inputs={}
        for item in session.get_inputs():
            input_shape=[1 if v=='N' else v for v in item.shape];assert all(isinstance(v,int) and v>0 for v in input_shape)
            dtype=np.float32 if item.type=='tensor(float)' else np.int64
            inputs[item.name]=rng.standard_normal(input_shape).astype(np.float32) if dtype==np.float32 and not item.name.startswith('cached') else np.zeros(input_shape,dtype=dtype)
        tensors={v.name:numpy_helper.to_array(v) for v in model.graph.initializer};tensors.update(inputs);tensors.update(zip(wanted,session.run(None,inputs)))
        unary={'Neg':0,'Relu':1,'Sigmoid':2,'Tanh':3,'Reciprocal':4};binary={'Add':0,'Sub':1,'Mul':2,'Div':3,'Pow':4}
        for node in model.graph.node:
            if node.op_type not in ('MatMul','Gemm','Conv','Softmax','ReduceMean','ReduceSum',*unary,*binary):continue
            x=np.ascontiguousarray(tensors[node.input[0]]);expected=tensors[node.output[0]]
            if tensors[node.input[0]].ndim==0:x=x.reshape(())
            if x.dtype!=np.float32:continue
            w=tensors[node.input[1]] if len(node.input)>1 else None
            if node.op_type in binary or node.op_type in ('MatMul','Gemm','Conv'):
                if w.dtype!=np.float32:continue
            attrs={attr.name:onnx.helper.get_attribute_value(attr) for attr in node.attribute}
            actual=np.empty_like(expected)
            if node.op_type=='MatMul':
                assert x.ndim>=2 and w.ndim>=2
                batch=np.broadcast_shapes(x.shape[:-2],w.shape[:-2]);xx=np.broadcast_to(x,batch+x.shape[-2:]);ww=np.broadcast_to(w,batch+w.shape[-2:])
                for index in np.ndindex(batch):
                    src=np.ascontiguousarray(xx[index]);dest=np.ascontiguousarray(actual[index]);m,k=src.shape;n=ww[index].shape[-1]
                    rows,keep=reader(ww[index].T)
                    ret=native.glv_asr_matmul(ct.byref(memory),ptr(src),src.size,ptr(dest),dest.size,m,k,n,ct.byref(rows));assert ret==0,(node.name,ret)
                    actual[index]=dest
            elif node.op_type=='Gemm':
                src=np.ascontiguousarray(x.T if attrs.get('transA',0) else x);weight=w if attrs.get('transB',0) else w.T
                rows,keep=reader(weight);m,k=src.shape;n=weight.shape[0]
                bias=np.ascontiguousarray(tensors[node.input[2]]) if len(node.input)>2 else None
                if bias is not None and tensors[node.input[2]].ndim==0:bias=bias.reshape(())
                sb=shape(bias) if bias is not None else Shape()
                ret=native.glv_asr_gemm(ct.byref(memory),ptr(src),src.size,ptr(actual),actual.size,m,k,n,ct.byref(rows),attrs.get('alpha',1.),attrs.get('beta',1.),ptr(bias),0 if bias is None else bias.size,ct.byref(sb));assert ret==0,(node.name,ret)
            elif node.op_type=='Conv':
                assert attrs.get('auto_pad',b'NOTSET')==b'NOTSET'
                assert x.ndim in (3,4) and x.shape[0]==1
                ndim=x.ndim-2;stride=attrs.get('strides',[1]*ndim);dilation=attrs.get('dilations',[1]*ndim);pads=attrs.get('pads',[0]*(ndim*2))
                if ndim==1:h,wi,kh,kw,sh,sw,dh,dw,top,left,bottom,right=1,x.shape[2],1,w.shape[2],1,stride[0],1,dilation[0],0,pads[0],0,pads[1]
                else:h,wi=x.shape[2:];kh,kw=w.shape[2:];sh,sw=stride;dh,dw=dilation;top,left,bottom,right=pads
                config=Conv(x.shape[1],h,wi,w.shape[0],kh,kw,attrs.get('group',1),sh,sw,dh,dw,top,left,bottom,right)
                rows,keep=reader(w.reshape(w.shape[0],-1));src=np.ascontiguousarray(x);bias=np.ascontiguousarray(tensors[node.input[2]]) if len(node.input)>2 else None
                ret=native.glv_asr_conv(ct.byref(memory),ptr(src),src.size,ptr(bias),0 if bias is None else bias.size,ptr(actual),actual.size,ct.byref(config),ct.byref(rows));assert ret==0,(node.name,ret)
            elif node.op_type in unary:
                ret=native.glv_asr_unary(ct.byref(memory),unary[node.op_type],ptr(x),ptr(actual),x.size);assert ret==0,(node.name,ret)
            elif node.op_type in binary:
                z=np.ascontiguousarray(w)
                if w.ndim==0:z=z.reshape(())
                sx,sz=shape(x),shape(z)
                ret=native.glv_asr_binary(ct.byref(memory),binary[node.op_type],ptr(x),x.size,ct.byref(sx),ptr(z),z.size,ct.byref(sz),ptr(actual),actual.size);assert ret==0,(node.name,ret)
            elif node.op_type=='Softmax':
                sx=shape(x);axis=attrs.get('axis',-1)%x.ndim
                ret=native.glv_asr_softmax(ct.byref(memory),ptr(x),x.size,ct.byref(sx),axis,ptr(actual),actual.size);assert ret==0,(node.name,ret)
            else:
                sx=shape(x);axes=list(w) if w is not None else attrs.get('axes',list(range(x.ndim)))
                if not axes and not attrs.get('noop_with_empty_axes',0):axes=list(range(x.ndim))
                mask=sum(1<<(int(axis)%x.ndim) for axis in set(axes))
                ret=native.glv_asr_reduce(ct.byref(memory),ptr(x),x.size,ct.byref(sx),mask,node.op_type=='ReduceMean',ptr(actual),actual.size);assert ret==0,(node.name,ret)
            assert memory.used==0,(node.name,memory.used)
            finite=np.isfinite(expected);difference=(actual[finite]-expected[finite]).ravel();error=float(np.abs(difference).max(initial=0));relative=float(np.linalg.norm(difference)/max(float(np.linalg.norm(expected[finite].ravel())),1e-12))
            assert np.allclose(actual,expected,rtol=2e-5,atol=1e-5,equal_nan=True) and (relative<2e-6 or error<1e-6),(file.name,node.name,error,relative)
            if node.op_type in ('MatMul','Gemm','Conv'):assert error<1e-4 and relative<2e-6
            results.append(dict(graph=file.name,op=node.op_type,name=node.name,shape=list(expected.shape),max_abs=error,relative_l2=relative))
        print(file.name,'C_OPS_OK',sum(v['graph']==file.name for v in results),flush=True)
    report=dict(passed=True,scope='All MatMul/Conv nodes on seeded host ONNX intermediate inputs; not a C graph runtime or board ASR',scratch_peak_bytes=memory.peak,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),sanitizer_lifecycle_passed=True,rv32_object_compiled=bool(a.toolchain),cases=results,asr_ready=False)
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('ASR_NATIVE_OPS_OK',len(results),flush=True)


if __name__=='__main__':main()
