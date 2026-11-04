#!/usr/bin/env python3
"""Exercise the LCD-only patched PWM conversion and physical signal ioctl."""
from pathlib import Path
import argparse,subprocess,tempfile,shutil,os
from prepare_p4_lcd_pwm import apply
p=argparse.ArgumentParser();p.add_argument('--nuttx',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);relative=Path('arch/risc-v/src/common/espressif/esp_ledc.c');(root/relative).parent.mkdir(parents=True)
 shutil.copy2(a.nuttx/relative,root/relative);first=apply(root);assert first==apply(root)
 text=(root/relative).read_text();begin=text.index('#ifdef P4_LCD_PWM\nextern int64_t esp_timer_get_time');end=text.index('\nstatic int pwm_ioctl(',begin)
 support=text[begin:end]
 assert 'ledc_duty_bin_conversion(uint16_t nuttx_duty' in text
 assert 'if (priv->timer == 0 && info->duty > 65536u)' in text
 assert 'OUTPUT | INPUT | PULLUP' in text
 pre=r"""
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#define P4_LCD_PWM 1
#define OK 0
#define LEDC_LL_FRACTIONAL_BITS 8
#define LEDC_CLK_SRC_FREQ_PRECISION 0
struct p4_lcd_pwm_state_s{uint32_t frequency,source_hz,divider,resolution,duty_ticks,samples,high_samples,transitions,elapsed_us;};
struct pwm_lowerhalf_s{int ignored;};
struct esp_ledc_chan_s{int pin,num;uint32_t duty;};
struct esp_ledc_s{int timer,channels;struct esp_ledc_chan_s *chans;};
typedef int ledc_slow_clk_sel_t;typedef int soc_module_clk_t;
static struct{int ledc_hal;} controller,*p_ledc_obj=&controller;
static unsigned g_ledc_open_timers=1,g_ledc_lock,locks,unlocks;
static int lock_error,clock_error;static uint32_t raw=256,resolution=9,divider=400;static int64_t now,ready_at;
static int nxmutex_lock(unsigned *p){(void)p;if(lock_error)return -EAGAIN;assert(!locks);locks++;return 0;}
static int nxmutex_unlock(unsigned *p){(void)p;assert(locks);locks--;unlocks++;return 0;}
static void ledc_hal_get_slow_clk_sel(int *h,int *s){(void)h;*s=1;}
static int esp_clk_tree_src_get_freq_hz(int s,int p,uint32_t *hz){assert(s==1);(void)p;*hz=40000000;return clock_error;}
static void ledc_hal_get_clock_divider(int *h,int t,uint32_t *v){(void)h;assert(!t);*v=divider;}
static void ledc_hal_get_duty_resolution(int *h,int t,uint32_t *v){(void)h;assert(!t);*v=resolution;}
static void ledc_hal_get_duty(int *h,int c,uint32_t *v){(void)h;assert(!c);*v=now<ready_at?0:raw;}
int64_t esp_timer_get_time(void){return now;}
static bool esp_gpioread(int pin){assert(pin==26);return now>=ready_at && (uint64_t)(now%20)*512u < raw*20u;}
static void up_udelay(int n){assert(n==1);now++;}
"""
 main=r"""
int main(void){
 struct esp_ledc_chan_s channel={26,0,256};struct esp_ledc_s device={0,1,&channel};struct pwm_lowerhalf_s *dev=(void*)&device;struct p4_lcd_pwm_state_s state;
 assert(p4_lcd_pwm_duty(0)==0&&p4_lcd_pwm_duty(65536)==512&&p4_lcd_pwm_duty(32768)==256);
 unsigned previous=0;for(unsigned percent=10;percent<=100;percent++){unsigned duty=(percent*65536u+50)/100;unsigned ticks=p4_lcd_pwm_duty(duty);assert(ticks>previous&&ticks<=512);previous=ticks;}
 assert(p4_lcd_pwm_state(dev,0)==-EINVAL&&!locks);
 device.timer=1;assert(p4_lcd_pwm_state(dev,(unsigned long)&state)==-ENOTTY);device.timer=0;
 lock_error=1;assert(p4_lcd_pwm_state(dev,(unsigned long)&state)==-EAGAIN);lock_error=0;
 g_ledc_open_timers=0;assert(p4_lcd_pwm_state(dev,(unsigned long)&state)==-ENODEV&&!locks);g_ledc_open_timers=1;
 clock_error=1;assert(p4_lcd_pwm_state(dev,(unsigned long)&state)==-EIO&&!locks);clock_error=0;
 divider=0;assert(p4_lcd_pwm_state(dev,(unsigned long)&state)==-EIO&&!locks);divider=400;
 resolution=13;assert(p4_lcd_pwm_state(dev,(unsigned long)&state)==-EIO&&!locks);resolution=9;
 assert(!p4_lcd_pwm_state(dev,(unsigned long)&state)&&state.frequency==50000&&state.duty_ticks==256&&state.samples==2000&&state.high_samples==1000&&state.elapsed_us==2000&&state.transitions>=199&&state.transitions<=200&&!locks);
 ready_at=now+1000;assert(!p4_lcd_pwm_state(dev,(unsigned long)&state)&&state.duty_ticks==256&&state.high_samples==1000&&state.elapsed_us==2000);
 ready_at=now+6000;assert(p4_lcd_pwm_state(dev,(unsigned long)&state)==-ETIMEDOUT&&!locks);ready_at=0;
 raw=512;channel.duty=512;assert(!p4_lcd_pwm_state(dev,(unsigned long)&state)&&state.high_samples==2000&&!state.transitions);
 raw=0;channel.duty=0;assert(!p4_lcd_pwm_state(dev,(unsigned long)&state)&&!state.high_samples&&!state.transitions);
 assert(unlocks==9);puts("LCD_PWM_DRIVER_OK: GPIO26-only patch, standard full-scale and monotone duty; actual clock/readback, bounded async latch wait and pin sampler; wrong route/null/lock/clock/unopened/malformed failures release ownership");
}
"""
 source=a.output/'driver.c';source.write_text(pre+support+main)
 exe=a.output/'driver';subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-pie','-no-pie',str(source),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1'})
print('LCD_PWM_PREPARE_OK: idempotent patch; other PWM conversion unchanged')
