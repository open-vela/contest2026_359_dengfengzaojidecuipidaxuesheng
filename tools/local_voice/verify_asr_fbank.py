#!/usr/bin/env python3
"""Check fixed C 16-kHz features against kaldi-native-fbank, including edges."""
import argparse,ctypes as ct,hashlib,json,subprocess,wave
from pathlib import Path
import numpy as np
import kaldi_native_fbank as knf
from verify_asr_ops import Memory,CANCEL,F

FRAME=ct.CFUNCTYPE(ct.c_int,ct.c_void_p,F,ct.c_uint)


def reference(pcm,tail=0):
    opts=knf.FbankOptions();opts.frame_opts.dither=0;opts.frame_opts.snip_edges=False
    opts.mel_opts.num_bins=80;opts.mel_opts.high_freq=-400
    fbank=knf.OnlineFbank(opts)
    samples=np.concatenate((pcm.astype(np.float32)/32768,np.zeros(tail,dtype=np.float32)))
    fbank.accept_waveform(16000,samples);fbank.input_finished()
    return np.array([fbank.get_frame(i) for i in range(fbank.num_frames_ready)],dtype=np.float32).reshape(-1,80)


def load(native):
    native.glv_asr_fbank_create.argtypes=[ct.POINTER(ct.c_void_p),ct.POINTER(Memory)]
    native.glv_asr_fbank_destroy.argtypes=[ct.c_void_p]
    native.glv_asr_fbank_pcm.argtypes=[ct.c_void_p,ct.POINTER(ct.c_int16),ct.c_size_t,ct.c_size_t,FRAME,ct.c_void_p]
    return native


def features(native,memory,handle,pcm,tail=0):
    result=[]
    @FRAME
    def frame(_,data,count):
        assert count==80;result.append(np.ctypeslib.as_array(data,shape=(80,)).copy());return 0
    pcm=np.ascontiguousarray(pcm,dtype=np.int16)
    ret=native.glv_asr_fbank_pcm(handle,pcm.ctypes.data_as(ct.POINTER(ct.c_int16)),len(pcm),tail,frame,None);assert ret==0,ret
    return np.asarray(result,dtype=np.float32).reshape(-1,80)


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--wav-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--toolchain',type=Path);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop';exp=root/'app/espdl-quickapp/experimental';source=exp/'local_asr_fbank.c'
    lib=out/'asr-fbank.so'
    subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fPIC','-shared','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),str(source),str(desktop/'glass_local_model.c'),str(desktop/'glass_local_sha.c'),'-lm','-o',str(lib)],check=True)
    if a.toolchain:
        subprocess.run([str(a.toolchain/'riscv32-esp-elf-gcc'),'-std=gnu11','-O2','-ffp-contract=off','-march=rv32imafc','-mabi=ilp32f','-fstack-usage','-I'+str(desktop),'-c',str(source),'-o',str(out/'fbank-rv32.o')],check=True)
    native=load(ct.CDLL(str(lib)));never=CANCEL(lambda _:False);memory=Memory(0,0,12*1024*1024,never,None);handle=ct.c_void_p()
    assert native.glv_asr_fbank_create(ct.byref(handle),ct.byref(memory))==0;idle=memory.used
    rng=np.random.default_rng(319);cases={}
    for size in (0,1,79,80,81,159,160,161,200,399,400,401,512,12345):
        cases['random-'+str(size)]=rng.integers(-32768,32768,size,dtype=np.int16)
    cases['silence']=np.zeros(16000,dtype=np.int16);cases['constant']=np.full(1600,20000,dtype=np.int16)
    impulse=np.zeros(2400,dtype=np.int16);impulse[::317]=32767;cases['impulse']=impulse
    cases['sine']=(np.sin(np.arange(16000)*2*np.pi*1000/16000)*32767).astype(np.int16)
    cases['low-amplitude']=rng.integers(-2,3,16000,dtype=np.int16)
    for file in sorted(a.wav_dir.glob('*.wav')):
        with wave.open(str(file)) as f:
            if f.getframerate()!=16000:continue
            assert f.getsampwidth()==2 and f.getnchannels()==1
            cases[file.name]=np.frombuffer(f.readframes(f.getnframes()),dtype='<i2').copy()
    reports=[]
    try:
        for name,pcm in cases.items():
            expected=reference(pcm);actual=features(native,memory,handle,pcm)
            assert actual.shape==expected.shape,(name,actual.shape,expected.shape)
            maximum=float(np.abs(actual-expected).max(initial=0));rms=float(np.sqrt(np.square(actual-expected).mean())) if actual.size else 0
            assert np.allclose(actual,expected,rtol=1e-4,atol=1e-3),(name,maximum,rms)
            assert memory.used==idle
            reports.append(dict(case=name,frames=len(actual),max_abs=maximum,rms=rms))
            print('FBANK_OK',name,len(actual),maximum,flush=True)
        # Cancellation and callback error must leave reusable, bounded scratch.
        stop=CANCEL(lambda _:True);memory.cancelled=stop
        reject=FRAME(lambda *_:-123)
        pcm=np.zeros(400,dtype=np.int16);pointer=pcm.ctypes.data_as(ct.POINTER(ct.c_int16))
        assert native.glv_asr_fbank_pcm(handle,pointer,len(pcm),0,reject,None)==-125
        memory.cancelled=never
        assert native.glv_asr_fbank_pcm(handle,pointer,len(pcm),0,reject,None)==-123
        assert memory.used==idle
    finally:native.glv_asr_fbank_destroy(handle)
    assert memory.used==0
    refs=Path(knf.__file__).parent.parent
    reference_files={str(f.relative_to(refs)):hashlib.sha256(f.read_bytes()).hexdigest() for f in refs.rglob('*.so')}
    result=dict(passed=True,scope='Independent C fixed 16kHz s16 fbank vs kaldi-native-fbank 1.22.5; no 8k resampler or board capture',asr_ready=False,reference_libraries=reference_files,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),cases=reports,cancel_callback_and_reclamation=True,persistent_scratch_bytes=idle,peak_bytes=memory.peak,rv32_compiled=bool(a.toolchain))
    (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
