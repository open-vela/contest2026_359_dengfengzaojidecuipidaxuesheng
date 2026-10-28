/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "glass_music_pcm.h"
#include "glass_device_controls.h"
#ifdef GLASS_LOCAL_SPEECH
#include "glass_speech.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
extern void board_music_amplifier(bool enabled);

/* The board has one ES8311 codec and one shared I2S DMA engine. Keep a
 * session exclusively owned until STOP, buffer release, and MQ teardown have
 * all completed; RESERVE alone cannot serialize the surrounding teardown. */
static pthread_mutex_t g_pcm_session_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_pcm_session_active;
static atomic_uint g_pcm_foreground_waiters;
/* Keep the established music startup level after unifying the controls. */
static atomic_uint g_speaker_volume = 40;
static atomic_uint g_pcm_block_frames;
static atomic_uint g_pcm_rate, g_pcm_codec_volume, g_pcm_applied_master;
static atomic_uint g_pcm_input_peak, g_pcm_output_peak, g_pcm_written_frames;
static atomic_uint g_pcm_empty, g_pcm_last_submit_us, g_pcm_max_submit_gap_us;

void music_pcm_status(void)
{
  printf("AUDIO_PCM rate=%u block_frames=%u codec=%u master=%u applied=%u input_peak=%u output_peak=%u frames=%u empty=%u max_submit_gap_us=%u\n",
    atomic_load(&g_pcm_rate),atomic_load(&g_pcm_block_frames),atomic_load(&g_pcm_codec_volume),atomic_load(&g_speaker_volume),
    atomic_load(&g_pcm_applied_master),atomic_load(&g_pcm_input_peak),atomic_load(&g_pcm_output_peak),
    atomic_load(&g_pcm_written_frames),atomic_load(&g_pcm_empty),atomic_load(&g_pcm_max_submit_gap_us));
}

unsigned glass_speaker_volume(void)
{
  return atomic_load(&g_speaker_volume);
}

int glass_speaker_volume_set(unsigned percent)
{
  if (percent > 100) return -EINVAL;
  atomic_store(&g_speaker_volume, percent);
  return 0;
}

int music_pcm_session_lock(void)
{
  int ret;
#ifdef GLASS_LOCAL_SPEECH
  glass_speech_stop();
#endif
  do
    {
      ret = pthread_mutex_lock(&g_pcm_session_lock);
    }
  while (ret == EINTR);

  if (ret == 0)
    {
      atomic_store(&g_pcm_session_active, true);
    }

  return ret ? -ret : 0;
}

int music_pcm_session_trylock(void)
{
  int ret=pthread_mutex_trylock(&g_pcm_session_lock);
  if(!ret)atomic_store(&g_pcm_session_active,true);
  return ret?-ret:0;
}

void music_pcm_session_unlock(void)
{
  atomic_store(&g_pcm_session_active, false);
  pthread_mutex_unlock(&g_pcm_session_lock);
}

bool music_pcm_session_active(void)
{
  return atomic_load(&g_pcm_session_active) || music_pcm_foreground_pending();
}

bool music_pcm_foreground_pending(void)
{
  return atomic_load(&g_pcm_foreground_waiters) != 0;
}

static int pcm_message(struct music_pcm *pcm)
{
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 2;
  struct audio_msg_s msg;
  ssize_t n;
  do { n = mq_timedreceive(pcm->mq, (char *)&msg, sizeof(msg), NULL, &deadline); }
  while (n < 0 && errno == EINTR);
  if (n < 0) return -errno;
  if (msg.msg_id == AUDIO_MSG_DEQUEUE) {
    for (unsigned i = 0; i < MUSIC_PCM_BUFFERS; i++)
      if (pcm->buffers[i] == msg.u.ptr && pcm->queued[i]) {
        pcm->queued[i] = false;
        pcm->pending--;
        pcm->played_samples += pcm->submitted_samples[i];
        return 0;
      }
    return -EIO;
  }
  if (msg.msg_id == AUDIO_MSG_IOERR || msg.msg_id == AUDIO_MSG_COMPLETE) {
    return -EIO;
  }
  return 0;
}

int music_pcm_volume(struct music_pcm *pcm, unsigned volume)
{
  if (pcm->fd < 0) return -ENODEV;
  /* ES8311 DAC 0xbf is unity. The lower half's 29*ln(volume*10)
   * curve reaches it at 75%; 100% requests +4.5 dB and clips loud PCM.
   * The system master supplies attenuation without codec overdrive. */
  unsigned hardware=volume>75?75:volume;
  struct audio_caps_desc_s caps = {0};
  caps.caps.ac_len = sizeof(caps.caps);
  caps.caps.ac_type = AUDIO_TYPE_FEATURE;
  caps.caps.ac_format.hw = AUDIO_FU_VOLUME;
  caps.caps.ac_controls.hw[0] = hardware * 10;
  if (ioctl(pcm->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps) < 0) return -errno;
  atomic_store(&g_pcm_codec_volume, hardware);
  return 0;
}

static int pcm_open(struct music_pcm *pcm, unsigned rate, unsigned volume, bool nonblocking)
{
  memset(pcm, 0, sizeof(*pcm)); pcm->fd = -1; pcm->mq = (mqd_t)-1;
  if (rate < 8000 || rate > 48000) return -EINVAL;
  int ret;
  if (nonblocking) {
    ret = pthread_mutex_trylock(&g_pcm_session_lock);
    if (!ret) atomic_store(&g_pcm_session_active, true);
    else ret = -ret;
  } else ret = music_pcm_session_lock();
  if (ret) return ret;
  pcm->session_locked = true;
  pcm->fd = open("/dev/audio/pcm0", O_RDWR);
  /* The board exposes its single ADC/DAC lower half under pcm_in0.
   * Reuse that upper half so recording and playback share RESERVE ownership. */
  if (pcm->fd < 0 && errno == ENOENT) pcm->fd = open("/dev/audio/pcm_in0", O_RDWR);
  if (pcm->fd < 0) { ret = -errno; goto fail; }
  ret = -EIO;
  if (ioctl(pcm->fd, AUDIOIOC_RESERVE, 0) < 0) { ret = -errno; goto fail; }
  pcm->reserved = true;
  struct audio_caps_desc_s caps = {0};
  caps.caps.ac_len = sizeof(caps.caps);
  caps.caps.ac_type = AUDIO_TYPE_OUTPUT;
  caps.caps.ac_channels = 1;
  caps.caps.ac_controls.hw[0] = rate;
  caps.caps.ac_controls.b[2] = 16;
  caps.caps.ac_subtype = AUDIO_FMT_PCM;
  if (ioctl(pcm->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps) < 0) { ret = -errno; goto fail; }
  struct mq_attr attr = {.mq_maxmsg = 12, .mq_msgsize = sizeof(struct audio_msg_s)};
  mq_unlink("/musicpcm");
  pcm->mq = mq_open("/musicpcm", O_CREAT | O_RDWR, 0600, &attr);
  if (pcm->mq == (mqd_t)-1) { ret = -errno; goto fail; }
  if (ioctl(pcm->fd, AUDIOIOC_REGISTERMQ, (unsigned long)pcm->mq) < 0) { ret = -errno; goto fail; }
  /* The upper half learns its allocation limit through GETBUFFERINFO. */
  struct ap_buffer_info_s info = {0};
  if (ioctl(pcm->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&info) < 0) { ret = -errno; goto fail; }
  if (info.nbuffers < MUSIC_PCM_BUFFERS) { ret = -ENOBUFS; goto fail; }
  pcm->rate = rate;
  for (unsigned i = 0; i < MUSIC_PCM_BUFFERS; i++) {
    struct audio_buf_desc_s desc = {.numbytes = MUSIC_PCM_BLOCK_FRAMES(rate) * 4, .u.pbuffer = &pcm->buffers[i]};
    int allocated = ioctl(pcm->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&desc);
    if (allocated != sizeof(desc) || !pcm->buffers[i]) { ret = allocated < 0 ? -errno : -ENOMEM; goto fail; }
  }
  pcm->rate = rate;
  ret = music_pcm_volume(pcm, volume);
  if (ret) goto fail;
  atomic_store(&g_pcm_rate,rate);
  atomic_store(&g_pcm_block_frames,MUSIC_PCM_BLOCK_FRAMES(rate));
  atomic_store(&g_pcm_written_frames,0);
  atomic_store(&g_pcm_empty,0);
  atomic_store(&g_pcm_last_submit_us,0);
  atomic_store(&g_pcm_max_submit_gap_us,0);
  printf("[music] PCM opened rate=%u mono volume=%u\n", rate, volume);
  return 0;
fail:
  music_pcm_close(pcm);
  return ret;
}

int music_pcm_open(struct music_pcm *pcm, unsigned rate, unsigned volume)
{ return pcm_open(pcm, rate, volume, false); }
int music_pcm_try_open(struct music_pcm *pcm, unsigned rate, unsigned volume)
{ return pcm_open(pcm, rate, volume, true); }

int music_pcm_foreground_open(struct music_pcm *pcm, unsigned rate,
                             unsigned volume, unsigned timeout_ms,
                             bool (*cancelled)(void *), void *opaque)
{
  if (!pcm || rate < 8000 || rate > 48000 || timeout_ms > 5000)
    return -EINVAL;
  if (cancelled && cancelled(opaque)) return -ECANCELED;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t started = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
  uint64_t deadline = started + timeout_ms;
  /* Publish intent before cancellation. The wake service must not reacquire
   * the session between capture teardown and the video worker taking it. */
  atomic_fetch_add(&g_pcm_foreground_waiters, 1);
  bool yielding = false;
#ifdef GLASS_LOCAL_SPEECH
  yielding = glass_speech_yield_background();
#endif
  int ret;
  for (;;) {
    if (cancelled && cancelled(opaque)) { ret = -ECANCELED; break; }
    ret = music_pcm_try_open(pcm, rate, volume);
    if (ret != -EBUSY || !yielding) break;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 >= deadline) {
      ret = -ETIMEDOUT;
      break;
    }
    usleep(10000);
  }
  /* Successful open publishes session_active before the pending intent goes
   * away. Failed/cancelled opens leave no suppression behind. */
  atomic_fetch_sub(&g_pcm_foreground_waiters, 1);
  clock_gettime(CLOCK_MONOTONIC, &now);
  printf("[audio-handoff] background=%u result=%d elapsed_ms=%llu\n",
         yielding, ret, (unsigned long long)((uint64_t)now.tv_sec * 1000 +
         now.tv_nsec / 1000000 - started));
  return ret;
}

static int pcm_submit(struct music_pcm *pcm, unsigned index, unsigned frames)
{
  struct ap_buffer_s *buffer = pcm->buffers[index];
  /* P4's current I2S lower half carries an unaligned cache-line tail into
   * a four-byte field. Submit complete 64-byte cache lines to avoid that
   * path. Only drain pads the final line, with silence. */
  unsigned padded = (frames + 15u) & ~15u;
  memset(buffer->samp + frames * 4, 0, (padded - frames) * 4);
  buffer->nbytes = padded * 4; buffer->curbyte = 0; buffer->flags = 0;
  struct audio_buf_desc_s desc = {.numbytes = buffer->nbytes, .u.buffer = buffer};
  pcm->submitted_samples[index] = frames;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC,&now);
  unsigned us=(unsigned)((uint64_t)now.tv_sec*1000000u+now.tv_nsec/1000);
  unsigned previous=atomic_exchange(&g_pcm_last_submit_us,us);
  if(previous&&us-previous>atomic_load(&g_pcm_max_submit_gap_us))
    atomic_store(&g_pcm_max_submit_gap_us,us-previous);
  if(pcm->started&&!pcm->pending)atomic_fetch_add(&g_pcm_empty,1);
  if (ioctl(pcm->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc) < 0) return -errno;
  pcm->queued[index] = true; pcm->pending++;
  pcm->staged_frames = 0;
  if (!pcm->started && pcm->pending >= 2) {
    if (ioctl(pcm->fd, AUDIOIOC_START, 0) < 0) return -errno;
    pcm->started = true;
    board_music_amplifier(true);
  }
  return 0;
}

int music_pcm_write(struct music_pcm *pcm, const int16_t *samples, unsigned frames, unsigned channels)
{
  if (!frames || (channels != 1 && channels != 2)) return -EINVAL;
  while (frames) {
    unsigned index = pcm->staged_index;
    while (!pcm->staged_frames) {
      for (index = 0; index < MUSIC_PCM_BUFFERS; index++) if (!pcm->queued[index]) break;
      if (index < MUSIC_PCM_BUFFERS) break;
      int ret = pcm_message(pcm);
      if (ret) return ret;
    }
    unsigned capacity = MUSIC_PCM_BLOCK_FRAMES(pcm->rate);
    unsigned available = capacity - pcm->staged_frames;
    unsigned chunk = frames > available ? available : frames;
    pcm->staged_index = index;
    struct ap_buffer_s *buffer = pcm->buffers[index];
    int16_t *out = (int16_t *)buffer->samp + pcm->staged_frames * 2;
    unsigned master = glass_speaker_volume();
    unsigned input_peak=0,output_peak=0;
    /* The I2S peripheral transfers two 16-bit slots per frame. Writing one
     * mono sample per frame let the DAC consume two samples per clock, which
     * played everything at double speed with aliasing. Duplicate the sample
     * into both slots so frame timing, pitch and duration stay correct. */
    for (unsigned i = 0; i < chunk; i++) {
      int16_t value = channels == 1 ? samples[i] :
        (int16_t)(((int32_t)samples[i*2] + samples[i*2+1]) / 2);
      unsigned peak=value<0?-(int32_t)value:value;
      if(peak>input_peak)input_peak=peak;
      value = (int16_t)((int32_t)value * (int32_t)master / 100);
      peak=value<0?-(int32_t)value:value;
      if(peak>output_peak)output_peak=peak;
      out[i*2] = value; out[i*2+1] = value;
    }
    atomic_store(&g_pcm_input_peak,input_peak);
    atomic_store(&g_pcm_output_peak,output_peak);
    atomic_store(&g_pcm_applied_master,master);
    atomic_fetch_add(&g_pcm_written_frames,chunk);
    pcm->staged_frames += chunk;
    if (pcm->staged_frames == capacity) {
      int ret = pcm_submit(pcm, index, pcm->staged_frames);
      if (ret) return ret;
    }
    samples += chunk * channels; frames -= chunk;
  }
  return 0;
}

int music_pcm_pause(struct music_pcm *pcm, bool paused)
{
  if (!pcm->started) return 0;
  return ioctl(pcm->fd, paused ? AUDIOIOC_PAUSE : AUDIOIOC_RESUME, 0) < 0 ? -errno : 0;
}

int music_pcm_drain(struct music_pcm *pcm)
{
  if (pcm->staged_frames) {
    int ret = pcm_submit(pcm, pcm->staged_index, pcm->staged_frames);
    if (ret) return ret;
  }
  if (pcm->pending && !pcm->started) {
    if (ioctl(pcm->fd, AUDIOIOC_START, 0) < 0) return -errno;
    pcm->started = true;
    board_music_amplifier(true);
  }
  while (pcm->pending) { int ret = pcm_message(pcm); if (ret) return ret; }
  return 0;
}

void music_pcm_close(struct music_pcm *pcm)
{
  bool session_locked = pcm->session_locked;
  if (pcm->fd >= 0) {
    if (pcm->reserved) board_music_amplifier(false);
    /* A failure or cancellation may leave one primed buffer below the normal
     * two-buffer start threshold. Start it with the amplifier muted so STOP
     * can synchronously return that buffer through the lower-half driver.
     */
    if (pcm->reserved && pcm->pending && !pcm->started &&
        ioctl(pcm->fd, AUDIOIOC_START, 0) == 0) pcm->started = true;
    /* STOP joins the lower-half worker before buffers can be reclaimed. */
    if (pcm->started) {
      ioctl(pcm->fd, AUDIOIOC_STOP, 0);
    }
    for (unsigned i = 0; i < MUSIC_PCM_BUFFERS; i++) if (pcm->buffers[i]) {
      struct audio_buf_desc_s desc = {.u.buffer = pcm->buffers[i]};
      ioctl(pcm->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&desc);
    }
    if (pcm->mq != (mqd_t)-1) ioctl(pcm->fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)pcm->mq);
    if (pcm->reserved) ioctl(pcm->fd, AUDIOIOC_RELEASE, 0);
    close(pcm->fd);
  }
  if (pcm->mq != (mqd_t)-1) { mq_close(pcm->mq); mq_unlink("/musicpcm"); }
  memset(pcm, 0, sizeof(*pcm)); pcm->fd = -1; pcm->mq = (mqd_t)-1;
  if (session_locked) music_pcm_session_unlock();
}
