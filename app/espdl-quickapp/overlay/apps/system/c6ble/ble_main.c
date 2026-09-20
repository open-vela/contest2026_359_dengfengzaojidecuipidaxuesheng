/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glass_ble.h"
#include "ble_register.h"

static int usage(void)
{
  fprintf(stderr, "Usage:\n"
    "  c6ble register\n"
    "  c6ble mouse start|status|release|stop\n"
    "  c6ble mouse move <x> <y> [wheel] [buttons]\n"
    "  c6ble mouse click left|right|middle\n"
    "  c6ble mouse scroll <steps>\n"
    "Arguments are decimal; x/y/wheel: -127..127, buttons: 0..7.\n"
    "Start opens a 120-second pairing window; it sends no input.\n");
  return 2;
}

static int number(const char *text, int minimum, int maximum, int *value)
{
  char *end;
  long parsed;
  if (!text || !*text) return -EINVAL;
  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno || end == text || *end || parsed < minimum || parsed > maximum)
    return -EINVAL;
  *value = (int)parsed;
  return 0;
}

static int result(const char *command, int ret)
{
  if (ret)
    {
      fprintf(stderr, "c6ble mouse %s: %d (%s)\n", command, ret,
              ret < 0 ? strerror(-ret) : "backend error");
      return 1;
    }
  printf("c6ble mouse %s: ok\n", command);
  return 0;
}

int main(int argc, char **argv)
{
  int x, y, wheel = 0, buttons = 0;
  const char *cmd;
  if (argc == 2 && !strcmp(argv[1], "register"))
    return result("register", c6_ble_register("/dev/ttyHCI0"));
  if (argc < 3 || (strcmp(argv[1], "mouse") && strcmp(argv[1], "hid")))
    return usage();
  cmd = argv[2];
  if (argc == 3)
    {
      if (!strcmp(cmd, "start")) return result(cmd, glass_ble_hid_start());
      if (!strcmp(cmd, "stop")) return result(cmd, glass_ble_hid_stop());
      if (!strcmp(cmd, "release")) return result(cmd, glass_ble_hid_release());
      if (!strcmp(cmd, "status"))
        {
          struct glass_ble_hid_state s;
          glass_ble_hid_read(&s);
          printf("host_ready=%d enabled=%d advertising=%d connected=%d "
                 "encrypted=%d notify=%d suspended=%d boot=%d buttons=%u "
                 "epoch=%lu error=%d ble_error=%d\n",
                 s.host_ready, s.enabled, s.advertising, s.connected,
                 s.encrypted, s.mouse_notify, s.suspended, s.boot_mode,
                 (unsigned)s.buttons, (unsigned long)s.epoch, s.error, s.ble_error);
          return 0;
        }
    }
  if (!strcmp(cmd, "click") && argc == 4)
    {
      if (!strcmp(argv[3], "left")) buttons = 1;
      else if (!strcmp(argv[3], "right")) buttons = 2;
      else if (!strcmp(argv[3], "middle")) buttons = 4;
      else return usage();
      return result(cmd, glass_ble_hid_click((unsigned)buttons));
    }
  if (!strcmp(cmd, "scroll") && argc == 4)
    {
      if (number(argv[3], -127, 127, &wheel)) return usage();
      return result(cmd, glass_ble_hid_mouse(0, 0, 0, wheel));
    }
  if (!strcmp(cmd, "move") && argc >= 5 && argc <= 7)
    {
      if (number(argv[3], -127, 127, &x) || number(argv[4], -127, 127, &y) ||
          (argc >= 6 && number(argv[5], -127, 127, &wheel)) ||
          (argc == 7 && number(argv[6], 0, 7, &buttons))) return usage();
      return result(cmd, glass_ble_hid_mouse(buttons, x, y, wheel));
    }
  return usage();
}
