#!/usr/bin/env python3
"""Freeze the host-verified C ASR port and external evaluation assets.

This is a separate experimental deliverable, never a replacement for the
currently flashed candidate or the boot-verified SD model release.
"""
import argparse,hashlib,json,zipfile
from pathlib import Path


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--artifacts',type=Path,required=True);p.add_argument('--revision',type=int,default=1);a=p.parse_args();assert a.revision>0
    root=Path(__file__).resolve().parents[2];r=a.artifacts.resolve();graph=r/'asr-c-graph7';audit=r/'asr-native-audit3/VERIFICATION.json'
    checked=json.loads(audit.read_text(encoding='utf-8'));assert checked['passed'] and not checked['board_tested']
    for file,digest in checked['sources'].items():assert sha(root/file)==digest
    for file,digest in checked['evidence'].items():assert sha(r/file)==digest
    cases=json.loads((r/'asr-c-transcription1/VERIFICATION.json').read_text(encoding='utf-8'))
    assert cases['passed'] and len(cases['cases'])==2 and all(v['mixed_reference_edit_distance']==0 for v in cases['cases'])
    for file,digest in cases['assets'].items():assert sha(graph/file)==digest
    folder='local-voice/models/zipformer-14m-mixed-eval-20261031/asr';files={}
    def add(name,path):
        path=Path(path);assert path.is_file() and '.private' not in path.parts and name not in files;files[name]=path
    assets=sorted([v for v in graph.iterdir() if v.suffix in ('.plan','.data') or v.name=='tokens.bin'])
    assert len(assets)==7
    for source in assets:add('sdcard/'+folder+'/'+source.name,source)
    work=r/('asr-delivery'+str(a.revision));work.mkdir(exist_ok=False)
    manifest=dict(format='GLV-ASR-EXPERIMENTAL-1',version='zipformer-14m-mixed-eval-20261031',source='Official Chinese Zipformer 14M 2023-02-23',source_archive_sha256='2cbd71b640d9c37d3784f29367333a4577b0398b62e9deeed418170b081cba8b',source_url='https://k2-fsa.github.io/sherpa/onnx/pretrained_models/online-transducer/zipformer-transducer-models.html#csukuangfj-sherpa-onnx-streaming-zipformer-zh-14m-2023-02-23-chinese',runtime_compatibility=dict(plan='GLA1 ABI 1',tokens='GLT1 ABI 1',profile='N=1 / 16kHz s16 / 80-mel / T39 shift32 / decoder2 / greedy blank0 unk2',quantization='group32 symmetric Q4 feed-forward; Q8 sensitive matrices; original FP32/int constants; output-row padding; little-endian',caller_manifest_check_required_before_open=True),asr_ready=False,board_tested=False,files=[dict(path=v.name,bytes=v.stat().st_size,sha256=sha(v)) for v in assets],limitations=['Not integrated into current candidate 7 firmware or released model manifest','Only two official 16kHz clips passed full C text comparison; not Chinese quality acceptance','Host glv_memory peak excludes libc FILE bookkeeping; native total/largest-block/reserve tests remain','No native microphone/ownership/UI/wake-to-ASR integration yet','8kHz resampling is not implemented; experimental mixed-precision reference retains two-character differences on the third 8kHz clip'])
    notices={'model-README.md':r/'asr-mixed-rows-evaluation/fp32/README.md','Apache-2.0-LICENSE.txt':r/'asr-fbank-reference/python/kaldi_native_fbank-1.22.5.dist-info/licenses/LICENSE'}
    for name,source in notices.items():add('sdcard/'+folder+'/notices/'+name,source)
    manifest['notices']=[dict(path='notices/'+name,bytes=source.stat().st_size,sha256=sha(source)) for name,source in notices.items()]
    path=work/'manifest.json';path.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8');add('sdcard/'+folder+'/manifest.json',path)
    add('THIRD_PARTY.md',root/'THIRD_PARTY.md');add('LOCAL-VOICE.md',root/'app/espdl-quickapp/LOCAL-VOICE.md')
    for name in ('TOKEN-SOURCE.json','decoder-epoch-99-avg-1.json','encoder-epoch-99-avg-1.json','joiner-epoch-99-avg-1.json'):add('export/'+name,graph/name)
    add('evidence/native-audit.json',audit)
    for rel in checked['evidence']:add('evidence/'+rel,r/rel)
    for source in (root/'app/espdl-quickapp/experimental').iterdir():
        if source.is_file():add('source/experimental/'+source.name,source)
    for name in ('local_asr_ops_host.c','local_asr_graph_host.c','local_asr_host.c'):add('source/tests/'+name,root/'app/espdl-quickapp/tests'/name)
    for source in (root/'tools/local_voice').glob('*asr*.py'):add('source/tools/'+source.name,source)
    for source in (r/'asr-c-transcription1').glob('*.su'):add('evidence/rv32-stack/'+source.name,source)
    text='''Zipformer 14M 实验性 C 转写资源包（主机验证阶段）

此包不是可烧录固件。当前开发板仍运行已验收画面/背光的候选 7；APP 的
“离线转写未就绪”提示保持。不要覆盖或删除原模型及 SD 文件。

sdcard/ 目录展示未来独立安装位置：
/sdcard/local-voice/models/zipformer-14m-mixed-eval-20261031/asr/
当前未自动安装；该 manifest 只供实验性 ASR 运行时使用，不替换已验证的
EdgeIdiot/V2/EI 主模型清单。正式加载之前必须先按 manifest 校验每个文件
长度、SHA-256 和运行时版本。GLA1 读取器另检查权重 SHA 和计划边界。

本轮主机完成：完整 C 特征、三张图、状态、实际 Q4/Q8 文件读取、贪心解码
和中文字表；两段官方 16 kHz 语音文字均与同精度参考一致。ASAN/UBSAN 检查
取消、错误、重入、重复运行、低内存及损坏资产。RV32 对象已编译，但尚无
包含本运行时的最终固件链接或实板识别结果。主机时间不是 P4 时间。

source/experimental/README.txt 解释范围和剩余工作。source/tools/ 是当前
仓库工具的审阅副本；重新验证请在完整 project 工作树运行，保留原模型来源
和第三方许可。实验输出不能被标作真人中文识别质量通过。

下一步：接入常驻服务和唯一麦克风所有权，构建新的独立候选，完成最终链接/
分区检查，再做板上文件转写、取消和内存预留验证；随后接 UI、真人录音及
全局唤醒 → ASR → EdgeIdiot → V2 完整链路。
'''
    path=work/'INSTALL.txt';path.write_text(text,encoding='utf-8');add('INSTALL.txt',path)
    path=work/'DELIVERY-MANIFEST.json';delivery=dict(plan_complete=False,asr_ready=False,board_tested=False,files=[dict(path=n,bytes=f.stat().st_size,sha256=sha(f)) for n,f in sorted(files.items())]);path.write_text(json.dumps(delivery,indent=2)+'\n',encoding='utf-8');add('DELIVERY-MANIFEST.json',path)
    target=r/('local-voice-asr-experimental-20261031'+('' if a.revision==1 else '-r'+str(a.revision))+'.zip');assert not target.exists()
    with zipfile.ZipFile(target,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for name,source in sorted(files.items()):z.write(source,name)
    with zipfile.ZipFile(target) as z:
        assert z.testzip() is None
        for entry in delivery['files']:assert hashlib.sha256(z.read(entry['path'])).hexdigest()==entry['sha256']
    result=dict(file=target.name,bytes=target.stat().st_size,sha256=sha(target),entries=len(files),asr_ready=False,board_tested=False,plan_complete=False)
    (r/('ASR-DELIVERY'+str(a.revision)+'.json')).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('ASR_EXPERIMENTAL_DELIVERY_VERIFIED',result['bytes'])


if __name__=='__main__':main()
