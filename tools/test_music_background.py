#!/usr/bin/env python3
"""Exercise real music workers/parser with deterministic offline HTTP/audio."""
import argparse,subprocess,tempfile,re,urllib.request,urllib.parse
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--apps',type=Path,required=True);p.add_argument('--live-lyrics',action='store_true');a=p.parse_args()
repo=Path(__file__).resolve().parents[1];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
with tempfile.TemporaryDirectory(prefix='music-background-') as tmp:
 root=Path(tmp);(root/'nuttx/audio').mkdir(parents=True)
 (root/'nuttx/config.h').write_text('#pragma once\n')
 (root/'nuttx/audio/audio.h').write_text('#pragma once\nstruct ap_buffer_s;\n')
 source=(desktop/'glass_music_service.c').read_text()
 source=re.sub(r'#if __has_include\("music_credentials.h"\).*?#endif','#define GLASS_MUSIC_API_KEY ""',source,count=1,flags=re.S)
 (root/'music_service_under_test.c').write_text(source)
 pcm=(desktop/'glass_music_pcm.c').read_text()
 (root/'speaker_under_test.c').write_text(pcm[pcm.index('static atomic_uint g_speaker_volume'):pcm.index('int music_pcm_session_lock')])
 flags=['cc','-D_GNU_SOURCE','-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer',
  '-I'+str(root),'-I'+str(desktop),'-I'+str(a.apps/'netutils/cjson/cJSON'),'-DMUSIC_SD_ROOT="'+str(root)+'/"','-DMUSIC_CACHE_ROOT="'+str(root)+'/cache"']
 flags+=['-DMUSIC_LYRICS_ROOT="'+str(root)+'/lyrics"']
 subprocess.run([*flags,str(repo/'app/espdl-quickapp/tests/music_background_host.c'),str(desktop/'glass_music_parse.c'),
  str(desktop/'glass_board_music.c'),str(desktop/'glass_chat_store.c'),
  str(a.apps/'netutils/cjson/cJSON/cJSON.c'),'-lm','-o',str(root/'test')],check=True)
 subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-f','lavfi','-i','sine=frequency=440:duration=16:sample_rate=22050',
  '-codec:a','libmp3lame','-b:a','64k',str(root/'fixture.mp3')],check=True)
 subprocess.run([str(root/'test'),str(root/'fixture.mp3')],check=True,timeout=25)
 if a.live_lyrics:
  url='https://api.lrc.cx/jsonapi?'+urllib.parse.urlencode({'title':'海阔天空','artist':'Beyond','album':'海阔天空'})
  with urllib.request.urlopen(url,timeout=20) as response:
   assert response.status==200
   body=response.read(512*1024+1);assert len(body)<=512*1024
  subprocess.run([str(root/'test'),'--lyrics-response','海阔天空','Beyond'],input=body,check=True,timeout=10)
