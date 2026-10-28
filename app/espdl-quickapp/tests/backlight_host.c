/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <nuttx/timers/pwm.h>
#include "glass_backlight.h"

static unsigned opens,sets,starts,current_duty;
static int open_error,fail_set,fail_start;
int backlight_test_open(const char *path,int flags,...)
{
  assert(!strcmp(path,"/dev/pwm0")&&(flags&O_CLOEXEC));opens++;
  if(open_error){errno=ENOENT;return -1;}return 7;
}
int backlight_test_ioctl(int fd,unsigned long request,...)
{
  assert(fd==7);
  va_list ap;va_start(ap,request);
  if(request==PWMIOC_SETCHARACTERISTICS)
    {
      const struct pwm_info_s *info=(void *)va_arg(ap,unsigned long);
      assert(info->frequency==5000&&info->duty>=6553&&info->duty<=65535);
      sets++;if(fail_set){fail_set--;errno=EIO;va_end(ap);return -1;}
      current_duty=info->duty;
    }
  else
    {
      assert(request==PWMIOC_START);starts++;
      if(fail_start){fail_start--;errno=EIO;va_end(ap);return -1;}
    }
  va_end(ap);return 0;
}
int main(void)
{
  struct glass_backlight_status s;
#ifndef GLASS_PWM_BACKLIGHT
  assert(glass_backlight_set(50)==-ENOTSUP);glass_backlight_read(&s);
  assert(!s.available&&!s.applied&&s.error==-ENOTSUP&&!opens&&!sets&&!starts);
#else
  assert(glass_backlight_set(0)==-EINVAL&&glass_backlight_set(9)==-EINVAL&&glass_backlight_set(101)==-EINVAL&&!opens);
  open_error=1;assert(glass_backlight_set(100)==-ENOENT);glass_backlight_read(&s);assert(!s.applied);
  open_error=0;assert(glass_backlight_set(100)==0&&current_duty==65535);glass_backlight_read(&s);
  assert(s.available&&s.applied&&s.percent==100&&s.duty==65535&&!s.error);
  unsigned before=sets;assert(!glass_backlight_set(100)&&sets==before&&opens==2);
  assert(!glass_backlight_set(10)&&current_duty==6554);
  assert(!glass_backlight_set(50)&&current_duty==32768);
  fail_set=1;assert(glass_backlight_set(75)==-EIO);glass_backlight_read(&s);
  assert(s.applied&&s.percent==50&&s.error==-EIO&&current_duty==32768);
  assert(!glass_backlight_set(50));
  fail_start=1;assert(glass_backlight_set(75)==-EIO);glass_backlight_read(&s);
  assert(s.applied&&s.percent==50&&s.error==-EIO&&current_duty==32768);
  fail_start=2;assert(glass_backlight_set(75)==-EIO);glass_backlight_read(&s);assert(!s.applied&&s.error==-EIO);
  assert(!glass_backlight_set(100));glass_backlight_read(&s);assert(s.applied&&s.percent==100&&!s.error&&current_duty==65535);
#endif
  puts("BACKLIGHT_OK: single-channel PWM ABI; GPIO route guard; 100% avoids uint16 wrap; bounds, no-op, error rollback and recovery");
  return 0;
}
