#!/usr/bin/env python3
"""Compile the actual backlight lower half against deterministic HAL/VFS mocks."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--nuttx', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
driver = repo / 'app/espdl-quickapp/overlay/nuttx/arch/risc-v/src/common/espressif'
a.output.mkdir(parents=True, exist_ok=True)

common = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#define FAR
#define OK 0
#define OUTPUT_FUNCTION_2 2
#define INPUT 1
#define SOC_MOD_CLK_XTAL 40
#define MCPWM_TIMER_COUNT_MODE_UP 0
#define MCPWM_TIMER_DIRECTION_UP 0
#define MCPWM_TIMER_EVENT_EMPTY 0
#define MCPWM_GEN_ACTION_HIGH 1
#define MCPWM_GEN_ACTION_LOW 0
#define MCPWM_TIMER_START_NO_STOP 1
#define MCPWM_TIMER_STOP_EMPTY 0
static int allocate_failure,registration_error,clock_failure,gpio_failure,lock_failure;
static unsigned allocated,routes,configures,resets,source_refs,comparison,period;
static int critical,force_level=1,timer_command,last_signal;
static bool bus_clock,group_clock,gpio_high;
static uint64_t time_us;
static unsigned counter;
typedef int mutex_t;
static inline int nxmutex_init(mutex_t *m){*m=0;return 0;}
static inline int nxmutex_lock(mutex_t *m){if(lock_failure)return -EAGAIN;assert(!*m);*m=1;return 0;}
static inline int nxmutex_unlock(mutex_t *m){assert(*m==1);*m=0;return 0;}
static inline int nxmutex_destroy(mutex_t *m){assert(!*m);return 0;}
static inline void *kmm_zalloc(size_t n){if(allocate_failure)return NULL;void *v=calloc(1,n);assert(v);allocated++;return v;}
static inline void kmm_free(void *v){if(v){assert(allocated);allocated--;free(v);}}
typedef struct {int group;} mcpwm_dev_t;
typedef struct {mcpwm_dev_t *dev;} mcpwm_hal_context_t;
typedef struct {int group_id;} mcpwm_hal_init_config_t;
static mcpwm_dev_t hardware={1};
static inline void check(mcpwm_dev_t *d){assert(d==&hardware&&d->group==1);}
#define PERIPH_RCC_ATOMIC() for(critical=1;critical;critical=0)
static inline int esp_clk_tree_enable_src(int source,bool enable){assert(source==40);if(enable){if(clock_failure)return 1;source_refs++;}else{assert(source_refs);source_refs--;}return 0;}
static inline void mcpwm_ll_enable_bus_clock(int g,bool v){assert(g==1&&critical);bus_clock=v;}
static inline void mcpwm_ll_reset_register(int g){assert(g==1&&critical);resets++;}
static inline void mcpwm_ll_group_enable_clock(int g,bool v){assert(g==1&&critical);group_clock=v;}
static inline void mcpwm_ll_group_set_clock_source(int g,int s){assert(g==1&&s==40&&critical);}
static inline void mcpwm_ll_group_set_clock_prescale(int g,int n){assert(g==1&&n==1&&critical);}
static inline void mcpwm_hal_init(mcpwm_hal_context_t *h,const mcpwm_hal_init_config_t *c){assert(c->group_id==1&&bus_clock&&group_clock);h->dev=&hardware;}
static inline void mcpwm_hal_timer_reset(mcpwm_hal_context_t *h,int t){check(h->dev);assert(!t);counter=0;timer_command=0;}
static inline void mcpwm_hal_operator_reset(mcpwm_hal_context_t *h,int o){check(h->dev);assert(!o);}
static inline void mcpwm_hal_generator_reset(mcpwm_hal_context_t *h,int o,int g){check(h->dev);assert(!o&&!g);}
static inline void mcpwm_ll_group_flush_shadow(mcpwm_dev_t *d){check(d);}
static inline unsigned mcpwm_ll_timer_get_count_value(mcpwm_dev_t *d,int t){check(d);assert(!t);return counter;}
static inline void mcpwm_ll_timer_set_clock_prescale(mcpwm_dev_t *d,int t,int n){check(d);assert(t==0&&n==1);}
static inline void mcpwm_ll_timer_set_peak(mcpwm_dev_t *d,int t,unsigned n,bool symmetric){check(d);assert(t==0&&n==800&&!symmetric);period=n;}
static inline void mcpwm_ll_timer_update_period_at_once(mcpwm_dev_t *d,int t){check(d);assert(t==0);}
static inline void mcpwm_ll_timer_set_count_mode(mcpwm_dev_t *d,int t,int mode){check(d);assert(t==0&&mode==0);}
static inline void mcpwm_ll_operator_connect_timer(mcpwm_dev_t *d,int o,int t){check(d);assert(o==0&&t==0);}
static inline void mcpwm_ll_operator_enable_update_compare_on_tez(mcpwm_dev_t *d,int o,int c,bool v){check(d);assert(o==0&&c==0&&v);}
static inline void mcpwm_ll_generator_set_action_on_timer_event(mcpwm_dev_t *d,int o,int g,int direction,int event,int action){check(d);assert(!o&&!g&&!direction&&!event&&action==1);}
static inline void mcpwm_ll_generator_set_action_on_compare_event(mcpwm_dev_t *d,int o,int g,int direction,int compare,int action){check(d);assert(!o&&!g&&!direction&&!compare&&action==0);}
static inline void mcpwm_ll_gen_set_continue_force_level(mcpwm_dev_t *d,int o,int g,int v){check(d);assert(!o&&!g&&(v==0||v==1));force_level=v;}
static inline void mcpwm_ll_gen_disable_continue_force_action(mcpwm_dev_t *d,int o,int g){check(d);assert(!o&&!g);force_level=-1;}
static inline void mcpwm_ll_operator_set_compare_value(mcpwm_dev_t *d,int o,int c,unsigned v){check(d);assert(!o&&!c&&v<800);comparison=v;}
static inline void mcpwm_ll_timer_set_start_stop_command(mcpwm_dev_t *d,int t,int command){check(d);assert(t==0);timer_command=command;}
static inline int esp_configgpio(int pin,int flags){assert(pin==26&&flags==3);configures++;return gpio_failure?-EINVAL:0;}
static inline void esp_gpiowrite(int pin,bool value){assert(pin==26);gpio_high=value;}
static inline void esp_gpio_matrix_out(int pin,int signal,bool a,bool b){assert(pin==26&&!a&&!b&&(signal==321||signal==0x100));routes++;last_signal=signal;}
static inline void up_udelay(unsigned us){time_us+=us;if(timer_command==1)counter=(counter+40*us)%800;}
int64_t esp_timer_get_time(void){return time_us;}
static inline bool esp_gpioread(int pin){assert(pin==26);return last_signal==0x100?gpio_high:force_level>=0?force_level:counter<comparison;}
static const struct {struct {struct {int pwm_sig;} generators[2];} operators[3];} soc_mcpwm_signals[2]={[1]={.operators={{.generators={{.pwm_sig=321}}}}}};
'''
pwm = r'''
#pragma once
#include "fake.h"
struct pwm_lowerhalf_s;
struct pwm_info_s {unsigned frequency;
#ifdef CONFIG_PWM_MULTICHAN
 struct {uint32_t duty;int channel;} channels[2];
#else
 uint32_t duty;
#endif
 unsigned count;
};
struct pwm_ops_s {int (*setup)(struct pwm_lowerhalf_s *);int (*shutdown)(struct pwm_lowerhalf_s *);
#ifdef CONFIG_PWM_PULSECOUNT
 int (*start)(struct pwm_lowerhalf_s *,const struct pwm_info_s *,void *);
#else
 int (*start)(struct pwm_lowerhalf_s *,const struct pwm_info_s *);
#endif
 int (*stop)(struct pwm_lowerhalf_s *);int (*ioctl)(struct pwm_lowerhalf_s *,int,unsigned long);};
struct pwm_lowerhalf_s {const struct pwm_ops_s *ops;};
static struct pwm_lowerhalf_s *registered;
static inline int pwm_register(const char *path,struct pwm_lowerhalf_s *lower){assert(!strcmp(path,"/dev/backlight0"));if(registration_error)return registration_error;if(registered)return -EEXIST;registered=lower;return 0;}
'''
main = r'''
#include <stdio.h>
static int start(unsigned frequency,unsigned duty,unsigned count)
{
 struct pwm_info_s info={.frequency=frequency,.count=count};
#ifdef CONFIG_PWM_MULTICHAN
 info.channels[0].channel=1;info.channels[0].duty=duty;
#else
 info.duty=duty;
#endif
#ifdef CONFIG_PWM_PULSECOUNT
 return registered->ops->start(registered,&info,NULL);
#else
 return registered->ops->start(registered,&info);
#endif
}
int main(void)
{
 allocate_failure=1;assert(esp_lcd_backlight_initialize()==-ENOMEM);allocate_failure=0;
 registration_error=-EIO;assert(esp_lcd_backlight_initialize()==-EIO&&!allocated);registration_error=0;
 assert(!esp_lcd_backlight_initialize()&&allocated==1&&!routes&&!configures);
 assert(!esp_lcd_backlight_initialize()&&allocated==1);
 clock_failure=1;assert(registered->ops->setup(registered)==-EIO&&!source_refs);clock_failure=0;
 gpio_failure=1;assert(registered->ops->setup(registered)==-EINVAL&&!source_refs&&!bus_clock&&!group_clock);gpio_failure=0;
 for(unsigned i=0;i<50;i++){
  assert(!registered->ops->setup(registered)&&source_refs==1&&force_level==1&&last_signal==0x100&&gpio_high);
  unsigned before=resets;assert(!registered->ops->setup(registered)&&resets==before);
  assert(start(4000,32768,0)==-EINVAL&&start(50000,65537,0)==-EINVAL);
  lock_failure=1;assert(start(50000,32768,0)==-EAGAIN);lock_failure=0;
  assert(!start(50000,32768,0)&&period==800&&comparison==400&&force_level==-1&&timer_command==1&&last_signal==321);
  struct p4_backlight_state_s state;
  assert(!registered->ops->ioctl(registered,P4_BACKLIGHT_GETSTATE,(unsigned long)&state));
  assert(state.duty==32768&&state.samples==2000&&state.high_samples==1000&&state.transitions>=399&&state.transitions<=400&&state.elapsed_us==4000&&state.pwm_routed);
  assert(registered->ops->ioctl(registered,P4_BACKLIGHT_GETSTATE,0)==-EINVAL);
  assert(!start(50000,6553,0)&&comparison==79);
  assert(!start(50000,65536,0)&&force_level==1&&last_signal==0x100&&gpio_high);
  assert(!registered->ops->ioctl(registered,P4_BACKLIGHT_GETSTATE,(unsigned long)&state));
  assert(state.high_samples==2000&&!state.transitions&&!state.pwm_routed);
  assert(!start(50000,0,0)&&force_level==0&&last_signal==0x100&&!gpio_high);
  assert(!registered->ops->ioctl(registered,P4_BACKLIGHT_GETSTATE,(unsigned long)&state));
  assert(!state.high_samples&&!state.transitions&&!state.pwm_routed);
#ifdef CONFIG_PWM_PULSECOUNT
  assert(start(50000,32768,1)==-ENOTSUP);
#endif
  assert(registered->ops->ioctl(registered,123,0)==-ENOTTY);
  assert(!registered->ops->stop(registered)&&force_level==0);
  assert(!registered->ops->shutdown(registered)&&!source_refs&&!bus_clock&&!group_clock&&gpio_high&&last_signal==0x100);
 }
 nxmutex_destroy(&((struct esp_backlight_s *)registered)->lock);kmm_free(registered);registered=NULL;assert(!allocated);
 puts("BACKLIGHT_DRIVER_OK: dedicated group1/GPIO26; lazy registration; 50kHz ETA1611 duty/endpoints; invalid requests; clock/GPIO/lock/allocation/registration failures; 50 reopen cycles and readable close state");
}
'''
for name, defines in [('single', ''), ('multi', '#define CONFIG_PWM_MULTICHAN 1\n'), ('pulse', '#define CONFIG_PWM_PULSECOUNT 1\n')]:
    work = a.output / name
    for directory in ['nuttx/timers', 'nuttx/fs', 'hal', 'esp_private']:
        (work / directory).mkdir(parents=True, exist_ok=True)
    (work / 'nuttx/config.h').write_text('#pragma once\n#define CONFIG_PWM 1\n#define CONFIG_ESP32P4_LCD_BACKLIGHT 1\n' + defines)
    (work / 'fake.h').write_text(common)
    (work / 'nuttx/timers/pwm.h').write_text(pwm)
    (work / 'nuttx/fs/ioctl.h').write_text('#define _PWMIOC(n) (0x3100|(n))\n')
    for header in ['nuttx/kmalloc.h', 'nuttx/arch.h', 'nuttx/mutex.h', 'esp_gpio.h', 'hal/mcpwm_hal.h', 'hal/mcpwm_ll.h', 'hal/mcpwm_periph.h', 'periph_ctrl.h', 'esp_private/esp_clk_tree_common.h']:
        (work / header).write_text('#include "fake.h"\n')
    source = work / 'test.c'
    source.write_text('#include <string.h>\n#include "' + str(driver / 'esp_lcd_backlight.c') + '"\n' + main)
    executable = work / 'test'
    subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', '-fno-pie', '-no-pie', '-I'+str(work), '-I'+str(driver), '-I'+str(repo/'app/espdl-quickapp/overlay/nuttx/include'), str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, env={**os.environ, 'ASAN_OPTIONS':'detect_leaks=1'})

from prepare_p4_backlight import apply
with tempfile.TemporaryDirectory(prefix='backlight-prepare-') as directory:
    import shutil
    root = Path(directory)
    for file in ['boards/risc-v/esp32p4/esp32p4-function-ev-board/Kconfig', 'boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd.c', 'arch/risc-v/src/common/espressif/Make.defs', 'arch/risc-v/src/common/espressif/CMakeLists.txt']:
        destination = root / file
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(a.nuttx / file, destination)
    before = apply(root)
    after = apply(root)
    assert before == after
    assert after['group'] == 1 and after['gpio'] == 26
print('BACKLIGHT_PREPARE_OK: idempotent isolated Make/CMake/Kconfig/board registration; existing display sequence retained')
