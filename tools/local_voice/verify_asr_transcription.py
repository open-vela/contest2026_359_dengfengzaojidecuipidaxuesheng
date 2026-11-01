#!/usr/bin/env python3
"""Run the entire experimental C PCM->Chinese pipeline, with real SD assets.

Only the input WAV reader/reference comparison live in Python. Feature
extraction, graph execution, state progression, greedy decoding and token
conversion happen in C. Host local temporary copies avoid measuring DrvFS
latency as CPU latency; all weights still use the row/file reader.
"""
import argparse,ctypes as ct,hashlib,json,shutil,subprocess,tempfile,time,wave
from pathlib import Path
import numpy as np
from verify_asr_ops import Memory,CANCEL
from evaluate_asr_q4 import recognizer,decode,distance


TOKEN=ct.CFUNCTYPE(ct.c_int,ct.c_void_p,ct.c_uint,ct.c_uint)


class Stats(ct.Structure):_fields_=[('frames',ct.c_uint),('chunks',ct.c_uint),('tokens',ct.c_uint)]
class IOStats(ct.Structure):_fields_=[('bytes',ct.c_uint64),('reads',ct.c_uint64),('seeks',ct.c_uint64)]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--assets',type=Path,required=True);p.add_argument('--packed',type=Path,required=True);p.add_argument('--wav-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--toolchain',type=Path);a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop';exp=root/'app/espdl-quickapp/experimental'
    sources=[exp/v for v in ('local_asr.c','local_asr_graph.c','local_asr_fbank.c','local_asr_ops.c')]+[desktop/'glass_local_model.c',desktop/'glass_local_sha.c']
    lib=out/'asr-transcription.so'
    subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fPIC','-shared','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),*[str(s) for s in sources],'-lm','-o',str(lib)],check=True)
    if a.toolchain:
        for source in sources[:4]:
            subprocess.run([str(a.toolchain/'riscv32-esp-elf-gcc'),'-std=gnu11','-O2','-g','-ffp-contract=off','-march=rv32imafc','-mabi=ilp32f','-fstack-usage','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(desktop),'-c',str(source),'-o',str(out/(source.stem+'-rv32.o'))],check=True)
    native=ct.CDLL(str(lib));native.glv_asr_open.argtypes=[ct.POINTER(ct.c_void_p),ct.c_char_p,ct.POINTER(Memory)];native.glv_asr_close.argtypes=[ct.c_void_p]
    native.glv_asr_transcribe.argtypes=[ct.c_void_p,ct.POINTER(ct.c_int16),ct.c_size_t,ct.c_void_p,ct.c_size_t,TOKEN,ct.c_void_p,ct.POINTER(Stats)]
    native.glv_asr_io_statistics.argtypes=[ct.c_void_p,ct.POINTER(IOStats)]
    native.glv_asr_cached_bytes.argtypes=[ct.c_void_p];native.glv_asr_cached_bytes.restype=ct.c_size_t
    native.glv_alloc.argtypes=[ct.POINTER(Memory),ct.c_size_t];native.glv_alloc.restype=ct.c_void_p;native.glv_free.argtypes=[ct.c_void_p]
    never=CANCEL(lambda _:False);memory=Memory(0,0,12*1024*1024,never,None);engine=ct.c_void_p()
    references={'fp32':recognizer(a.packed/'fp32'),'mixed':recognizer(a.packed/'q4-dequantized')}
    reports=[]
    with tempfile.TemporaryDirectory(prefix='glv-asr-transcribe-') as temp:
        temp=Path(temp)
        for file in a.assets.iterdir():
            if file.suffix in ('.plan','.data') or file.name=='tokens.bin':shutil.copyfile(file,temp/file.name)
        assert native.glv_asr_open(ct.byref(engine),str(temp).encode(),ct.byref(memory))==0
        idle=memory.used
        try:
            for file in sorted(a.wav_dir.glob('*.wav')):
                with wave.open(str(file)) as wav:
                    if wav.getframerate()!=16000:continue
                    assert wav.getnchannels()==1 and wav.getsampwidth()==2
                    pcm=np.frombuffer(wav.readframes(wav.getnframes()),dtype='<i2').copy()
                before=memory.used;input_pointer=native.glv_alloc(ct.byref(memory),pcm.nbytes);output_pointer=native.glv_alloc(ct.byref(memory),4096)
                assert input_pointer and output_pointer;ct.memmove(input_pointer,pcm.ctypes.data,pcm.nbytes)
                tokens=[];first=[None];began=time.monotonic()
                @TOKEN
                def token(_,value,frame):
                    if first[0] is None:first[0]=time.monotonic()-began
                    tokens.append(dict(id=value,frame=frame));return 0
                stats=Stats();io_before=IOStats();native.glv_asr_io_statistics(engine,ct.byref(io_before))
                ret=native.glv_asr_transcribe(engine,ct.cast(input_pointer,ct.POINTER(ct.c_int16)),len(pcm),output_pointer,4096,token,None,ct.byref(stats))
                elapsed=time.monotonic()-began
                assert ret==0,ret
                io_after=IOStats();native.glv_asr_io_statistics(engine,ct.byref(io_after))
                io_counts={key:getattr(io_after,key)-getattr(io_before,key) for key in ('bytes','reads','seeks')};io_counts['packed_cache_bytes']=native.glv_asr_cached_bytes(engine)
                assert io_counts['reads']>0 and io_counts['seeks']<io_counts['reads']/10,io_counts
                text=ct.string_at(output_pointer).decode('utf-8')
                reference={k:decode(v,file) for k,v in references.items()}
                edits=distance(text,reference['mixed']['text']);assert edits==0,(file.name,text,reference)
                reports.append(dict(file=file.name,source_sha256=hashlib.sha256(file.read_bytes()).hexdigest(),text=text,seconds=elapsed,first_token_seconds=first[0],audio_seconds=len(pcm)/16000,peak_bytes=memory.peak,weight_file_io=io_counts,stats=dict(frames=stats.frames,chunks=stats.chunks,tokens=stats.tokens),token_trace=tokens,reference=reference,mixed_reference_edit_distance=edits,fp32_reference_edit_distance=distance(text,reference['fp32']['text'])))
                native.glv_free(input_pointer);native.glv_free(output_pointer);assert memory.used==before==idle
                print('ASR_C_TRANSCRIPT_OK',file.name,round(elapsed,3),text,'peak',memory.peak,flush=True)
        finally:native.glv_asr_close(engine)
    assert memory.used==0
    report=dict(passed=True,scope='Full C 16kHz PCM feature/encoder/decoder/joiner/token pipeline vs official sherpa host reference on two official clips; not P4 microphone, timing or Chinese-quality acceptance',asr_ready=False,board_tested=False,graphs_loaded_from_temporary_host_file_copies=True,engine_glv_alloc_io_counted_and_released=True,memory_scope='C engine metadata, tensors, PCM/output and operator scratch tracked by glv_memory; excludes host reference/NumPy and libc FILE bookkeeping. Actual P4 heap/reserve remains to be measured.',cases=reports,rv32_compiled=bool(a.toolchain),source_sha256={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},assets={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in a.assets.iterdir() if f.suffix in ('.plan','.data') or f.name=='tokens.bin'})
    (out/'VERIFICATION.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
