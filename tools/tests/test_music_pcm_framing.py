"""Exercise the production PCM framing functions with a fake audio queue."""
from pathlib import Path
import hashlib, re, subprocess, tempfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from render_all_ui import function
repo=Path(__file__).resolve().parents[2]
source=repo/'app/espdl-quickapp/overlay/apps/system/desktop/glass_music_pcm.c'
text=source.read_text()
parts=[text[text.index('static atomic_uint g_speaker_volume'):text.index('int music_pcm_session_lock')],
       text[text.index('static int pcm_message'):text.index('static int pcm_open')],
       text[text.index('static int pcm_submit'):text.index('int music_pcm_pause')],
       text[text.index('int music_pcm_drain'):text.index('void music_pcm_close')]]
video=(repo/'app/bilibili_live/local_video_ui.c').read_text()
codec=(repo/'app/espdl-quickapp/overlay/nuttx/drivers/audio/es8311.c').read_text()
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <sys/types.h>
#define MUSIC_PCM_BUFFERS 4
#define PCM_TEST_CAPACITY 1152
#define AUDIO_MSG_DEQUEUE 1
#define AUDIO_MSG_IOERR 2
#define AUDIO_MSG_COMPLETE 3
#define AUDIOIOC_ENQUEUEBUFFER 1
#define AUDIOIOC_START 2
#define AUDIOIOC_CONFIGURE 3
#define AUDIO_TYPE_FEATURE 1
#define AUDIO_FU_VOLUME 1
struct audio_caps_desc_s {struct {unsigned ac_len,ac_type;union {unsigned hw;} ac_format;union {unsigned hw[4];} ac_controls;} caps;};
struct ap_buffer_s {unsigned nbytes,curbyte,flags;unsigned char *samp;};
struct audio_msg_s {int msg_id;union {void *ptr;} u;};
struct audio_buf_desc_s {unsigned numbytes;union {struct ap_buffer_s *buffer;} u;};
struct music_pcm {int fd,mq;struct ap_buffer_s *buffers[4];bool queued[4],started;
 unsigned rate,pending,played_samples,submitted_samples[4],staged_frames,staged_index;};
static struct ap_buffer_s *fifo[1024];static unsigned head,tail,used,starts;
static int message_id=AUDIO_MSG_DEQUEUE;
static int16_t captured[65536];
static int ioctl(int fd,unsigned request,...);
static ssize_t mq_timedreceive(int mq,char *out,size_t size,void *priority,const struct timespec *deadline);
static void board_music_amplifier(bool enable) {(void)enable;}
'''
codec_stubs=r'''
#define FAR
#define OK 0
#define ES8311_DAC_REG32 0x32
#define ES8311_ADC_REG17 0x17
#define auderr(...) ((void)0)
#define audinfo(...) ((void)0)
typedef enum {ES_MODULE_ADC,ES_MODULE_DAC,ES_MODULE_ADC_DAC} es_module_e;
struct es8311_dev_s {unsigned volume_out,volume_in;};
static struct es8311_dev_s codec_state;
static unsigned dac_register;
static int es8311_writereg(struct es8311_dev_s *p,unsigned reg,unsigned value)
{(void)p;if(reg==ES8311_DAC_REG32)dac_register=value;return 0;}
'''
parts.insert(0,codec_stubs+'\n'+function(codec,'es8311_setvolume'))
video_stubs=r'''
struct local_video {unsigned volume;};
struct play_session {struct music_pcm pcm;struct local_video *media;};
static struct play_session *session;
static unsigned codec_volume;
static bool audio_cancelled(void *p){(void)p;return false;}
static int music_pcm_foreground_open(struct music_pcm *pcm,unsigned rate,unsigned volume,
 unsigned timeout,bool (*cancel)(void *),void *opaque)
{(void)rate;(void)timeout;(void)cancel;(void)opaque;return music_pcm_volume(pcm,volume);}
static void local_video_volume(struct local_video *media,unsigned value){media->volume=value;}
'''
parts += [video_stubs,*(function(video,n) for n in ['audio_open','audio_volume','local_video_set_volume'])]
suffix=r'''
#include <stdarg.h>
static int ioctl(int fd,unsigned request,...) {
 (void)fd;
 if(request==AUDIOIOC_START){starts++;return 0;}
 va_list args;va_start(args,request);
 void *argument=(void *)va_arg(args,unsigned long);va_end(args);
 if(request==AUDIOIOC_CONFIGURE){struct audio_caps_desc_s *caps=argument;codec_volume=caps->caps.ac_controls.hw[0]/10;assert(codec_volume<=75);assert(!es8311_setvolume(&codec_state,ES_MODULE_DAC,caps->caps.ac_controls.hw[0]));assert(dac_register<=0xbf);return 0;}
 assert(request==AUDIOIOC_ENQUEUEBUFFER);
 struct audio_buf_desc_s *d=argument;
 struct ap_buffer_s *b=d->u.buffer;
 assert(b->nbytes && b->nbytes<=PCM_TEST_CAPACITY*4 && b->nbytes%64==0 && b->curbyte==0);
 assert(used+b->nbytes/2<=65536);
 memcpy(captured+used,b->samp,b->nbytes);used+=b->nbytes/2;
 fifo[tail++]=b;return 0;
}
static ssize_t mq_timedreceive(int mq,char *out,size_t size,void *priority,const struct timespec *deadline) {
 (void)mq;(void)priority;(void)deadline;assert(size==sizeof(struct audio_msg_s));assert(head<tail);
 struct audio_msg_s msg={.msg_id=message_id,.u.ptr=fifo[head++]};
 memcpy(out,&msg,sizeof(msg));return sizeof(msg);
}
int main(void) {
 unsigned totals[]={1,15,16,17,591,542,1151,1152,1153,20000};
 unsigned splits[]={1,15,16,17,542,591,1151,1152,4096};
 unsigned cases=0;
 unsigned levels[]={0,1,25,50,100};
 unsigned rates[]={16000,44100,48000};
 for(unsigned rate_index=0;rate_index<3;rate_index++){
 for(unsigned level=0;level<5;level++){
 assert(!glass_speaker_volume_set(levels[level])&&glass_speaker_volume()==levels[level]);
 for(unsigned t=0;t<sizeof(totals)/sizeof(totals[0]);t++)
 for(unsigned s=0;s<sizeof(splits)/sizeof(splits[0]);s++) {
  struct music_pcm pcm={.fd=1,.rate=rates[rate_index]};struct ap_buffer_s buffers[4];
  uint32_t storage[4][PCM_TEST_CAPACITY+2];int16_t input[20000];
  for(unsigned i=0;i<4;i++) {
   storage[i][0]=0xdeadbeef;storage[i][PCM_TEST_CAPACITY+1]=0xcafebabe;
   buffers[i]=(struct ap_buffer_s){.samp=(unsigned char *)&storage[i][1]};pcm.buffers[i]=&buffers[i];
  }
  for(unsigned i=0;i<totals[t];i++)input[i]=(int16_t)(i*97+1);
  head=tail=used=starts=0;
  for(unsigned offset=0;offset<totals[t];) {
   unsigned n=totals[t]-offset;if(n>splits[s])n=splits[s];
   assert(!music_pcm_write(&pcm,input+offset,n,1));offset+=n;
  }
  assert(!music_pcm_drain(&pcm));assert(pcm.played_samples==totals[t]);
  assert(!pcm.pending && !pcm.staged_frames && starts==1);
  assert(used==((totals[t]+15)&~15u)*2);
  for(unsigned i=0;i<totals[t];i++)assert(captured[2*i]==(int16_t)((int32_t)input[i]*(int32_t)levels[level]/100) && captured[2*i+1]==captured[2*i]);
  for(unsigned i=totals[t]*2;i<used;i++)assert(captured[i]==0);
  for(unsigned i=0;i<4;i++)assert(storage[i][0]==0xdeadbeef && storage[i][PCM_TEST_CAPACITY+1]==0xcafebabe);
  cases++;
 }
 }
 }
 /* Reproduce the diagnostic's player mute followed by Settings unmute on
  * the same stream. Old code kept codec_volume=0 despite the master change. */
 {
  struct local_video media={0};struct play_session playing={.media=&media};session=&playing;
  struct ap_buffer_s buffers[4];uint32_t storage[4][PCM_TEST_CAPACITY];
  struct music_pcm *pcm=&playing.pcm;pcm->fd=1;
  for(unsigned i=0;i<4;i++){buffers[i]=(struct ap_buffer_s){.samp=(unsigned char *)storage[i]};pcm->buffers[i]=&buffers[i];}
  local_video_set_volume(0);assert(!glass_speaker_volume()&&!media.volume);
  assert(!audio_open(&playing,16000,0)&&codec_volume==75);
  unsigned restore[]={0,25,75,0,40};int16_t sample=10000;head=tail=used=starts=0;
  for(unsigned i=0;i<sizeof(restore)/sizeof(restore[0]);i++){
   assert(!glass_speaker_volume_set(restore[i]));
   assert(!audio_volume(&playing,0)); /* stale worker callback cannot remute */
   unsigned before=used;assert(!music_pcm_write(pcm,&sample,1,1));assert(!music_pcm_drain(pcm));
   assert(codec_volume==75&&glass_speaker_volume()==restore[i]);
   assert(captured[before]==(int16_t)(sample*restore[i]/100)&&captured[before+1]==captured[before]);
  }
  local_video_set_volume(55);assert(glass_speaker_volume()==55&&media.volume==55);
  local_video_set_volume(101);assert(glass_speaker_volume()==55&&media.volume==55);
  assert(audio_volume(&playing,101)==-EINVAL);session=NULL;
  unsigned stream[]={0,1,40,75,100,101};
  for(unsigned i=0;i<sizeof(stream)/sizeof(stream[0]);i++){
   assert(!music_pcm_volume(pcm,stream[i]));assert(codec_volume==(stream[i]>75?75:stream[i]));
  }
  assert(!es8311_setvolume(&codec_state,ES_MODULE_DAC,1000)&&dac_register==200);
  assert(!music_pcm_volume(pcm,100)&&dac_register==0xbf);
  puts("CODEC_UNITY_GAIN_OK: production ES8311 mapping; old 1000=+4.5dB, capped 750=0dB; zero/range preserved");
  puts("VIDEO_MASTER_VOLUME_OK: player mute -> Settings restore on same PCM, stale callbacks, single gain, bidirectional setters");
 }
 glass_speaker_volume_set(100);
 assert(glass_speaker_volume_set(101)==-EINVAL && glass_speaker_volume()==100);
 struct ap_buffer_s rejected={.nbytes=0};
 struct music_pcm bad={.buffers={&rejected},.queued={true},.pending=1,.submitted_samples={576}};
 head=tail=0;fifo[tail++]=&rejected;message_id=AUDIO_MSG_IOERR;
 assert(pcm_message(&bad)==-EIO && !bad.played_samples);
 printf("PCM_FRAMING_PASS cases=%u; master volume 0/1/25/50/100, signed samples, invalid input, sample order, stereo slots, 64-byte DMA alignment, tail silence, guards, queue drain, rejected DMA not counted\n",cases);
}
'''
header=(source.parent/'glass_music_pcm.h').read_text()
prefix=prefix.replace('#define PCM_TEST_CAPACITY 1152','#define PCM_TEST_CAPACITY MUSIC_PCM_MEDIA_FRAMES')
defines='\n'.join(re.findall(r'^#define MUSIC_PCM_(?:FRAMES|MEDIA_FRAMES|BLOCK_FRAMES)[^\n]*',header,re.M))
prefix=defines+'\n'+prefix
with tempfile.TemporaryDirectory(prefix='pcm-framing-') as directory:
    path=Path(directory)/'check.c';path.write_text(prefix+'\n'.join(parts)+suffix)
    subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-g',str(path),'-lm','-o',str(path.with_suffix(''))],check=True)
    result=subprocess.check_output([str(path.with_suffix(''))],text=True)
    print('production_source_sha256='+hashlib.sha256(source.read_bytes()).hexdigest())
    print(result,end='')
