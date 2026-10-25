#!/usr/bin/env python3
"""Generate a versioned synthetic Mandarin/English wake-word corpus.

Only phrase text is sent to Edge TTS. No microphone recordings are uploaded.
This corpus is a bootstrap, not a substitute for held-out human recordings.
"""
import argparse,asyncio,hashlib,json,shutil,subprocess
from pathlib import Path
import edge_tts

VOICES={
 'train':['zh-CN-XiaoxiaoNeural','zh-CN-YunxiNeural','zh-CN-YunjianNeural','zh-CN-YunyangNeural',
          'zh-TW-HsiaoChenNeural','zh-TW-YunJheNeural'],
 'validation':['zh-CN-XiaoyiNeural','zh-CN-YunxiaNeural'],
 'test':['zh-CN-liaoning-XiaobeiNeural','zh-CN-shaanxi-XiaoniNeural']}
NEGATIVES=[
 '你好','OpenVela','你好 Open','你好 Vela','你好小爱','你好小智','你好朋友',
 '打开 OpenVela','关闭 OpenVela','再见 OpenVela','OpenVela 你好','我喜欢 OpenVela',
 '你好 OpenAI','你好 Opera','你好 OpenValue','你好 OpenWeather','你好贝拉',
 '您好，请问有什么需要帮助的','打开灯光','关闭灯光','打开窗帘','关闭窗帘','打开风扇',
 '关闭风扇','返回桌面','停止聆听','现在几点钟','今天天气怎么样','把声音调小一点',
 '请打开相机','开始录制视频','停止录像','你好，欢迎使用智能家居','我们明天一起出门',
 '这个功能很好用','系统正在更新，请稍候','播放音乐','音量大一点','请打开设备列表',
 '不用了谢谢','我想看一下照片','Hello world','Open the window','Hello Vela',
 '你好，欧拉','你好，维拉','你好，打开这个','Hello Open','Good morning',
 '在这里我们介绍开源系统','我现在正在说话','请不要唤醒设备','欢迎回来','谢谢大家',
 '这里没有任何指令','你好，朋友们大家好','你好，好久不见','Open source operating system']

POSITIVES=['你好 OpenVela','你好，Open Vela','你好，openvela。']
EXPANDED_POSITIVES=POSITIVES+['你好，Open-Vela','你好，欧喷维拉','你好，欧朋维拉']
EXPANDED_NEGATIVES=[
 '你好 OpenHarmony','你好 OpenClaw','你好 OpenCore','你好 OpenVPN',
 '你好 OpenCV','你好 OpenWrt','你好 Open Vino','你好 Open World',
 '你好，欧朋浏览器','你好，欧派橱柜','你好，欧文先生','你好，维拉同学',
 '你好，贝拉','你好，艾拉','你好，奥拉','你好，欢迎光临',
 '你好，朋友','你好，今天怎么样','欧喷维拉','欧朋维拉',
 '打开欧喷维拉','关闭欧喷维拉','介绍一下 OpenVela','这个系统叫 OpenVela',
 '欢迎使用 OpenVela','请说出唤醒词','现在不要开始录音','听到声音后请返回',
 '我正在观看新闻','这个视频介绍操作系统','大家好，欢迎收看节目','请不要打断我的讲话']
EXPANDED_VOICES={
 'train':VOICES['train']+['zh-TW-HsiaoYuNeural','zh-HK-WanLungNeural'],
 'validation':VOICES['validation']+['zh-HK-HiuGaaiNeural'],
 'test':VOICES['test']+['zh-HK-HiuMaanNeural']}

async def build(output,expanded=False,reuse_corpus=None,inventory=None,concurrency=4):
    output.mkdir(parents=True,exist_ok=True);rows=[];sem=asyncio.Semaphore(concurrency)
    voices=EXPANDED_VOICES if expanded else VOICES
    if expanded and inventory is None:raise ValueError('Expanded voices require a frozen --voice-inventory')
    voice_locales={}
    if inventory:
        inventory_rows=json.loads(inventory.read_text(encoding='utf-8'))
        available={v['ShortName'] for v in inventory_rows}
        voice_locales={v['ShortName']:v['Locale'] for v in inventory_rows}
        missing=set(sum(voices.values(),[]))-available
        if missing:raise ValueError('Voices absent from the frozen inventory: '+str(sorted(missing)))
    reuse={};reuse_digest=None
    if reuse_corpus:
        source_manifest=reuse_corpus/'corpus.json';data=source_manifest.read_bytes()
        reuse_digest=hashlib.sha256(data).hexdigest()
        for row in json.loads(data)['records']:
            reuse[(row['speaker'],row['text'],row['rate'])]=row
        for split in ['validation','test']:
            old={r['speaker'] for r in reuse.values() if r['split']==split}
            if not old<=set(voices[split]) or old&set(voices['train']):
                raise ValueError('Existing held-out speakers must retain their split')
    progress=output/'corpus-progress.json'
    def save_progress():
        progress.write_text(json.dumps({'complete':False,'records':rows},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    reused=0
    async def one(split,voice,text,label,rate):
        nonlocal reused
        key=hashlib.sha256(json.dumps([voice,text,rate],ensure_ascii=False).encode()).hexdigest()[:20]
        mp3=output/(key+'.mp3');pcm=output/(key+'.pcm')
        async with sem:
            original=reuse.get((voice,text,rate))
            if original:
                if original['split']!=split or original['label']!=label:
                    raise ValueError('Cached corpus label/split mismatch')
                source=reuse_corpus/original['pcm'];raw=source.read_bytes()
                if hashlib.sha256(raw).hexdigest()!=original['sha256']:
                    raise ValueError('Corrupt cached PCM: '+str(source))
                if pcm.resolve()!=source.resolve():shutil.copyfile(source,pcm)
                reused+=1
            elif not pcm.exists():
                for attempt in range(5):
                    try:
                        await asyncio.wait_for(edge_tts.Communicate(text,voice,rate=rate).save(str(mp3)),90);break
                    except Exception as error:
                        print('WAKE_TTS_RETRY',voice,key,attempt+1,type(error).__name__,flush=True)
                        if attempt==4:raise
                        await asyncio.sleep(2+attempt*2)
                subprocess.run(['ffmpeg','-v','error','-y','-i',str(mp3),'-ar','16000','-ac','1','-f','s16le',str(pcm)],check=True)
            data=pcm.read_bytes()
            if len(data)<3200 or len(data)%2:raise ValueError('Invalid TTS output')
        rows.append({'split':split,'speaker':voice,'text':text,'label':label,'rate':rate,
          'pcm':pcm.name,'sha256':hashlib.sha256(data).hexdigest(),'samples':len(data)//2,
          'source':'synthetic Edge TTS','source_type':'synthetic','locale':voice_locales.get(voice,'-'.join(voice.split('-')[:2]))})
        if len(rows)%25==0:
            save_progress();print('WAKE_CORPUS',len(rows),'reused',reused,flush=True)
    tasks=[]
    for split,speakers in voices.items():
        for voice in speakers:
            for text in EXPANDED_POSITIVES if expanded else POSITIVES:
                for rate in (['-25%','-15%','+0%','+15%','+25%'] if expanded else ['-15%','+0%','+15%']):
                    tasks.append(one(split,voice,text,1,rate))
            for text in NEGATIVES+(EXPANDED_NEGATIVES if expanded else []):tasks.append(one(split,voice,text,0,'+0%'))
    await asyncio.gather(*tasks)
    rows.sort(key=lambda r:(r['split'],r['speaker'],r['pcm']))
    result={'format':1,'wake_phrase':'你好 openvela','sample_rate':16000,'synthetic_only':True,
      'split_policy':'disjoint TTS voices before augmentation; human evaluation still required','voices':voices,'records':rows,
      'expanded':expanded,'new_human_recordings':0,'reuse_corpus_sha256':reuse_digest,'reused_clips':reused,
      'voice_inventory_sha256':hashlib.sha256(inventory.read_bytes()).hexdigest() if inventory else None,
      'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
      'unique_audio_sha256_count':len({row['sha256'] for row in rows}),
      'new_independent_speakers':{split:sorted(set(voices[split])-set(VOICES[split])) for split in voices},
      'previous_validation_test_have_development_history':True,
      'new_holdout_policy':'New validation voices calibrate/choose candidates; new test voice is reserved until final evaluation'}
    (output/'corpus.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    progress.write_text(json.dumps({'complete':True,'records':len(rows),'reused':reused})+'\n',encoding='utf-8')
    print('WAKE_CORPUS_READY',len(rows),'reused',reused,flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--expanded',action='store_true',help='Frozen 8/3/3 voice corpus with six pronunciations and five rates')
    p.add_argument('--reuse-corpus',type=Path,help='Hash-verified old corpus; retain all existing holdout speakers')
    p.add_argument('--voice-inventory',type=Path,help='JSON returned by edge_tts.list_voices')
    p.add_argument('--concurrency',type=int,default=4)
    a=p.parse_args()
    if not 1<=a.concurrency<=8:p.error('concurrency must be 1..8')
    asyncio.run(build(a.output,a.expanded,a.reuse_corpus,a.voice_inventory,a.concurrency))
