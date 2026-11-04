/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_backlight.h"
#ifdef GLASS_PWM_BACKLIGHT
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/timers/p4_lcd_pwm.h>
_Static_assert(sizeof(struct glass_backlight_signal)==sizeof(struct p4_lcd_pwm_state_s),"PWM signal ABI");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int fd=-1;
static struct glass_backlight_status state={.available=true,.percent=100,.signal_error=-EAGAIN};

static int start(unsigned duty)
{
  struct pwm_info_s info={.frequency=50000,.duty=duty};
  if(ioctl(fd,PWMIOC_SETCHARACTERISTICS,(unsigned long)&info)<0)return -errno;
  if(ioctl(fd,PWMIOC_START,0)<0)return -errno;
  return 0;
}

int glass_backlight_set(unsigned percent)
{
  if(percent<10||percent>100)return -EINVAL;
  pthread_mutex_lock(&lock);
  if(state.applied&&!state.error&&state.percent==percent)
    {pthread_mutex_unlock(&lock);return 0;}
  /* Standard NuttX unsigned 16.16; the dedicated LCD conversion retains 1.0. */
  unsigned duty=(percent*65536u+50u)/100u;
  int ret=0;
  if(fd<0){fd=open("/dev/pwm0",O_RDWR|O_CLOEXEC);if(fd<0)ret=-errno;}
  if(!ret)ret=start(duty);
  if(ret)
    {
      /* Do not stop/close a working backlight on an update failure. Restore
       * its last confirmed duty; report an unknown output if that fails too. */
      if(state.applied&&start(state.duty)<0)state.applied=false;
      state.error=ret;
    }
  else
    {state.applied=true;state.percent=percent;state.duty=duty;state.error=0;}
  if(state.applied)state.signal_error=ioctl(fd,P4_LCD_PWM_GETSTATE,(unsigned long)&state.signal)<0?-errno:0;
  pthread_mutex_unlock(&lock);
  return ret;
}

void glass_backlight_read(struct glass_backlight_status *status)
{pthread_mutex_lock(&lock);*status=state;pthread_mutex_unlock(&lock);}
#endif
