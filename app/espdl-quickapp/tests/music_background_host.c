/* SPDX-License-Identifier: Apache-2.0 */
/* Real service workers, HTTP/codec boundary mocked; never contacts a device. */
#include <assert.h>
#include <stdatomic.h>
#include "music_service_under_test.c"
#include "speaker_under_test.c"
#include "glass_agent.h"
#include "glass_board_music.h"

static const struct glass_board_home *music_provider;
static struct music_now_playing wait_for(const char *name,enum music_state phase,enum music_lyrics_state lyric);
void glass_agent_set_board_music(const struct glass_board_home *provider){music_provider=provider;}
static cJSON *mcp_music(const char *tool,const char *json,bool failed)
{
  cJSON *args=cJSON_Parse(json);assert(args);atomic_bool cancel=false;
  char *text=music_provider->call(tool,args,&cancel);cJSON_Delete(args);assert(text);
  assert(!strstr(text,"https://")&&!strstr(text,"offline-fixture"));
  cJSON *out=cJSON_Parse(text);free(text);assert(out);
  assert((cJSON_GetObjectItem(out,"error")!=NULL)==failed);return out;
}
static void mcp_checks(void)
{
  glass_board_music_register();assert(music_provider);
  char *schema=malloc(strlen(music_provider->catalog)+3);assert(schema);
  sprintf(schema,"[%s]",music_provider->catalog);cJSON *catalog=cJSON_Parse(schema);
  assert(cJSON_GetArraySize(catalog)==8);cJSON_Delete(catalog);free(schema);
  unsigned gen=play_generation;
  cJSON *out=mcp_music("music_search","{\"query\":\"B\"}",false);
  assert(!strcmp(cJSON_GetObjectItem(out,"name")->valuestring,"B")&&play_generation==gen);
  assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(out,"card"),"kind")->valuestring,"music"));cJSON_Delete(out);
  cJSON_Delete(mcp_music("music_play","{\"query\":\"https://fixture/evil\"}",true));
  cJSON_Delete(mcp_music("music_play","{\"query\":\"B\",\"url\":\"https://fixture/evil\"}",true));
  assert(play_generation==gen);
  out=mcp_music("music_play","{\"query\":\"B\"}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(out,"accepted"))&&!cJSON_IsTrue(cJSON_GetObjectItem(out,"verified")));cJSON_Delete(out);
  cJSON_Delete(mcp_music("music_pause","{}",false));
  cJSON_Delete(mcp_music("music_resume","{}",false));
  wait_for("B",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  out=mcp_music("music_status","{}",false);
  assert(!strcmp(cJSON_GetObjectItem(out,"state")->valuestring,"playing"));cJSON_Delete(out);
  cJSON_Delete(mcp_music("music_pause","{}",false));
  cJSON_Delete(mcp_music("music_pause","{}",false));
  struct music_now_playing view;glass_music_now_playing(&view);assert(view.pause_requested&&view.state==MUSIC_PAUSED);
  cJSON_Delete(mcp_music("music_resume","{}",false));
  cJSON_Delete(mcp_music("music_resume","{}",false));
  glass_music_now_playing(&view);assert(!view.pause_requested);
  out=mcp_music("music_volume","{\"level\":35}",false);assert(cJSON_GetObjectItem(out,"volume")->valueint==35);cJSON_Delete(out);
  cJSON_Delete(mcp_music("music_volume","{\"level\":101}",true));
  cJSON_Delete(mcp_music("music_volume","{\"level\":0.5}",true));
  cJSON_Delete(mcp_music("music_skip","{\"direction\":\"next\"}",false));
  cJSON_Delete(mcp_music("music_skip","{\"direction\":\"next\",\"direction\":\"previous\"}",true));
  cJSON_Delete(mcp_music("music_stop","{}",false));assert(!glass_music_stop_wait(500));
  cJSON_Delete(mcp_music("music_resume","{}",true));
  atomic_bool cancel=true;cJSON *args=cJSON_CreateObject();
  char *text=music_provider->call("music_stop",args,&cancel);assert(text&&strstr(text,"cancelled"));free(text);cJSON_Delete(args);
  puts("MUSIC_MCP_OK: real service search/play/state, idempotent pause/resume, volume/skip/stop, URL redaction, argument rejection, cancellation; no UI linked");
}

static const char *mp3_path;
static atomic_bool hold_lyrics,lyrics_entered;
static atomic_uint opens,closes;
static atomic_uint metadata_fetches;
static atomic_uint search_requests;
static atomic_uint audio_requests, pcm_pauses;
static atomic_bool hold_audio, audio_held;
static atomic_bool capture_pcm, captured_pcm;
static int16_t captured_samples[128];
static atomic_bool has_music_key=true;

int portal_music_key(char *out,size_t cap)
{if(!atomic_load(&has_music_key)){out[0]=0;return -ENOENT;}snprintf(out,cap,"offline-fixture");return 0;}
int qpk_storage_read(const char *root,const char *package,const char *key,char *out,size_t cap,size_t *size)
{(void)root;(void)package;(void)key;(void)out;(void)cap;*size=0;return -ENOENT;}
int qpk_storage_write(const char *root,const char *package,const char *key,const char *data,size_t size)
{(void)root;(void)package;(void)key;(void)data;(void)size;return 0;}

int music_http_get(const char *url,music_data_cb sink,void *ctx,music_cancel_cb cancel,unsigned *status)
{
  if(status)*status=200;
  if(strstr(url,"jkapi.com")) {
    atomic_fetch_add(&search_requests,1);
    const char *name=strstr(url,"&name=");assert(name);name+=6;
    if(!strcmp(name,"Missing"))return -EIO;
    char json[1024];snprintf(json,sizeof(json),"{\"code\":1,\"name\":\"%s\",\"artist\":\"Demo\",\"album\":\"Test\",\"music_url\":\"https://fixture/audio\"}",name);
    return sink(ctx,(const unsigned char *)json,strlen(json));
  }
  if(strstr(url,"api.lrc.cx")) {
    atomic_fetch_add(&metadata_fetches,1);
    const char *name=strstr(url,"title=");assert(name);name+=6;
    char title[128];size_t n=strcspn(name,"&");assert(n<sizeof(title));memcpy(title,name,n);title[n]=0;
    if(!strcmp(title,"NoLyrics"))return -ENOENT;
    if(!strcmp(title,"A") && atomic_load(&hold_lyrics)) {
      atomic_store(&lyrics_entered,true);
      /* Deliberately complete a stale response despite cancellation. */
      while(atomic_load(&hold_lyrics))usleep(1000);
    }
    char json[2048];snprintf(json,sizeof(json),"[{\"title\":\"%s\",\"artist\":\"Wrong Artist\",\"lyrics\":\"[00:00]WRONG\"},{\"title\":\"%s\",\"artist\":\"Demo\",\"lyrics\":\"[00:00]%s intro\\n[00:00.50]%s current\\n[00:01.00]%s next\"}]",title,title,title,title,title);
    cJSON *metadata=cJSON_Parse(json);assert(metadata);
    cJSON_AddStringToObject(cJSON_GetArrayItem(metadata,0),"cover","https://fixture/wrong-artist.jpg");
    cJSON_AddStringToObject(cJSON_GetArrayItem(metadata,1),"cover_format","https://fixture/{w}x{h}.jpg");
    if(!strcmp(title,"CoverOnly"))cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetArrayItem(metadata,1),"lyrics");
    char *body=cJSON_PrintUnformatted(metadata);assert(body);
    int ret=sink(ctx,(const unsigned char *)body,strlen(body));cJSON_free(body);cJSON_Delete(metadata);return ret;
  }
  assert(!strcmp(url,"https://fixture/audio"));
  atomic_fetch_add(&audio_requests, 1);
  FILE *f=fopen(mp3_path,"rb");assert(f);unsigned char bytes[4096];int ret=0;size_t n;
  while((n=fread(bytes,1,sizeof(bytes),f))) {
    if(cancel(ctx)){ret=-ECANCELED;break;}
    ret=sink(ctx,bytes,n);if(ret)break;
    if (ftell(f) >= 65536 && atomic_load(&hold_audio)) {
      atomic_store(&audio_held, true);
      while (atomic_load(&hold_audio) && !cancel(ctx)) usleep(1000);
    }
  }
  fclose(f);return ret;
}

int music_pcm_open(struct music_pcm *pcm,unsigned rate,unsigned volume)
{assert(volume==100);pcm->fd=3;pcm->rate=rate;atomic_fetch_add(&opens,1);return 0;}
int music_pcm_write(struct music_pcm *pcm,const int16_t *samples,unsigned frames,unsigned channels)
{
 assert(samples && (channels==1||channels==2));
 if(atomic_exchange(&capture_pcm,false)){
   assert(frames*channels>=128);memcpy(captured_samples,samples,sizeof(captured_samples));
   atomic_store(&captured_pcm,true);
 }
 usleep(5000);pcm->played_samples+=frames;return 0;
}
int music_pcm_pause(struct music_pcm *pcm,bool pause){assert(pcm->fd>=0);if(pause)atomic_fetch_add(&pcm_pauses,1);return 0;}
int music_pcm_volume(struct music_pcm *pcm,unsigned volume){(void)pcm;assert(volume==100);return 0;}
int music_pcm_drain(struct music_pcm *pcm){(void)pcm;return 0;}
void music_pcm_close(struct music_pcm *pcm){if(pcm->fd>=0)atomic_fetch_add(&closes,1);pcm->fd=-1;}

static struct music_now_playing wait_for(const char *name,enum music_state phase,enum music_lyrics_state lyric)
{
  struct music_now_playing s;
  for(unsigned i=0;i<3000;i++) {
    glass_music_now_playing(&s);
    if(!strcmp(s.name,name) && s.state==phase && s.lyrics_state==lyric)return s;
    usleep(1000);
  }
  fprintf(stderr,"timeout %s: state=%d lyric=%d\n",name,s.state,s.lyrics_state);abort();
}
static void play(const char *name)
{struct music_song song={0};snprintf(song.name,sizeof(song.name),"%s",name);assert(!glass_music_play(&song));}

static void parser_checks(void)
{
  struct music_lrc *lrc=calloc(1,sizeof(*lrc));assert(lrc);
  const char *sample="\xef\xbb\xbf[offset:-250]\r\n[00:02.000]最后一句\r\n[00:00.50][00:01.5]重复一句\n[00:01.75]\n";
  assert(!music_lrc_parse(sample,strlen(sample),lrc));assert(lrc->count==4);
  assert(lrc->lines[0].ms==250 && lrc->lines[1].ms==1250 && lrc->lines[2].ms==1500 && lrc->lines[3].ms==1750);
  assert(music_lrc_index(lrc,249)==-1 && music_lrc_index(lrc,250)==0 && music_lrc_index(lrc,1750)==3);
  assert(!strcmp(lrc->lines[3].text,"最后一句"));
  sample="[00:00.001]one\n[00:01.01]two\n[offset:+100]\n";
  assert(!music_lrc_parse(sample,strlen(sample),lrc));assert(lrc->lines[0].ms==101 && lrc->lines[1].ms==1110);
  assert(music_lrc_parse("<html>error</html>",18,lrc)==-ENODATA);
  assert(music_lrc_parse("[00:60]bad",10,lrc)==-ENODATA);
  assert(music_lrc_parse("[00:00]\xc0\xaf",9,lrc)==-EINVAL);
  assert(music_lrc_parse("[00:00]\0bad",11,lrc)==-EINVAL);
  char *large=calloc(1,30000);assert(large);
  strcpy(large,"[00:00]");for(unsigned i=0;i<300;i++)strcat(large,"中");
  assert(!music_lrc_parse(large,strlen(large),lrc));assert(music_lrc_utf8(lrc->lines[0].text,strlen(lrc->lines[0].text)));
  large[0]=0;for(unsigned i=0;i<193;i++){
    char line[40];snprintf(line,sizeof(line),"[%02u:%02u]line\n",i/60,i%60);strcat(large,line);
  }assert(music_lrc_parse(large,strlen(large),lrc)==-E2BIG);
  assert(music_lyrics_identity(" Demo ","demo"));assert(!music_lyrics_identity("", " "));assert(!music_lyrics_identity("Singer","Other"));
  struct music_song song={0};strcpy(song.name,"中文 &?");strcpy(song.artist,"A/B");char url[2048];assert(!music_lyrics_url(&song,url,sizeof(url)));
  assert(music_search_url("B","",url,sizeof(url))==MUSIC_ERROR_KEY_REQUIRED);
  assert(music_search_url("B",NULL,url,sizeof(url))==MUSIC_ERROR_KEY_REQUIRED);
  assert(music_search_url("","configured",url,sizeof(url))==-EINVAL);
  assert(strstr(url,"%E4%B8%AD%E6%96%87%20%26%3F") && strstr(url,"A%2FB"));assert(music_lyrics_url(&song,url,8)==-E2BIG);
  unsigned seed=1;for(unsigned i=0;i<1000;i++){size_t n=i%255+1;for(size_t j=0;j<n;j++){seed=seed*1664525+1013904223;large[j]=seed>>24;}music_lrc_parse(large,n,lrc);}
  free(large);free(lrc);puts("LRC_PARSER_OK: timing, offsets, multiple tags, UTF-8, invalid/oversized input, identity, URL encoding and fuzz");
}

static void bilingual_checks(void)
{
  struct music_lrc *lrc=calloc(1,sizeof(*lrc));assert(lrc);
  const char *sample="[00:00]Good morning\n[00:00]早晨與風\n[00:01]Hello\n[00:01]你好\n";
  assert(!music_lrc_parse(sample,strlen(sample),lrc)&&lrc->count==2);
  assert(!strcmp(lrc->lines[0].text,"Good morning")&&!strcmp(lrc->lines[0].translation,"早晨與風"));
  music_lrc_simplify(lrc);assert(!strcmp(lrc->lines[0].translation,"早晨与风"));
  sample="[00:00]愛與風\n[00:01]乾隆與乾坤，乾杯\n";
  assert(!music_lrc_parse(sample,strlen(sample),lrc));music_lrc_simplify(lrc);
  assert(!strcmp(lrc->lines[0].text,"爱与风"));
  assert(!strcmp(lrc->lines[1].text,"乾隆与乾坤，干杯"));
  sample="[00:00]愛の歌\n[00:01]時\n";
  assert(!music_lrc_parse(sample,strlen(sample),lrc));music_lrc_simplify(lrc);
  assert(lrc->foreign&&!strcmp(lrc->lines[0].text,"愛の歌")&&!strcmp(lrc->lines[1].text,"時"));
  sample="[00:00]한글 愛\n";
  assert(!music_lrc_parse(sample,strlen(sample),lrc));music_lrc_simplify(lrc);
  assert(lrc->foreign&&!strcmp(lrc->lines[0].text,"한글 愛"));
  sample="[00:00]Morning\n[00:01]Light\n";
  assert(!music_lrc_parse(sample,strlen(sample),lrc));
  sample="[00:00]晨光與你\n[00:01.100]不能錯配\n";
  assert(!music_lrc_translation(lrc,sample,strlen(sample)));music_lrc_simplify(lrc);
  assert(!strcmp(lrc->lines[0].translation,"晨光与你")&&!lrc->lines[1].translation[0]);
  char *many=calloc(1,24000);assert(many);
  for(unsigned i=0;i<192;i++){
    char pair[100];snprintf(pair,sizeof(pair),"[%02u:%02u]original %u\n[%02u:%02u]译文 %u\n",i/60,i%60,i,i/60,i%60,i);strcat(many,pair);
  }
  assert(!music_lrc_parse(many,strlen(many),lrc)&&lrc->count==192);
  assert(!strcmp(lrc->lines[191].translation,"译文 191"));
  assert(!mkdir(MUSIC_LYRICS_ROOT,0700)||errno==EEXIST);
  FILE *side=fopen(MUSIC_LYRICS_ROOT "/Sidecar - Demo.zh.lrc","wb");assert(side);
  fputs("[00:00]帶你聽音樂\n",side);fclose(side);
  sample="[00:00]音楽を聴こう\n";assert(!music_lrc_parse(sample,strlen(sample),lrc));
  struct music_song song={0};strcpy(song.name,"Sidecar");strcpy(song.artist,"Demo");
  finish_lyrics(&song,lrc);
  assert(!strcmp(lrc->lines[0].text,"音楽を聴こう")&&!strcmp(lrc->lines[0].translation,"带你听音乐"));
  free(many);free(lrc);puts("MUSIC_BILINGUAL_OK: 192 paired timestamps, exact translation alignment, OpenCC phrases, Chinese simplification and Japanese/Korean preservation");
}

static void cache_checks(void)
{
  const unsigned char sample[]={0,1,2,255};size_t size=0;
  music_cache_write('l',"first",sample,sizeof(sample),100);
  unsigned char *body=music_cache_read('l',"first",100,512,&size);
  assert(body&&size==sizeof(sample)&&!memcmp(body,sample,size));free(body);
  assert(!music_cache_read('l',"first",2,0,&size));
  char other[32];unsigned slot=music_cache_hash(2166136261u,"first",5)%32;
  for(unsigned n=0;;n++){snprintf(other,sizeof(other),"other-%u",n);if(music_cache_hash(2166136261u,other,strlen(other))%32==slot)break;}
  assert(!music_cache_read('l',other,100,0,&size));
  music_cache_write('l',other,sample,sizeof(sample),100);assert(!music_cache_read('l',"first",100,0,&size));
  char path[256];assert(!music_cache_path('l',other,path,sizeof(path)));
  FILE *f=fopen(path,"r+b");assert(f);assert(!fseek(f,-1,SEEK_END));fputc(0,f);fclose(f);
  assert(!music_cache_read('l',other,100,0,&size));
  f=fopen(path,"wb");assert(f);fputs("partial",f);fclose(f);assert(!music_cache_read('l',other,100,0,&size));
  assert(music_cache_path('x',"bad",path,sizeof(path))<0);
  puts("MUSIC_METADATA_CACHE_OK: binary hit, bounds, full-identity collision check, replacement, checksum and truncated files");
}

static void stream_checks(void)
{
  struct music_song song={0};strcpy(song.name,"Match");strcpy(song.artist,"Demo");
  struct music_lrc *lrc=calloc(1,sizeof(*lrc));assert(lrc);
  char cover[1024]={0};struct lyric_transfer t={.song=&song,.out=lrc,.cover=cover};t.transfer.body=malloc(LYRIC_OBJECT_MAX+1);assert(t.transfer.body);
  const char *json="[{\"title\":\"Match\",\"artist\":\"Other\",\"lrc\":\"[00:00]wrong\"},{\"extra\":\"quoted \\\" brace } and [\",\"title\":\"Match\",\"artist\":\"Demo\",\"lrc\":\"[00:00.1]right\"}]";
  int ret=0;for(size_t i=0;json[i]&&!ret;i++)ret=collect_lyrics(&t,(const unsigned char *)json+i,1);
  assert(ret==-EALREADY && t.found && lrc->lines[0].ms==100 && !strcmp(lrc->lines[0].text,"right"));
  t.found=false;t.array=false;t.depth=0;t.total=0;t.candidates=0;t.transfer.used=0;
  json="[{\"title\":\"Match\",\"artist\":\"Demo\",\"lrc\":\"[00:00.1]Hello\",\"tlyric\":{\"lyric\":\"[00:00.1]與你同在\"}}]";
  assert(collect_lyrics(&t,(const unsigned char *)json,strlen(json))==-EALREADY);
  music_lrc_simplify(lrc);assert(!strcmp(lrc->lines[0].translation,"与你同在"));
  cJSON *item=cJSON_Parse("{\"cover_format\":\"https://fixture/{w}x{h}.jpg\"}");assert(item);
  music_metadata_cover(item,cover);assert(!strcmp(cover,"https://fixture/320x320.jpg"));cJSON_Delete(item);
  item=cJSON_Parse("{\"cover\":\"http://fixture/unsafe.jpg\"}");assert(item);music_metadata_cover(item,cover);assert(!cover[0]);cJSON_Delete(item);
  free(t.transfer.body);free(lrc);puts("LYRICS_STREAM_OK: one-byte chunk boundaries, quoted braces/escapes, lrc/lyrics schema and artist identity");
}

static int live_response(const char *title,const char *artist)
{
  struct music_song song={0};snprintf(song.name,sizeof(song.name),"%s",title);snprintf(song.artist,sizeof(song.artist),"%s",artist);
  struct music_lrc *lrc=calloc(1,sizeof(*lrc));assert(lrc);
  char cover[1024]={0};struct lyric_transfer t={.song=&song,.out=lrc,.cover=cover};t.transfer.body=malloc(LYRIC_OBJECT_MAX+1);assert(t.transfer.body);
  unsigned char bytes[137];size_t n;int ret=0;
  while(!ret && (n=fread(bytes,1,sizeof(bytes),stdin)))ret=collect_lyrics(&t,bytes,n);
  assert(ret==-EALREADY && t.found && lrc->count>0);
  assert(!strncmp(cover,"https://",8));
  printf("LIVE_LYRIC_PARSE_OK: exact song/artist, %u candidates, %u timed lines, %zu response bytes consumed; lyric text not retained\n",t.candidates,lrc->count,t.total);
  free(t.transfer.body);free(lrc);return 0;
}

static void missing_key_checks(void)
{
  glass_board_music_register();atomic_store(&has_music_key,false);
  unsigned requests=atomic_load(&search_requests);
  cJSON *out=mcp_music("music_search","{\"query\":\"B\"}",true);
  assert(!strcmp(cJSON_GetObjectItem(out,"error")->valuestring,"music_search_key_required"));
  assert(cJSON_IsTrue(cJSON_GetObjectItem(out,"configuration_required")));
  assert(cJSON_IsString(cJSON_GetObjectItem(out,"message")));cJSON_Delete(out);
  struct music_snapshot *s=malloc(sizeof(*s));assert(s);glass_music_get(s);
  assert(!s->searching&&!s->has_result&&s->search_error==MUSIC_ERROR_KEY_REQUIRED);free(s);
  assert(atomic_load(&search_requests)==requests);
  atomic_store(&has_music_key,true);
  cJSON_Delete(mcp_music("music_search","{\"query\":\"B\"}",false));
  assert(atomic_load(&search_requests)==requests+1);
  puts("MUSIC_SEARCH_KEY_OK: distinct missing-key result, no HTTP request, MCP configuration guidance, recovery after save");
}

static unsigned search_result(const char *query)
{
  char args[256];snprintf(args,sizeof(args),"{\"query\":\"%s\"}",query);
  cJSON *out=mcp_music("music_search",args,false);
  unsigned id=(unsigned)cJSON_GetObjectItem(out,"result_id")->valuedouble;
  assert(id);cJSON_Delete(out);return id;
}

static cJSON *play_result(const char *query,unsigned id,bool failed)
{
  char args[256];snprintf(args,sizeof(args),"{\"query\":\"%s\",\"result_id\":%u}",query,id);
  return mcp_music("music_play",args,failed);
}

static void selected_result_checks(void)
{
  unsigned requests=atomic_load(&search_requests),gen=play_generation;
  unsigned id=search_result("B");
  assert(atomic_load(&search_requests)==requests+1&&play_generation==gen);
  cJSON_Delete(play_result("C",id,true)); /* wrong query cannot consume B */
  const char *invalid[]={"0","-1","0.5","4294967296","null","\"1\""};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
    char args[256];snprintf(args,sizeof(args),"{\"query\":\"B\",\"result_id\":%s}",invalid[i]);
    cJSON_Delete(mcp_music("music_play",args,true));
  }
  cJSON_Delete(mcp_music("music_play","{\"query\":\"B\",\"result_id\":1,\"result_id\":2}",true));
  cJSON_Delete(mcp_music("music_search","{\"query\":\"B\",\"result_id\":1}",true));
  cJSON_Delete(mcp_music("music_play","{\"query\":\"B\",\"query\":\"C\"}",true));
  assert(play_generation==gen);
  cJSON *out=play_result("B",id,false);
  unsigned track=(unsigned)cJSON_GetObjectItem(out,"requested_track_id")->valuedouble;
  assert(cJSON_IsTrue(cJSON_GetObjectItem(out,"accepted")));cJSON_Delete(out);
  struct music_now_playing view=wait_for("B",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  assert(view.track_id==track&&!strcmp(view.artist,"Demo"));
  assert(atomic_load(&search_requests)==requests+1); /* no lookup on play */
  gen=play_generation;out=play_result("B",id,true);
  assert(!strcmp(cJSON_GetObjectItem(out,"error")->valuestring,"music_result_used_do_not_replay"));
  cJSON_Delete(out);assert(play_generation==gen);
  out=mcp_music("music_status","{}",false);
  assert((unsigned)cJSON_GetObjectItem(out,"track_id")->valuedouble==track);cJSON_Delete(out);
  assert(!glass_music_stop_wait(3000));

  id=search_result("B");search_result("C");gen=play_generation;
  cJSON_Delete(play_result("B",id,true));assert(play_generation==gen);
  id=search_result("B");
  pthread_mutex_lock(&lock);result_deadline=music_now_ms()-1;pthread_mutex_unlock(&lock);
  cJSON_Delete(play_result("B",id,true));assert(play_generation==gen);
  id=search_result("B");
  cJSON_Delete(mcp_music("music_search","{\"query\":\"Missing\"}",true));
  cJSON_Delete(play_result("B",id,true));assert(play_generation==gen);
  /* A new explicit search recovers; the old URL is never silently replayed. */
  id=search_result("B");cJSON_Delete(play_result("B",id,false));
  wait_for("B",MUSIC_PLAYING,MUSIC_LYRICS_READY);assert(!glass_music_stop_wait(3000));
  /* Query-only/native requests must reset the resolved-result flag. */
  requests=atomic_load(&search_requests);play("C");wait_for("C",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  assert(atomic_load(&search_requests)==requests+1);assert(!glass_music_stop_wait(3000));
  puts("MUSIC_SELECTED_RESULT_OK: one lookup, exact song/artist and track, single use, replaced/expired/failed result rejection, strict arguments and query-only recovery");
}

static void audio_cache_checks(void)
{
  const char *key="https://fixture/cache-test";
  const unsigned char bytes[]={0,255,1,7,99};
  struct music_audio_cache cache;
  music_audio_cache_begin(&cache,key);assert(cache.file);
  music_audio_cache_append(&cache,bytes,sizeof(bytes));
  music_audio_cache_finish(&cache,false);assert(!music_audio_cache_open(key));
  music_audio_cache_begin(&cache,key);assert(cache.file);
  music_audio_cache_append(&cache,bytes,2);
  music_audio_cache_append(&cache,bytes+2,sizeof(bytes)-2);
  music_audio_cache_finish(&cache,true);
  FILE *f=music_audio_cache_open(key);assert(f);
  unsigned char out[16];assert(fread(out,1,sizeof(out),f)==sizeof(bytes)&&!memcmp(out,bytes,sizeof(bytes)));fclose(f);
  char other[80];unsigned slot=music_cache_hash(2166136261u,key,strlen(key))%8;
  for(unsigned i=0;;i++){snprintf(other,sizeof(other),"https://fixture/other-%u",i);if(music_cache_hash(2166136261u,other,strlen(other))%8==slot)break;}
  assert(!music_audio_cache_open(other));
  f=fopen(cache.path,"r+b");assert(f);assert(!fseek(f,-1,SEEK_END));fputc(0,f);fclose(f);
  assert(!music_audio_cache_open(key));
  music_audio_cache_begin(&cache,key);assert(cache.file);
  music_audio_cache_append(&cache,bytes,MUSIC_AUDIO_CACHE_MAX+1u);
  assert(!cache.file && access(cache.temporary,F_OK));
  f=fopen(cache.path,"wb");assert(f);fputs("partial",f);fclose(f);
  assert(!music_audio_cache_open(key));
  puts("MUSIC_AUDIO_CACHE_OK: binary round-trip, exact identity/collision, checksum, truncated/cancelled/oversize rejection");
}

static void seek_buffer_checks(void)
{
  /* Decode a whole track into an independent oracle, then compare the actual
   * first audible samples after seeking against its absolute sample offsets. */
  FILE *reference=fopen(mp3_path,"rb");assert(reference);
  fseek(reference,0,SEEK_END);size_t size=ftell(reference);rewind(reference);
  unsigned char *mp3=malloc(size);assert(mp3&&fread(mp3,1,size,reference)==size);fclose(reference);
  int16_t *oracle=malloc(1000000*sizeof(*oracle));assert(oracle);
  mp3dec_t decoder;mp3dec_init(&decoder);size_t offset=0,total=0;
  while(offset<size){
    mp3dec_frame_info_t info={0};int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int frames=mp3dec_decode_frame(&decoder,mp3+offset,size-offset,frame,&info);
    assert(info.frame_bytes>0);offset+=info.frame_bytes;
    if(frames){assert(info.hz==22050&&info.channels==1&&total+frames<1000000);memcpy(oracle+total,frame,frames*sizeof(*frame));total+=frames;}
  }
  free(mp3);assert(total>22050*15);
  struct music_song local={0};strcpy(local.name,"Local");strcpy(local.artist,"Demo");
  snprintf(local.url,sizeof(local.url),"%s",mp3_path);
  assert(!glass_music_play(&local));
  struct music_now_playing s=wait_for("Local",MUSIC_PLAYING,MUSIC_LYRICS_MISSING);
  assert(s.duration_ms>10000);
  unsigned track=s.track_id;
  assert(glass_music_seek(track+1,3000)==-ESTALE);
  assert(!glass_music_set_paused(true));usleep(30000);
  assert(!glass_music_seek(track,7000));
  assert(!glass_music_seek(track,2000));
  usleep(80000);glass_music_now_playing(&s);
  assert(s.state==MUSIC_PAUSED && s.position_ms==2000 && s.track_id==track);
  atomic_store(&captured_pcm,false);atomic_store(&capture_pcm,true);
  assert(!glass_music_set_paused(false));
  for(unsigned i=0;i<3000&&!atomic_load(&captured_pcm);i++)usleep(1000);
  s=wait_for("Local",MUSIC_PLAYING,MUSIC_LYRICS_MISSING);
  assert(s.position_ms>=2000 && s.position_ms<3000);
  assert(atomic_load(&captured_pcm)&&!memcmp(captured_samples,oracle+44100,sizeof(captured_samples)));
  assert(!glass_music_set_paused(true));usleep(30000);
  atomic_store(&captured_pcm,false);atomic_store(&capture_pcm,true);
  assert(!glass_music_seek(track,9000));
  usleep(50000);assert(!glass_music_set_paused(false));
  for(unsigned i=0;i<3000&&!atomic_load(&captured_pcm);i++)usleep(1000);
  s=wait_for("Local",MUSIC_PLAYING,MUSIC_LYRICS_MISSING);
  assert(s.position_ms>=9000 && s.position_ms<10000);
  assert(atomic_load(&captured_pcm)&&!memcmp(captured_samples,oracle+198450,sizeof(captured_samples)));
  assert(!glass_music_seek(track,0));
  s=wait_for("Local",MUSIC_PLAYING,MUSIC_LYRICS_MISSING);assert(s.position_ms<1000);
  assert(!glass_music_stop_wait(3000));assert(glass_music_seek(track,10)==-ESTALE);
  free(oracle);
  puts("MUSIC_SEEK_OK: sample-exact oracle at 2s/9s, stale track rejection, rapid last request wins, backward/forward/zero seek, pause retained, sample clock rebased");

  char path[256];assert(!music_audio_cache_path("https://fixture/audio",path,sizeof(path)));unlink(path);
  atomic_store(&hold_audio,true);atomic_store(&audio_held,false);
  unsigned pauses=atomic_load(&pcm_pauses),requests=atomic_load(&audio_requests);
  play("BufferTest");
  for(unsigned i=0;i<3000;i++) {
    glass_music_now_playing(&s);
    if(atomic_load(&audio_held)&&atomic_load(&pcm_pauses)>pauses&&s.state==MUSIC_LOADING)break;
    usleep(1000);
  }
  assert(atomic_load(&pcm_pauses)>pauses&&s.state==MUSIC_LOADING);
  unsigned frozen=s.position_ms;usleep(30000);glass_music_now_playing(&s);assert(s.position_ms==frozen);
  atomic_store(&hold_audio,false);
  wait_for("BufferTest",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  wait_for("BufferTest",MUSIC_ENDED,MUSIC_LYRICS_READY);
  assert(atomic_load(&audio_requests)==requests+1);
  fputs("MUSIC_REBUFFER_OK: injected network stall pauses PCM, freezes position, resumes and finishes\n",stdout);
  FILE *cached=music_audio_cache_open("https://fixture/audio");assert(cached);fclose(cached);
  play("BufferTest");wait_for("BufferTest",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  assert(atomic_load(&audio_requests)==requests+1);
  assert(!glass_music_stop_wait(3000));
  puts("MUSIC_AUDIO_CACHE_HIT_OK: replay feeds real decoder from SD without another audio HTTP request");
  /* An unavailable cache directory must leave streaming functional. */
  char saved[300];snprintf(saved,sizeof(saved),"%s.saved",MUSIC_CACHE_ROOT);
  assert(!rename(MUSIC_CACHE_ROOT,saved));
  FILE *blocked=fopen(MUSIC_CACHE_ROOT,"wb");assert(blocked);fclose(blocked);
  requests=atomic_load(&audio_requests);
  play("NoCache");wait_for("NoCache",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  assert(atomic_load(&audio_requests)==requests+1);assert(!glass_music_stop_wait(3000));
  assert(!unlink(MUSIC_CACHE_ROOT)&&!rename(saved,MUSIC_CACHE_ROOT));
  puts("MUSIC_AUDIO_CACHE_UNAVAILABLE_OK: real streaming and teardown survive inaccessible SD cache");
}

static void volume_checks(void)
{
  unsigned levels[]={0,1,25,75,100};
  struct music_snapshot snapshot;struct music_now_playing now;
  for(unsigned i=0;i<sizeof(levels)/sizeof(*levels);i++){
    assert(!glass_speaker_volume_set(levels[i]));
    glass_music_get(&snapshot);glass_music_now_playing(&now);
    assert(snapshot.volume==levels[i]&&now.volume==levels[i]);
    glass_music_volume(100-levels[i]);
    assert(glass_speaker_volume()==100-levels[i]);
    glass_music_get(&snapshot);glass_music_now_playing(&now);
    assert(snapshot.volume==100-levels[i]&&now.volume==snapshot.volume);
  }
  glass_music_volume(UINT_MAX);assert(glass_speaker_volume()==100);
  assert(glass_speaker_volume_set(101)==-EINVAL&&glass_speaker_volume()==100);
  glass_music_volume(40);
  puts("MUSIC_MASTER_VOLUME_OK: real shared speaker state; settings/music bidirectional updates; snapshot/now-playing; mute/range; single master gain");
}
int main(int argc,char **argv)
{
  if(argc==4 && !strcmp(argv[1],"--lyrics-response"))return live_response(argv[2],argv[3]);
  assert(argc==2);mp3_path=argv[1];volume_checks();parser_checks();bilingual_checks();stream_checks();cache_checks();audio_cache_checks();
  /* Test the public bounded view against the real retained lyric store. */
  assert(!lyrics);lyrics=calloc(1,sizeof(*lyrics));assert(lyrics);
  lyrics->count=MUSIC_LRC_LINES;lyrics_state=MUSIC_LYRICS_READY;
  for(unsigned i=0;i<lyrics->count;i++){
    lyrics->lines[i].ms=i*1000;snprintf(lyrics->lines[i].text,MUSIC_LYRIC_TEXT_MAX,"line %u",i);
  }
  struct music_lyrics_window page;
  state.position_ms=0;glass_music_lyrics_read(0,&page);
  assert(page.total==192&&page.count==9&&page.first==0&&page.current==0);
  state.position_ms=191999;glass_music_lyrics_read(UINT32_MAX,&page);
  assert(page.first==183&&page.count==9&&page.current==191&&!strcmp(page.lines[8],"line 191"));
  assert(page.start_ms[8]==191000&&page.end_ms[7]==191000);
  page.lines[8][0]='X';glass_music_lyrics_read(183,&page);assert(!strcmp(page.lines[8],"line 191"));
  free(lyrics);lyrics=NULL;lyrics_state=MUSIC_LYRICS_NONE;state.position_ms=0;
  glass_music_lyrics_read(0,&page);assert(page.total==0&&page.count==0&&page.current==-1);
  puts("MUSIC_LYRICS_WINDOW_OK: first/last of 192 lines, bounded 9-row copies, independent storage, cleared lyrics");
  glass_music_start();glass_music_start();play("B");
  struct music_now_playing s=wait_for("B",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  unsigned fetched=atomic_load(&metadata_fetches);
  struct music_song cached_song={0};strcpy(cached_song.name,"B");strcpy(cached_song.artist,"Demo");strcpy(cached_song.album,"Test");
  struct music_lrc *cached_lrc=calloc(1,sizeof(*cached_lrc));assert(cached_lrc);char cached_cover[1024];
  assert(!fetch_lyrics(&cached_song,play_generation,cached_lrc,cached_cover));
  assert(cached_lrc->count==3&&strstr(cached_lrc->lines[0].text,"B intro")&&!strcmp(cached_cover,s.cover_url));
  assert(atomic_load(&metadata_fetches)==fetched);free(cached_lrc);
  puts("MUSIC_LYRIC_CACHE_HIT_OK: retained timing and artwork without a second HTTP request");
  while(s.position_ms<600){usleep(5000);glass_music_now_playing(&s);}
  assert(!strcmp(s.lyrics[1],"B current"));
  assert(!strcmp(s.cover_url,"https://fixture/320x320.jpg"));
  struct music_lyrics_window window;glass_music_lyrics_read(0,&window);
  assert(window.track_id==s.track_id&&window.state==MUSIC_LYRICS_READY);
  assert(window.total&&window.count<=MUSIC_LYRICS_WINDOW&&window.current>=0);
  assert(strstr(window.lines[window.current-window.first],"B "));
  strcpy(window.lines[0],"not owned by the service");
  glass_music_lyrics_read(UINT32_MAX,&window);
  assert(window.first+window.count==window.total);
  assert(strcmp(window.lines[0],"not owned by the service"));
  glass_music_toggle();usleep(30000);glass_music_now_playing(&s);assert(s.state==MUSIC_PAUSED);
  volume_checks();
  unsigned position=s.position_ms;usleep(60000);glass_music_now_playing(&s);assert(s.position_ms==position);
  glass_music_toggle();wait_for("B",MUSIC_PLAYING,MUSIC_LYRICS_READY);
  /* Real workers keep advancing with no LVGL/music page alive. */
  usleep(50000);glass_music_now_playing(&s);assert(s.position_ms>position);
  atomic_store(&hold_lyrics,true);play("A");
  for(unsigned i=0;!atomic_load(&lyrics_entered)&&i<3000;i++)usleep(1000);
  assert(atomic_load(&lyrics_entered));play("C");glass_music_now_playing(&s);
  assert(!s.lyrics[1][0] && !s.cover_url[0] && s.state==MUSIC_LOADING);atomic_store(&hold_lyrics,false);
  s=wait_for("C",MUSIC_PLAYING,MUSIC_LYRICS_READY);assert(strstr(s.lyrics[1],"C ") && !strstr(s.lyrics[1],"A "));
  struct music_song favorite={0};strcpy(favorite.name,"B");strcpy(favorite.artist,"Demo");strcpy(favorite.query,"B");assert(!glass_music_favorite(&favorite));
  for(unsigned i=0;i<3000;i++){glass_music_now_playing(&s);if(s.can_skip)break;usleep(1000);}assert(s.can_skip);
  assert(!glass_music_skip(1));wait_for("B",MUSIC_PLAYING,MUSIC_LYRICS_READY);assert(glass_music_skip(0)==-EINVAL);
  play("CoverOnly");s=wait_for("CoverOnly",MUSIC_PLAYING,MUSIC_LYRICS_MISSING);assert(s.cover_url[0]);
  play("NoLyrics");wait_for("NoLyrics",MUSIC_PLAYING,MUSIC_LYRICS_MISSING);
  play("Missing");wait_for("Missing",MUSIC_ERROR,MUSIC_LYRICS_NONE);
  assert(!glass_music_stop_wait(3000));glass_music_now_playing(&s);assert(s.state==MUSIC_STOPPED && !s.lyrics[1][0]);
  assert(atomic_load(&opens)>2 && atomic_load(&opens)==atomic_load(&closes));
  printf("MUSIC_BACKGROUND_SERVICE_OK: real MP3 decoder/workers, pause freezes clock, resume, stale lyric cancellation, skip, missing lyrics, network failure and %u/%u PCM teardown\n",atomic_load(&opens),atomic_load(&closes));
  missing_key_checks();mcp_checks();selected_result_checks();seek_buffer_checks();
  struct music_song local={0};strcpy(local.name,"Local");strcpy(local.artist,"Demo");snprintf(local.url,sizeof(local.url),"%s",mp3_path);
  assert(!glass_music_play(&local));wait_for("Local",MUSIC_PLAYING,MUSIC_LYRICS_MISSING);
  assert(!glass_music_set_paused(true));usleep(30000);glass_music_now_playing(&s);unsigned frozen=s.position_ms;
  usleep(50000);glass_music_now_playing(&s);assert(s.state==MUSIC_PAUSED&&s.position_ms==frozen);
  assert(!glass_music_set_paused(false));usleep(50000);glass_music_now_playing(&s);assert(s.position_ms>frozen);
  assert(!glass_music_stop_wait(3000));assert(atomic_load(&opens)==atomic_load(&closes));
  assert(music_sd_stream(MUSIC_SD_ROOT "../escape.mp3",NULL,NULL,NULL)==-EINVAL);
  assert(music_sd_stream(MUSIC_SD_ROOT "nested/escape.mp3",NULL,NULL,NULL)==-EINVAL);
  puts("MUSIC_SD_SERVICE_OK: real SD source and MP3 decoder, pause/resume clock, cancellation/PCM teardown and path rejection");
  return 0;
}
