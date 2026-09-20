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
  s->enabled = false;
  s->disconnect_required = s->connected;
  s->error = error;
  return error;
}

int mouse_core_release(struct mouse_core *s, mouse_send_fn send, void *arg)
{
  uint8_t report[4] = {0};
  int ret = 0;
  if (s->buttons == 0) return 0;
  if (s->connected && s->encrypted &&
      (s->boot ? s->boot_notify : s->report_notify))
    ret = send(arg, report, s->boot ? 3 : 4, s->boot);
  else if (s->connected)
    ret = -EACCES;
  s->buttons = 0;
  if (ret) return failed(s, ret);
  return 0;
}

int mouse_core_move(struct mouse_core *s, uint32_t epoch, int buttons,
                    int x, int y, int wheel, uint64_t now,
                    mouse_send_fn send, void *arg)
{
  uint8_t report[4];
  int ret;
  if (buttons < 0 || buttons > 7 || x < -127 || x > 127 ||
      y < -127 || y > 127 || wheel < -127 || wheel > 127)
    return -EINVAL;
  if (epoch != s->epoch) return -ESTALE;
  if (!s->enabled) return -EACCES;
  if (!s->connected) return -ENOTCONN;
  if (!s->encrypted) return -EACCES;
  if (s->suspended) return -EAGAIN;
  if (!(s->boot ? s->boot_notify : s->report_notify)) return -EAGAIN;
  if (s->boot && wheel) return -ENOTSUP;
  report[0] = (uint8_t)buttons;
  report[1] = (uint8_t)x;
  report[2] = (uint8_t)y;
  report[3] = (uint8_t)wheel;
  ret = send(arg, report, s->boot ? 3 : 4, s->boot);
  if (ret)
    {
      /* The previous report may have held buttons. Try a neutral report,
       * then fail closed and request disconnect even if neutral succeeds. */
      (void)mouse_core_release(s, send, arg);
      return failed(s, ret);
    }
  s->buttons = (uint8_t)buttons;
  s->held_at = now;
  s->error = 0;
  return 0;
}

int mouse_core_stop(struct mouse_core *s, mouse_send_fn send, void *arg)
{
  int ret;
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
  ret = mouse_core_release(s, send, arg); /* Release on OLD protocol. */
  if (!ret) s->boot = mode == 0;
  return ret;
}

int mouse_core_suspend(struct mouse_core *s, unsigned value,
                       mouse_send_fn send, void *arg)
{
  int ret = 0;
  if (value > 1) return -EINVAL;
  if (!value) ret = mouse_core_release(s, send, arg);
  s->suspended = value == 0;
  return ret;
}

int mouse_core_tick(struct mouse_core *s, uint64_t now,
                    mouse_send_fn send, void *arg)
{
  if (s->buttons && now - s->held_at >= MOUSE_HOLD_MS)
    return mouse_core_release(s, send, arg);
  return 0;
}
