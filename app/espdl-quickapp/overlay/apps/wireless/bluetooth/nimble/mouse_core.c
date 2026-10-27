/* SPDX-License-Identifier: Apache-2.0 */
#include "mouse_core.h"
#include <errno.h>
#include <string.h>

/* Three buttons, signed 8-bit relative X/Y/wheel; GATT Report Reference
 * carries the ID, so the characteristic payload itself has no ID byte. */
const uint8_t mouse_report_map[] = {
  0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x85,MOUSE_REPORT_ID,
  0x09,0x01, 0xa1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x03,
  0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
  0x95,0x01, 0x75,0x05, 0x81,0x01, 0x05,0x01, 0x09,0x30,
  0x09,0x31, 0x09,0x38, 0x15,0x81, 0x25,0x7f, 0x75,0x08,
  0x95,0x03, 0x81,0x06, 0xc0,0xc0
};
const size_t mouse_report_map_size = sizeof(mouse_report_map);

void mouse_core_connect(struct mouse_core *s)
{
  bool enabled = s->enabled;
  uint32_t epoch = s->epoch + 1;
  memset(s, 0, sizeof(*s));
  s->enabled = enabled;
  s->connected = true;
  s->epoch = epoch;
}

void mouse_core_disconnect(struct mouse_core *s)
{
  uint32_t epoch = s->epoch + 1;
  memset(s, 0, sizeof(*s));
  s->epoch = epoch; /* Never replay an old request on a new connection. */
}

static int failed(struct mouse_core *s, int error)
{
  s->discarded += s->queued;
  s->head = s->queued = 0;
  s->release_pending = false;
  s->cancelled++;
  s->enabled = false;
  s->disconnect_required = s->connected;
  s->error = error;
  return error;
}

bool mouse_core_transient(int error)
{
  return error == -EAGAIN || error == -ENOMEM || error == -EBUSY;
}

static int ready(struct mouse_core *s, uint32_t epoch)
{
  if (epoch != s->epoch) return -ESTALE;
  if (!s->enabled) return -EACCES;
  if (!s->connected) return -ENOTCONN;
  if (!s->encrypted) return -EACCES;
  if (s->suspended || s->release_pending) return -EAGAIN;
  if (!(s->boot ? s->boot_notify : s->report_notify)) return -EAGAIN;
  return 0;
}

static int report(struct mouse_core *s, int buttons, int x, int y, int wheel,
                  uint64_t now, mouse_send_fn send, void *arg)
{
  uint8_t data[4] = {(uint8_t)buttons, (uint8_t)x, (uint8_t)y, (uint8_t)wheel};
  int ret = send(arg, data, s->boot ? 3 : 4, s->boot);
  s->retry_at = now + MOUSE_REPORT_MS;
  if (mouse_core_transient(ret))
    {
      s->retries++;
      s->error = ret;
      return ret; /* Stack did not accept this report. Preserve button state. */
    }
  if (ret) return failed(s, ret); /* Ambiguous/fatal failures are never replayed. */
  if (buttons && !s->buttons) s->held_at = now;
  s->buttons = (uint8_t)buttons;
  s->submitted++;
  s->error = 0;
  return 0;
}

void mouse_core_cancel_input(struct mouse_core *s, uint64_t now)
{
  s->discarded += s->queued;
  s->head = s->queued = 0;
  s->cancelled++;
  if (s->buttons && !s->release_pending)
    {
      s->release_pending = true;
      s->release_until = now + MOUSE_RELEASE_MS;
      s->retry_at = now;
    }
}

int mouse_core_release(struct mouse_core *s, mouse_send_fn send, void *arg)
{
  int ret = 0;
  bool sent = false;
  if (s->buttons == 0) {s->release_pending = false; return 0;}
  if (s->connected && s->encrypted &&
      (s->boot ? s->boot_notify : s->report_notify))
    {
      uint8_t data[4] = {0};
      ret = send(arg, data, s->boot ? 3 : 4, s->boot);
      sent = !ret;
    }
  else if (s->connected)
    ret = -EACCES;
  if (mouse_core_transient(ret)) {s->retries++;s->error=ret;return ret;}
  if (ret) return failed(s, ret);
  s->buttons = 0;
  s->release_pending = false;
  if(sent)s->submitted++;
  s->error = 0;
  return 0;
}

int mouse_core_move(struct mouse_core *s, uint32_t epoch, int buttons,
                    int x, int y, int wheel, uint64_t now,
                    mouse_send_fn send, void *arg)
{
  int ret;
  if (buttons < 0 || buttons > 7 || x < -127 || x > 127 ||
      y < -127 || y > 127 || wheel < -127 || wheel > 127)
    return -EINVAL;
  ret = ready(s, epoch);
  if (ret) return ret;
  if (s->queued) return -EBUSY;
  if (s->boot && wheel) return -ENOTSUP;
  ret = report(s, buttons, x, y, wheel, now, send, arg);
  if (ret) return ret;
  s->held_at = now;
  s->error = 0;
  return 0;
}

int mouse_core_stop(struct mouse_core *s, mouse_send_fn send, void *arg)
{
  int ret;
  s->discarded += s->queued;
  s->head = s->queued = 0;
  s->enabled = false;
  ret = mouse_core_release(s, send, arg);
  s->disconnect_required = s->connected;
  return ret;
}

int mouse_core_mode(struct mouse_core *s, unsigned mode,
                    mouse_send_fn send, void *arg)
{
  int ret;
  if (mode > 1) return -EINVAL;
  s->discarded += s->queued;
  s->head = s->queued = 0;
  ret = mouse_core_release(s, send, arg); /* Release on OLD protocol. */
  if (!ret) s->boot = mode == 0;
  return ret;
}

int mouse_core_suspend(struct mouse_core *s, unsigned value,
                       mouse_send_fn send, void *arg)
{
  int ret = 0;
  if (value > 1) return -EINVAL;
  if (!value) {s->discarded += s->queued;s->head=s->queued=0;}
  if (!value) ret = mouse_core_release(s, send, arg);
  s->suspended = value == 0;
  return ret;
}

int mouse_core_tick(struct mouse_core *s, uint64_t now,
                    mouse_send_fn send, void *arg)
{
  if (s->buttons && !s->release_pending && now - s->held_at >= MOUSE_HOLD_MS)
    mouse_core_cancel_input(s, now);
  if (s->release_pending)
    {
      if (now < s->retry_at) return 0;
      if (now >= s->release_until) return failed(s, -ETIMEDOUT);
      int ret = mouse_core_release(s, send, arg);
      s->retry_at = now + MOUSE_REPORT_MS;
      return ret;
    }
  if (!s->queued || now < s->retry_at) return 0;
  struct mouse_frame *frame = &s->frames[s->head];
  if (now - frame->created >= MOUSE_INPUT_MS)
    {
      mouse_core_cancel_input(s, now);
      s->error = -ETIMEDOUT;
      return -ETIMEDOUT;
    }
  int ret = ready(s, s->epoch);
  if (ret) {mouse_core_cancel_input(s, now);return ret;}
  int x = frame->x, y = frame->y, wheel = frame->wheel;
  if (x > 127) x=127; else if (x < -127) x=-127;
  if (y > 127) y=127; else if (y < -127) y=-127;
  if (wheel > 127) wheel=127; else if (wheel < -127) wheel=-127;
  ret = report(s, frame->buttons, x, y, wheel, now, send, arg);
  if (ret) return ret;
  frame->x -= x;frame->y -= y;frame->wheel -= wheel;
  if (!frame->x && !frame->y && !frame->wheel)
    {memset(frame,0,sizeof(*frame));s->head=(s->head+1)%MOUSE_QUEUE_LIMIT;s->queued--;}
  return 0;
}

int mouse_core_enqueue(struct mouse_core *s, uint32_t epoch, int buttons,
                       int x, int y, int wheel, uint64_t now)
{
  if (buttons<0 || buttons>7 || x < -MOUSE_DELTA_LIMIT || x > MOUSE_DELTA_LIMIT ||
      y < -MOUSE_DELTA_LIMIT || y > MOUSE_DELTA_LIMIT ||
      wheel < -MOUSE_DELTA_LIMIT || wheel > MOUSE_DELTA_LIMIT) return -EINVAL;
  int ret=ready(s,epoch);if(ret)return ret;
  if(s->boot && wheel)return -ENOTSUP;
  struct mouse_frame *tail=s->queued?&s->frames[(s->head+s->queued-1)%MOUSE_QUEUE_LIMIT]:NULL;
  unsigned desired=tail?tail->buttons:s->buttons;
  if(tail && !tail->edge && desired==(unsigned)buttons)
    {
      int nx=tail->x+x,ny=tail->y+y,nw=tail->wheel+wheel;
      if(nx < -MOUSE_DELTA_LIMIT || nx > MOUSE_DELTA_LIMIT ||
         ny < -MOUSE_DELTA_LIMIT || ny > MOUSE_DELTA_LIMIT ||
         nw < -MOUSE_DELTA_LIMIT || nw > MOUSE_DELTA_LIMIT)return -ENOSPC;
      tail->x=nx;tail->y=ny;tail->wheel=nw;s->merged++;
    }
  else
    {
      /* Reserve the last slot for a neutral release. Button edges never
       * merge or overtake previously accepted edges. */
      if(s->queued>=MOUSE_QUEUE_LIMIT ||
         (s->queued>=MOUSE_QUEUE_LIMIT-1 && buttons))return -ENOSPC;
      struct mouse_frame *f=&s->frames[(s->head+s->queued)%MOUSE_QUEUE_LIMIT];
      *f=(struct mouse_frame){.x=x,.y=y,.wheel=wheel,.created=now,
                              .buttons=buttons,.edge=desired!=(unsigned)buttons};
      s->queued++;
    }
  if(buttons)s->held_at=now;
  s->accepted++;
  return 0;
}

int mouse_core_enqueue_click(struct mouse_core *s, uint32_t epoch,
                             unsigned button, uint64_t now)
{
  if(button!=1 && button!=2 && button!=4)return -EINVAL;
  int ret=ready(s,epoch);if(ret)return ret;
  unsigned desired=s->queued?s->frames[(s->head+s->queued-1)%MOUSE_QUEUE_LIMIT].buttons:s->buttons;
  if(desired || s->queued>MOUSE_QUEUE_LIMIT-2)return -EBUSY;
  ret=mouse_core_enqueue(s,epoch,button,0,0,0,now);
  if(!ret)ret=mouse_core_enqueue(s,epoch,0,0,0,0,now);
  return ret;
}

int mouse_core_keepalive(struct mouse_core *s, uint32_t epoch, uint64_t now)
{
  int ret=ready(s,epoch);if(ret)return ret;
  s->held_at=now;
  return 0;
}
