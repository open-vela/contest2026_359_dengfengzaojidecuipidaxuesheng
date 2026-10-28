/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "glass_music.h"
#include "glass_music_sd.h"
#include "glass_music_track_key.h"
#include <limits.h>
#include "glass_portal.h"
#include "glass_music_pcm.h"
#include "glass_device_controls.h"
#include "glass_music_duration.h"
#include "glass_music_lyrics.h"
#include "glass_music_cache.h"
#include "glass_music_audio_cache.h"
#include "qpk_storage.h"
#include <cJSON.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if __has_include("music_credentials.h")
#include "music_credentials.h"
#else
#define GLASS_MUSIC_API_KEY ""
#endif
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "music_vendor/minimp3.h"

#define RING_CAP (256 * 1024)
#define PREFILL (48 * 1024)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static struct music_snapshot state;
static bool search_started, player_started, want_play, want_pause, want_search, want_favorite;
static bool want_lyrics;
static struct music_song pending_lyrics;
static unsigned lyrics_generation;
static struct music_lrc *lyrics;
static enum music_lyrics_state lyrics_state;
static char current_cover[1024];
static bool player_active;
static unsigned play_generation, search_generation;
static unsigned seek_revision, seek_position_ms;
static struct music_song pending_song, pending_favorite;
static char pending_query[128];
static uint64_t result_deadline, pending_result_deadline;
static unsigned consumed_result;

static uint64_t music_now_ms(void)
{
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

struct transfer {
  unsigned generation;
  bool search;
  char *body;
  size_t used;
};

static bool transfer_cancelled(void *ctx)
{
  struct transfer *t = ctx;
  pthread_mutex_lock(&lock);
  bool cancelled = t->generation != (t->search ? search_generation : play_generation);
  pthread_mutex_unlock(&lock);
  return cancelled;
}

static int collect_json(void *ctx, const unsigned char *data, size_t size)
{
  struct transfer *t = ctx;
  if (size > 16384 - t->used) return -E2BIG;
  memcpy(t->body + t->used, data, size); t->used += size;
  return 0;
}

static int lookup(const char *query, struct music_song *song, unsigned generation, bool search)
{
  struct transfer t = {.generation = generation, .search = search};
  t.body = malloc(16385);
  char *url = malloc(2048);
  int ret = -ENOMEM;
  if (!t.body || !url) goto done;
  char custom_key[129];
  ret = music_search_url(query, portal_music_key(custom_key,sizeof(custom_key)) == 0 ? custom_key : GLASS_MUSIC_API_KEY, url, 2048);
  if (!ret) ret = music_http_get(url, collect_json, &t, transfer_cancelled, NULL);
  if (!ret) ret = music_song_parse(t.body, t.used, query, song);
done:
  free(t.body); free(url);
  return ret;
}

/* Lyrics use the existing search worker, so audio never waits for the network
 * or SD and lock/unlock does not create another decoder or background thread. */
#define LYRIC_OBJECT_MAX 65536
#ifndef MUSIC_LYRICS_ROOT
#define MUSIC_LYRICS_ROOT "/sdcard/music/lyrics"
#endif
struct lyric_transfer {
  struct transfer transfer;
  const struct music_song *song;
  struct music_lrc *out;
  size_t total;
  unsigned depth,candidates;
  bool array,quoted,escaped,oversized,found,local_lyrics;
  char *cover;
  int lyric_error;
};

static void music_metadata_cover(const cJSON *item,char *url)
{
  if(!url)return;url[0]=0;
  const cJSON *field=cJSON_GetObjectItemCaseSensitive(item,"cover_format");
  if(!cJSON_IsString(field) || !field->valuestring[0])field=cJSON_GetObjectItemCaseSensitive(item,"cover");
  if(!cJSON_IsString(field) || strncmp(field->valuestring,"https://",8))return;
  const char *p=field->valuestring;size_t n=0;
  while(*p) {
    if((unsigned char)*p<33 || n>=1023){url[0]=0;return;}
    if(!strncmp(p,"{w}",3) || !strncmp(p,"{h}",3)) {
      if(n+3>=1024){url[0]=0;return;}memcpy(url+n,"320",3);n+=3;p+=3;
    } else {if(*p=='{' || *p=='}'){url[0]=0;return;}url[n++]=*p++;}
  }
  url[n]=0;
}

/* The endpoint can return many covers and large TTML fields. Parse one bounded
 * JSON object at a time, stopping as soon as title AND artist match. */
static int collect_lyrics(void *ctx,const unsigned char *data,size_t size)
{
  struct lyric_transfer *t=ctx;
  if(size>512*1024-t->total)return -E2BIG;
  t->total+=size;
  for(size_t i=0;i<size;i++) {
    unsigned char c=data[i];if(!c)return -EINVAL;
    if(!t->array){if(isspace(c))continue;if(c!='[')return -EINVAL;t->array=true;continue;}
    if(!t->depth) {
      if(isspace(c)||c==','||c==']')continue;
      if(c!='{')return -EINVAL;
      if(++t->candidates>32)return -E2BIG;
      t->transfer.used=0;t->oversized=false;
    }
    if(t->transfer.used<LYRIC_OBJECT_MAX)t->transfer.body[t->transfer.used++]=c;
    else t->oversized=true;
    if(t->quoted) {
      if(t->escaped)t->escaped=false;else if(c=='\\')t->escaped=true;else if(c=='"')t->quoted=false;
    } else if(c=='"')t->quoted=true;
    else if(c=='{'||c=='['){if(++t->depth>8)return -E2BIG;}
    else if(c=='}'||c==']') {
      if(!t->depth)return -EINVAL;
      if(--t->depth)continue;
      if(t->oversized)continue;
      t->transfer.body[t->transfer.used]=0;
      cJSON *item=cJSON_ParseWithOpts(t->transfer.body,NULL,true);
      if(!cJSON_IsObject(item)){cJSON_Delete(item);return -EINVAL;}
      const cJSON *title=cJSON_GetObjectItemCaseSensitive(item,"title");
      const cJSON *artist=cJSON_GetObjectItemCaseSensitive(item,"artist");
      const cJSON *lrc=cJSON_GetObjectItemCaseSensitive(item,"lyrics");
      if(!cJSON_IsString(lrc))lrc=cJSON_GetObjectItemCaseSensitive(item,"lrc");
      bool match=cJSON_IsString(title)&&cJSON_IsString(artist)&&
        music_lyrics_identity(title->valuestring,t->song->name)&&music_lyrics_identity(artist->valuestring,t->song->artist);
      int ret=-ENOENT;bool artwork=false;
      if(match) {
        char candidate[1024];music_metadata_cover(item,candidate);
        artwork=candidate[0]!=0;
        if(t->cover&&artwork)memcpy(t->cover,candidate,strlen(candidate)+1);
        ret=t->local_lyrics?0:cJSON_IsString(lrc)?music_lrc_parse(lrc->valuestring,strlen(lrc->valuestring),t->out):-ENODATA;
        if(!ret){
          const cJSON *translated=cJSON_GetObjectItemCaseSensitive(item,"translated_lyrics");
          if(!cJSON_IsString(translated))translated=cJSON_GetObjectItemCaseSensitive(item,"translation");
          if(!cJSON_IsString(translated))translated=cJSON_GetObjectItemCaseSensitive(item,"tlyric");
          if(cJSON_IsObject(translated))translated=cJSON_GetObjectItemCaseSensitive(translated,"lyric");
          if(cJSON_IsString(translated))music_lrc_translation(t->out,translated->valuestring,strlen(translated->valuestring));
        }
      }
      cJSON_Delete(item);
      if(match && (!ret || artwork)) {t->lyric_error=ret;t->found=true;return -EALREADY;}
    }
  }
  return 0;
}

static bool lyric_filename(const char *s)
{
  for(;*s;s++)if((unsigned char)*s<32 || *s=='/' || *s=='\\')return false;
  return true;
}

static int fetch_lyrics(const struct music_song *song,unsigned generation,struct music_lrc *out,char *cover)
{
  struct transfer t={.generation=generation};
  t.body=malloc(MUSIC_LRC_MAX+1);
  if(!t.body)return -ENOMEM;
  int ret=-ENOENT;bool local_lyrics=false;cover[0]=0;
  /* A user-supplied exact song/artist sidecar takes priority over fuzzy APIs. */
  if(lyric_filename(song->name) && lyric_filename(song->artist)) {
    char path[512];
    int n=snprintf(path,sizeof(path),MUSIC_LYRICS_ROOT "/%s - %s.lrc",song->name,song->artist);
    FILE *f=n>0&&(size_t)n<sizeof(path)&&strlen(song->name)+strlen(song->artist)+7<=NAME_MAX?fopen(path,"rb"):NULL;
    if(!f){
      snprintf(path,sizeof(path),MUSIC_LYRICS_ROOT "/%08lx.lrc",(unsigned long)music_track_key(song->name,song->artist));
      f=fopen(path,"rb");
    }
    if(f) {
      t.used=fread(t.body,1,MUSIC_LRC_MAX+1,f);
      ret=ferror(f)?-EIO:music_lrc_parse(t.body,t.used,out);
      fclose(f);
      if(ret){free(t.body);return ret;}
      local_lyrics=true; /* Preserve local timing; query metadata for artwork. */
    }
  }
  free(t.body);
  /* Offline audio must not wait on an artwork API before publishing its
   * local sidecar. A disconnected router cannot stall an SD-only player. */
  if(!strncmp(song->url,MUSIC_SD_ROOT,sizeof(MUSIC_SD_ROOT)-1))return local_lyrics?0:-ENOENT;
  /* Cover fetching is independent of lyric availability. Prefer a small exact
   * metadata cover, but keep the official cover endpoint when JSON has none. */
  if(music_cover_url(song,cover,1024))cover[0]=0;
  struct lyric_transfer stream={.transfer={.generation=generation},.song=song,.out=out,.cover=cover,.local_lyrics=local_lyrics};
  stream.transfer.body=malloc(LYRIC_OBJECT_MAX+1);
  if(!stream.transfer.body)return -ENOMEM;
  char url[2048];unsigned status=0;
  ret=music_lyrics_url(song,url,sizeof(url));
  if(!ret){
    size_t cached_size=0;unsigned char *cached=music_cache_read('l',url,LYRIC_OBJECT_MAX,0,&cached_size);
    if(cached){
      stream.array=true;int hit=collect_lyrics(&stream,cached,cached_size);free(cached);
      if(hit==-EALREADY&&stream.found){ret=stream.lyric_error;free(stream.transfer.body);return ret;}
      char *body=stream.transfer.body;
      stream=(struct lyric_transfer){.transfer={.generation=generation,.body=body},.song=song,.out=out,.cover=cover,.local_lyrics=local_lyrics};
    }
  }
  if(!ret)ret=music_http_get(url,collect_lyrics,&stream,transfer_cancelled,&status);
  if(ret==-EALREADY && stream.found && status==200){
    ret=stream.lyric_error;
    /* Cache successful timed lyrics, not transient empty/not-found responses. */
    if(!ret&&!local_lyrics&&!transfer_cancelled(&stream))music_cache_write('l',url,stream.transfer.body,stream.transfer.used,LYRIC_OBJECT_MAX);
  }else {if(local_lyrics)ret=0;else if(!ret)ret=-ENODATA;}
  free(stream.transfer.body);return ret;
}

static void clear_lyrics_locked(void)
{
  free(lyrics);lyrics=NULL;want_lyrics=false;lyrics_state=MUSIC_LYRICS_NONE;current_cover[0]=0;
}

/* A translated sidecar is optional and must match the original timestamps.
 * It never changes the audio URL, original timings or another song's cache. */
static void finish_lyrics(const struct music_song *song,struct music_lrc *out)
{
  char path[512];FILE *f=NULL;
  if(lyric_filename(song->name)&&lyric_filename(song->artist)&&
     strlen(song->name)+strlen(song->artist)+10<=NAME_MAX){
    snprintf(path,sizeof(path),MUSIC_LYRICS_ROOT "/%s - %s.zh.lrc",song->name,song->artist);
    f=fopen(path,"rb");
  }
  if(!f){snprintf(path,sizeof(path),MUSIC_LYRICS_ROOT "/%08lx.zh.lrc",(unsigned long)music_track_key(song->name,song->artist));f=fopen(path,"rb");}
  if(f){char *body=malloc(MUSIC_LRC_MAX+1);
    if(body){size_t n=fread(body,1,MUSIC_LRC_MAX+1,f);
      if(!ferror(f))music_lrc_translation(out,body,n);free(body);}
    fclose(f);
  }
  music_lrc_simplify(out);
}

bool glass_music_is_favorite(const struct music_snapshot *s, const struct music_song *song)
{
  if (!song || !song->name[0]) return false;
  for (unsigned i = 0; i < s->favorite_count; i++)
    if (!strcmp(s->favorites[i].name, song->name) && !strcmp(s->favorites[i].artist, song->artist)) return true;
  return false;
}

static int save_favorites(struct music_song *songs, unsigned count)
{
  cJSON *array = cJSON_CreateArray();
  if (!array) return -ENOMEM;
  int ret = -ENOMEM;
  for (unsigned i = 0; i < count; i++) {
    cJSON *song = cJSON_CreateObject();
    if (!song) goto done;
    cJSON_AddItemToArray(array, song);
    if (!cJSON_AddStringToObject(song, "name", songs[i].name) ||
        !cJSON_AddStringToObject(song, "artist", songs[i].artist) ||
        !cJSON_AddStringToObject(song, "album", songs[i].album) ||
        !cJSON_AddStringToObject(song, "query", songs[i].query)) goto done;
  }
  char *json = cJSON_PrintUnformatted(array);
  if (json) {
    ret = qpk_storage_write("/data/qpk", "music", "favorites", json, strlen(json));
    cJSON_free(json);
  }
done:
  cJSON_Delete(array);
  return ret;
}

static void load_favorites(void)
{
  char *json = malloc(QPK_STORAGE_VALUE_MAX + 1);
  if (!json) return;
  size_t size = 0;
  int ret = qpk_storage_read("/data/qpk", "music", "favorites", json, QPK_STORAGE_VALUE_MAX + 1, &size);
  if (!ret) {
    json[size] = 0;
    cJSON *array = cJSON_ParseWithOpts(json, NULL, true);
    if (cJSON_IsArray(array) && cJSON_GetArraySize(array) <= MUSIC_FAVORITES_MAX) {
      cJSON *item;
      cJSON_ArrayForEach(item, array) {
        struct music_song song = {0};
        const char *keys[] = {"name", "artist", "album", "query"};
        char *values[] = {song.name, song.artist, song.album, song.query};
        size_t caps[] = {sizeof(song.name), sizeof(song.artist), sizeof(song.album), sizeof(song.query)};
        bool valid = cJSON_IsObject(item);
        for (unsigned i = 0; i < 4 && valid; i++) {
          cJSON *v = cJSON_GetObjectItemCaseSensitive(item, keys[i]);
          valid = cJSON_IsString(v) && strlen(v->valuestring) < caps[i];
          if (valid) strcpy(values[i], v->valuestring);
        }
        if (valid && song.name[0] && song.query[0]) state.favorites[state.favorite_count++] = song;
      }
    } else ret = -EINVAL;
    cJSON_Delete(array);
  }
  state.storage_error = ret == -ENOENT ? 0 : ret;
  free(json);
}

static void *search_worker(void *unused)
{
  (void)unused;
  pthread_setname_np(pthread_self(), "music_search");
  pthread_mutex_lock(&lock);
  load_favorites();
  state.initialized = true; state.revision++;
  pthread_mutex_unlock(&lock);
  for (;;) {
    pthread_mutex_lock(&lock);
    while (!want_search && !want_favorite && !want_lyrics) pthread_cond_wait(&wake, &lock);
    if (want_lyrics && !want_search && !want_favorite) {
      struct music_song song=pending_lyrics;unsigned generation=lyrics_generation;
      want_lyrics=false;pthread_mutex_unlock(&lock);
      struct music_lrc *parsed=calloc(1,sizeof(*parsed));
      char cover[1024]={0};
      int ret=parsed?fetch_lyrics(&song,generation,parsed,cover):-ENOMEM;
      if(!ret)finish_lyrics(&song,parsed);
      pthread_mutex_lock(&lock);
      if(generation==play_generation) {
        free(lyrics);lyrics=ret?NULL:parsed;
        if(!ret)parsed=NULL;
        memcpy(current_cover,cover,sizeof(current_cover));
        lyrics_state=ret?MUSIC_LYRICS_MISSING:MUSIC_LYRICS_READY;
        state.revision++;
      }
      pthread_mutex_unlock(&lock);free(parsed);continue;
    }
    if (want_favorite) {
      struct music_song *songs = malloc(sizeof(state.favorites));
      unsigned count = state.favorite_count;
      struct music_song song = pending_favorite;
      want_favorite = false;
      if (songs) memcpy(songs, state.favorites, sizeof(state.favorites));
      pthread_mutex_unlock(&lock);
      int ret = -ENOMEM;
      if (songs) {
        unsigned i;
        for (i = 0; i < count; i++) if (!strcmp(songs[i].name, song.name) && !strcmp(songs[i].artist, song.artist)) break;
        if (i < count) { memmove(songs+i, songs+i+1, (count-i-1)*sizeof(*songs)); count--; ret = 0; }
        else if (count >= MUSIC_FAVORITES_MAX) ret = -ENOSPC;
        else { memmove(songs+1, songs, count*sizeof(*songs)); songs[0] = song; songs[0].url[0] = 0; count++; ret = 0; }
        if (!ret) ret = save_favorites(songs, count);
      }
      pthread_mutex_lock(&lock);
      if (!ret) { memcpy(state.favorites, songs, count*sizeof(*songs)); state.favorite_count = count; }
      state.storage_error = ret; state.revision++;
      pthread_mutex_unlock(&lock);
      free(songs);
      continue;
    }
    char query[128]; strcpy(query, pending_query);
    unsigned generation = search_generation;
    want_search = false;
    pthread_mutex_unlock(&lock);
    struct music_song song = {0};
    int ret = lookup(query, &song, generation, true);
    pthread_mutex_lock(&lock);
    if (generation == search_generation) {
      state.searching = false; state.search_error = ret; state.has_result = ret == 0;
      if (!ret) state.result = song;
      if (++state.search_revision == 0) state.search_revision++;
      result_deadline = ret ? 0 : music_now_ms() + 60000;
      state.revision++;
      printf("[music] search result=%d\n", ret);
    }
    pthread_mutex_unlock(&lock);
  }
  return NULL;
}

struct stream {
  struct transfer transfer; /* generation first; shared cancellation callback */
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  unsigned char *ring;
  size_t read, used;
  bool done, abort;
  int error;
  const char *url;
  unsigned seek;
  struct music_audio_cache cache;
};

static bool stream_cancelled(void *ctx)
{
  struct stream *s = ctx;
  pthread_mutex_lock(&s->mutex); bool abort = s->abort; pthread_mutex_unlock(&s->mutex);
  pthread_mutex_lock(&lock);
  bool stale = s->seek != seek_revision;
  pthread_mutex_unlock(&lock);
  return abort || stale || transfer_cancelled(&s->transfer);
}

static void stream_wait(struct stream *s)
{
  struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
  t.tv_nsec += 100000000;
  if (t.tv_nsec >= 1000000000) { t.tv_nsec -= 1000000000; t.tv_sec++; }
  pthread_cond_timedwait(&s->condition, &s->mutex, &t);
}

static int stream_sink(void *ctx, const unsigned char *data, size_t size)
{
  struct stream *s = ctx;
  music_audio_cache_append(&s->cache, data, size);
  while (size) {
    if (stream_cancelled(s)) return -ECANCELED;
    pthread_mutex_lock(&s->mutex);
    if (s->used == RING_CAP) { stream_wait(s); pthread_mutex_unlock(&s->mutex); continue; }
    size_t pos = (s->read + s->used) % RING_CAP;
    size_t take = RING_CAP - s->used;
    if (take > RING_CAP-pos) take = RING_CAP-pos;
    if (take > size) take = size;
    memcpy(s->ring+pos, data, take); s->used += take;
    pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->mutex);
    data += take; size -= take;
  }
  return 0;
}

static void *network_worker(void *ctx)
{
  struct stream *s = ctx;
  pthread_setname_np(pthread_self(), "music_net");
  bool local = !strncmp(s->url, MUSIC_SD_ROOT, sizeof(MUSIC_SD_ROOT)-1);
  FILE *cached = local ? NULL : music_audio_cache_open(s->url);
  int ret = 0;
  if (cached) {
    unsigned char bytes[4096]; size_t got;
    while (!ret && (got = fread(bytes, 1, sizeof(bytes), cached)))
      ret = stream_sink(s, bytes, got);
    if (!ret && ferror(cached)) ret = -EIO;
    fclose(cached);
  } else {
    if (!local) music_audio_cache_begin(&s->cache, s->url);
    ret = local ? music_sd_stream(s->url, stream_sink, s, stream_cancelled) :
      music_http_get(s->url, stream_sink, s, stream_cancelled, NULL);
    music_audio_cache_finish(&s->cache, !ret && !stream_cancelled(s));
  }
  pthread_mutex_lock(&s->mutex);
  s->error = ret; s->done = true;
  pthread_cond_broadcast(&s->condition);
  pthread_mutex_unlock(&s->mutex);
  return NULL;
}

static int spawn(pthread_t *thread, void *(*entry)(void *), void *arg, size_t stack)
{
  pthread_attr_t attr; pthread_attr_init(&attr);
  int ret = pthread_attr_setstacksize(&attr, stack);
  if (!ret) ret = pthread_create(thread, &attr, entry, arg);
  pthread_attr_destroy(&attr);
  return ret ? -ret : 0;
}

static int play_stream(struct music_song *song, unsigned generation,
                       unsigned seek, unsigned target_ms)
{
  struct stream s = {.transfer = {.generation = generation},
    .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
    .url = song->url, .seek = seek};
  s.ring = malloc(RING_CAP);
  unsigned char *input = malloc(32768);
  mp3dec_t *decoder = calloc(1, sizeof(*decoder));
  int16_t *samples = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(*samples));
  struct music_pcm pcm = {.fd = -1, .mq = (mqd_t)-1};
  int ret = -ENOMEM; bool network_started = false, paused = false;
  pthread_t network;
  if (!s.ring || !input || !decoder || !samples) goto done;
  mp3dec_init(decoder);
  ret = spawn(&network, network_worker, &s, 32768);
  if (ret) goto done;
  network_started = true;
  size_t buffered = 0, skipped = 0;
  bool prefilled = false, duration_checked = false;
  bool rebuffering = false;
  uint64_t decoded = 0, base_samples = 0;
  while (!stream_cancelled(&s)) {
    pthread_mutex_lock(&lock);
    bool pause = want_pause;
    pthread_mutex_unlock(&lock);
    if (pause != paused && !rebuffering) {
      /* A pause may arrive while the track is still loading, before PCM open. */
      if (pcm.fd >= 0) { ret = music_pcm_pause(&pcm, pause); if (ret) break; }
      paused = pause;
    }
    /* Decode silently to a seek target even when the user paused playback. */
    if (pause && !target_ms) { usleep(20000); continue; }
    pthread_mutex_lock(&s.mutex);
    if (!prefilled && s.used < PREFILL && !s.done) { stream_wait(&s); pthread_mutex_unlock(&s.mutex); continue; }
    prefilled = true;
    while (s.used && buffered < 32768) {
      size_t take = RING_CAP-s.read;
      if (take > s.used) take = s.used;
      if (take > 32768-buffered) take = 32768-buffered;
      memcpy(input+buffered, s.ring+s.read, take);
      buffered += take; s.read = (s.read+take)%RING_CAP; s.used -= take;
    }
    pthread_cond_broadcast(&s.condition);
    bool eof = s.done && !s.used;
    int network_error = s.error;
    /* Keep complete MP3 lookahead, but never block a running DMA queue on a
     * fixed 16 KiB network read. Pause before starvation and refill with
     * hysteresis, preserving the decoder reservoir and all queued samples. */
    if (buffered < (rebuffering ? 24576u : 4096u) && !eof) {
      pthread_mutex_unlock(&s.mutex);
      if (pcm.fd >= 0 && !paused) {
        ret = music_pcm_pause(&pcm, true); if (ret) break;
        paused = true;
      }
      rebuffering = true;
      pthread_mutex_lock(&lock);
      if (generation == play_generation && seek == seek_revision) {
        state.state = want_pause ? MUSIC_PAUSED : MUSIC_LOADING; state.revision++;
      }
      pthread_mutex_unlock(&lock);
      pthread_mutex_lock(&s.mutex);
      if (!s.done) stream_wait(&s);
      pthread_mutex_unlock(&s.mutex); continue;
    }
    pthread_mutex_unlock(&s.mutex);
    rebuffering = false;
    if (paused && !pause && pcm.fd >= 0) {
      ret = music_pcm_pause(&pcm, false); if (ret) break;
      paused = false;
    }
    if (!buffered) { ret = network_error; break; }
    mp3dec_frame_info_t info = {0};
    int frames = mp3dec_decode_frame(decoder, input, buffered, samples, &info);
    if (info.frame_bytes < 0 || (size_t)info.frame_bytes > buffered) { ret = -EINVAL; break; }
    if (!duration_checked && info.hz > 0 && info.layer == 3 &&
        info.frame_offset >= 0 && info.frame_offset < info.frame_bytes) {
      unsigned duration = music_frame_duration(input + info.frame_offset,
        info.frame_bytes - info.frame_offset, info.hz);
      duration_checked = true;
      pthread_mutex_lock(&lock);
      if (generation == play_generation && seek == seek_revision) { state.duration_ms = duration; state.revision++; }
      pthread_mutex_unlock(&lock);
    }
    if (info.frame_bytes) { memmove(input, input+info.frame_bytes, buffered-info.frame_bytes); buffered -= info.frame_bytes; }
    else if (eof) { ret = network_error; break; }
    else { ret = -EINVAL; break; }
    if (!frames) { skipped += info.frame_bytes; if (skipped > 1024*1024) { ret = -EINVAL; break; } continue; }
    skipped = 0;
    uint64_t target = (uint64_t)target_ms * info.hz / 1000;
    unsigned trim = target > decoded ? (target - decoded > (unsigned)frames ?
      (unsigned)frames : (unsigned)(target - decoded)) : 0;
    decoded += frames;
    if (trim == (unsigned)frames) continue;
    if (target_ms) {
      base_samples = target;
      target_ms = 0;
      if (pause) {
        /* Retain this decoded frame until resume; do not start audible DMA. */
        while (!stream_cancelled(&s)) {
          pthread_mutex_lock(&lock); pause = want_pause; pthread_mutex_unlock(&lock);
          if (!pause) break;
          usleep(20000);
        }
        if (stream_cancelled(&s)) break;
      }
    }
    if (pcm.fd < 0) {
      /* The shared PCM writer applies the master speaker gain. A second
       * music-specific codec gain would attenuate the same setting twice. */
      ret = music_pcm_open(&pcm, info.hz, 100);
      if (ret) break;
    }
    if (pcm.rate != (unsigned)info.hz) { ret = -EINVAL; break; }
    ret = music_pcm_write(&pcm, samples + trim * info.channels, frames - trim, info.channels);
    if (ret) break;
    pthread_mutex_lock(&lock);
    if (generation == play_generation && seek == seek_revision) {
      state.state = want_pause ? MUSIC_PAUSED : MUSIC_PLAYING;
      state.position_ms = (base_samples + pcm.played_samples) * 1000 / pcm.rate;
      state.revision++;
    }
    pthread_mutex_unlock(&lock);
  }
  if (!ret && !stream_cancelled(&s) && pcm.fd >= 0) {
    ret = music_pcm_drain(&pcm);
    pthread_mutex_lock(&lock);
    if(generation==play_generation && seek==seek_revision && !ret) {
      state.position_ms=(base_samples+pcm.played_samples)*1000/pcm.rate;state.revision++;
    }
    pthread_mutex_unlock(&lock);
  }
  if (!ret && pcm.fd < 0) ret = target_ms && decoded ? 0 : -ENODATA;
done:
  pthread_mutex_lock(&s.mutex); s.abort = true; pthread_cond_broadcast(&s.condition); pthread_mutex_unlock(&s.mutex);
  music_pcm_close(&pcm);
  if (network_started) pthread_join(network, NULL);
  free(s.ring); free(input); free(decoder); free(samples);
  pthread_cond_destroy(&s.condition); pthread_mutex_destroy(&s.mutex);
  return ret;
}

static void *player_worker(void *unused)
{
  (void)unused; pthread_setname_np(pthread_self(), "music_player");
  for (;;) {
    pthread_mutex_lock(&lock);
    while (!want_play) pthread_cond_wait(&wake, &lock);
    unsigned generation = play_generation;
    struct music_song song = pending_song;
    uint64_t resolved_until = pending_result_deadline;
    want_play = false;
    player_active = true;
    pthread_mutex_unlock(&lock);
    bool local=!strncmp(song.url,MUSIC_SD_ROOT,sizeof(MUSIC_SD_ROOT)-1);
    int ret = resolved_until ? (music_now_ms() < resolved_until ? 0 : -ESTALE) :
      local ? (music_sd_path(song.url) ? 0 : -EINVAL) :
      lookup(song.query[0] ? song.query : song.name, &song, generation, false);
    pthread_mutex_lock(&lock);
    bool current = generation == play_generation;
    if (current && !ret) {
      state.current = song; state.revision++;
      pending_lyrics=song;lyrics_generation=generation;want_lyrics=true;
      lyrics_state=MUSIC_LYRICS_LOADING;pthread_cond_broadcast(&wake);
    }
    if (!ret && current) {
      while (generation == play_generation) {
        unsigned seek = seek_revision, target = seek_position_ms;
        pthread_mutex_unlock(&lock);
        ret = play_stream(&song, generation, seek, target);
        pthread_mutex_lock(&lock);
        if (seek == seek_revision) break;
      }
    }
    /* Completion and the last seek check share the command lock, so a seek
     * accepted at EOF cannot be overwritten by an older completion. */
    if (generation == play_generation) {
      state.state = ret ? MUSIC_ERROR : MUSIC_ENDED; state.error = ret;
      if (!ret) state.duration_ms = state.position_ms;
      state.revision++;
    }
    player_active = false;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    printf("[music] playback finished ret=%d\n", ret);
  }
  return NULL;
}

void glass_music_start(void)
{
  pthread_mutex_lock(&lock);
  pthread_t thread;int ret=0;
  if(!search_started) {
    ret=spawn(&thread,search_worker,NULL,32768);
    if(!ret){search_started=true;pthread_detach(thread);}
  }
  if(!ret && !player_started) {
    ret=spawn(&thread,player_worker,NULL,49152);
    if(!ret){player_started=true;pthread_detach(thread);}
  }
  if(ret){state.error=ret;state.state=MUSIC_ERROR;state.revision++;}
  pthread_mutex_unlock(&lock);
}

void glass_music_now_playing(struct music_now_playing *out)
{
  if(!out)return;
  pthread_mutex_lock(&lock);
  memset(out,0,sizeof(*out));
  memcpy(out->name,state.current.name,sizeof(out->name));
  memcpy(out->artist,state.current.artist,sizeof(out->artist));
  memcpy(out->album,state.current.album,sizeof(out->album));
  memcpy(out->cover_url,current_cover,sizeof(out->cover_url));out->track_id=play_generation;
  out->revision=state.revision;out->state=state.state;
  out->position_ms=state.position_ms;out->duration_ms=state.duration_ms;
  out->volume=glass_speaker_volume();out->pause_requested=want_pause;
  out->favorite=glass_music_is_favorite(&state,&state.current);
  out->can_skip=state.favorite_count>0;out->lyrics_state=lyrics_state;
  int index=music_lrc_index(lyrics,state.position_ms);
  out->lyrics_started=index>=0;
  for(int i=0;lyrics && i<3;i++) {
    int n=index+i-1;
    if(n>=0 && (unsigned)n<lyrics->count){
      memcpy(out->lyrics[i],lyrics->lines[n].text,MUSIC_LYRIC_TEXT_MAX);
      memcpy(out->translations[i],lyrics->lines[n].translation,MUSIC_LYRIC_TEXT_MAX);
    }
  }
  pthread_mutex_unlock(&lock);
}

void glass_music_lyrics_read(unsigned first, struct music_lyrics_window *out)
{
  if(!out)return;
  pthread_mutex_lock(&lock);
  memset(out,0,sizeof(*out));out->track_id=play_generation;
  out->state=lyrics_state;out->current=music_lrc_index(lyrics,state.position_ms);
  out->total=lyrics?lyrics->count:0;
  unsigned last=out->total>MUSIC_LYRICS_WINDOW?out->total-MUSIC_LYRICS_WINDOW:0;
  out->first=first>last?last:first;
  for(unsigned i=out->first;i<out->total&&out->count<MUSIC_LYRICS_WINDOW;i++){
    unsigned slot=out->count++;
    out->start_ms[slot]=lyrics->lines[i].ms;
    /* A zero end denotes unknown duration, never an invented cut boundary. */
    out->end_ms[slot]=i+1<out->total?lyrics->lines[i+1].ms:state.duration_ms;
    memcpy(out->lines[slot],lyrics->lines[i].text,MUSIC_LYRIC_TEXT_MAX);
    memcpy(out->translations[slot],lyrics->lines[i].translation,MUSIC_LYRIC_TEXT_MAX);
  }
  pthread_mutex_unlock(&lock);
}

int glass_music_skip(int direction)
{
  if(direction!=-1 && direction!=1)return -EINVAL;
  pthread_mutex_lock(&lock);
  if(!state.favorite_count){pthread_mutex_unlock(&lock);return -ENOENT;}
  int index=-1;
  for(unsigned i=0;i<state.favorite_count;i++)
    if(!strcmp(state.current.name,state.favorites[i].name) && !strcmp(state.current.artist,state.favorites[i].artist)){index=i;break;}
  index=index<0?0:(index+direction+(int)state.favorite_count)%state.favorite_count;
  struct music_song song=state.favorites[index];
  pthread_mutex_unlock(&lock);
  return glass_music_play(&song);
}

void glass_music_get(struct music_snapshot *out)
{ pthread_mutex_lock(&lock); *out = state; out->volume=glass_speaker_volume(); pthread_mutex_unlock(&lock); }

int glass_music_search(const char *query)
{
  if (!query || !query[0] || strlen(query) >= sizeof(pending_query)) return -EINVAL;
  pthread_mutex_lock(&lock);
  strcpy(pending_query, query); search_generation++; want_search = true;
  state.searching = true; state.search_error = 0; state.has_result = false; state.revision++;
  pthread_cond_broadcast(&wake); pthread_mutex_unlock(&lock); return 0;
}

/* Caller holds lock. A deadline marks a trusted, freshly resolved result. */
static void music_queue_locked(const struct music_song *song, uint64_t deadline)
{
  clear_lyrics_locked();
  pending_result_deadline = deadline;
  pending_song = *song; state.current = *song; play_generation++; want_play = true; want_pause = false;
  seek_position_ms = 0; seek_revision++;
  state.state = MUSIC_LOADING; state.position_ms = 0; state.duration_ms = 0; state.error = 0; state.revision++;
  pthread_cond_broadcast(&wake);
}

int glass_music_play(const struct music_song *song)
{
  if (!song || !song->name[0]) return -EINVAL;
  pthread_mutex_lock(&lock);
  music_queue_locked(song, 0);
  pthread_mutex_unlock(&lock); return 0;
}

int glass_music_play_result(const char *query, unsigned result_id, unsigned *track_id)
{
  if (!query || !query[0] || !result_id) return -EINVAL;
  pthread_mutex_lock(&lock);
  if (consumed_result == result_id) {
    pthread_mutex_unlock(&lock); return -EALREADY;
  }
  if (state.searching || !state.has_result || state.search_error ||
      state.search_revision != result_id ||
      strcmp(state.result.query, query) || music_now_ms() >= result_deadline) {
    pthread_mutex_unlock(&lock); return -ESTALE;
  }
  music_queue_locked(&state.result, result_deadline);
  consumed_result = result_id;
  if (track_id) *track_id = play_generation;
  pthread_mutex_unlock(&lock); return 0;
}

void glass_music_toggle(void)
{
  pthread_mutex_lock(&lock);
  if (state.state == MUSIC_PLAYING || state.state == MUSIC_PAUSED || state.state == MUSIC_LOADING) {
    want_pause = !want_pause;
    if (state.state != MUSIC_LOADING) state.state = want_pause ? MUSIC_PAUSED : MUSIC_PLAYING;
    state.revision++;
  } else if (state.current.name[0]) {
    clear_lyrics_locked();
    pending_song = state.current; play_generation++; want_play = true; want_pause = false;
    seek_position_ms = 0; seek_revision++;
    pending_result_deadline = 0;
    state.state = MUSIC_LOADING; state.error = 0; state.position_ms = 0; state.duration_ms = 0; state.revision++;
    pthread_cond_broadcast(&wake);
  }
  pthread_mutex_unlock(&lock);
}

int glass_music_set_paused(bool paused)
{
  pthread_mutex_lock(&lock);
  if (state.state != MUSIC_PLAYING && state.state != MUSIC_PAUSED && state.state != MUSIC_LOADING) {
    pthread_mutex_unlock(&lock); return -ENOENT;
  }
  want_pause = paused;
  if (state.state != MUSIC_LOADING) state.state = paused ? MUSIC_PAUSED : MUSIC_PLAYING;
  state.revision++;
  pthread_cond_broadcast(&wake);
  pthread_mutex_unlock(&lock);
  return 0;
}

int glass_music_seek(unsigned track_id, unsigned position_ms)
{
  pthread_mutex_lock(&lock);
  if (track_id != play_generation) {
    pthread_mutex_unlock(&lock); return -ESTALE;
  }
  if (!state.duration_ms || (state.state != MUSIC_PLAYING &&
      state.state != MUSIC_PAUSED && state.state != MUSIC_LOADING)) {
    pthread_mutex_unlock(&lock); return -EAGAIN;
  }
  if (position_ms >= state.duration_ms) position_ms = state.duration_ms - 1;
  seek_position_ms = position_ms; seek_revision++;
  state.position_ms = position_ms;
  state.state = want_pause ? MUSIC_PAUSED : MUSIC_LOADING;
  state.revision++;
  pthread_cond_broadcast(&wake);
  pthread_mutex_unlock(&lock);
  return 0;
}

void glass_music_stop(void)
{ pthread_mutex_lock(&lock); clear_lyrics_locked(); play_generation++; want_play = false; want_pause = false; state.state = MUSIC_STOPPED; state.revision++; pthread_cond_broadcast(&wake); pthread_mutex_unlock(&lock); }

int glass_music_stop_wait(unsigned timeout_ms)
{
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&lock);
  clear_lyrics_locked();
  play_generation++;
  want_play = false;
  want_pause = false;
  state.state = MUSIC_STOPPED;
  state.revision++;
  pthread_cond_broadcast(&wake);

  int ret = 0;
  while (player_active && !ret) ret = pthread_cond_timedwait(&wake, &lock, &deadline);
  pthread_mutex_unlock(&lock);
  return ret == ETIMEDOUT ? -ETIMEDOUT : ret ? -ret : 0;
}

void glass_music_volume(unsigned volume)
{ pthread_mutex_lock(&lock); glass_speaker_volume_set(volume > 100 ? 100 : volume); state.revision++; pthread_mutex_unlock(&lock); }

int glass_music_favorite(const struct music_song *song)
{
  if (!song || !song->name[0]) return -EINVAL;
  pthread_mutex_lock(&lock);
  if (want_favorite) { pthread_mutex_unlock(&lock); return -EBUSY; }
  pending_favorite = *song; want_favorite = true;
  pthread_cond_broadcast(&wake); pthread_mutex_unlock(&lock); return 0;
}
