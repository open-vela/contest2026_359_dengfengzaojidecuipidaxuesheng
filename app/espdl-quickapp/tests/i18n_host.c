/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_i18n.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int wait_saved(void)
{
  for(unsigned i=0;i<3000;i++){
    int status=glass_i18n_save_status();if(status)return status;
    usleep(1000);
  }
  assert(!"language save timed out");return 0;
}
int main(int argc,char **argv)
{
  assert(argc==3);
  const char *mode=argv[1];unsigned expected=(unsigned)strtoul(argv[2],NULL,10);
  assert(!strcmp(glass_i18n_code(),"zh-CN"));
  assert(glass_i18n_set_language(999)==-EINVAL);
  if(!strcmp(mode,"restore")){
    glass_i18n_start();
    for(unsigned i=0;i<500;i++){glass_i18n_poll();usleep(1000);}
    assert(glass_i18n_language()==expected);
  }else if(!strcmp(mode,"override")){
    glass_i18n_start();
    assert(glass_i18n_set_language(GLASS_LANGUAGE_ZH_CN)==0);
    for(unsigned i=0;i<1500;i++){glass_i18n_poll();usleep(1000);}
    assert(glass_i18n_language()==GLASS_LANGUAGE_ZH_CN);
    assert(wait_saved()==1);
  }else if(!strcmp(mode,"failure")){
    assert(glass_i18n_set_language(GLASS_LANGUAGE_EN)==0);
    assert(wait_saved()<0);
    assert(glass_i18n_language()==GLASS_LANGUAGE_EN);
  }else {
    for(unsigned i=0;i<100;i++)assert(glass_i18n_set_language(i%2)==0);
    assert(glass_i18n_set_language(GLASS_LANGUAGE_EN)==0);
    assert(wait_saved()==1);
    expected=GLASS_LANGUAGE_EN;
  }
  const char *user="A user filename 设置.txt";
  assert(glass_tr(user)==user);
  assert(!strcmp(glass_tr("控制面板"),expected==GLASS_LANGUAGE_EN?"Control panel":"控制面板"));
  assert(!strcmp(glass_tr("连接成功 · IP: %u.%u.%u.%u"),expected==GLASS_LANGUAGE_EN?
    "Connected · IP: %u.%u.%u.%u":"连接成功 · IP: %u.%u.%u.%u"));
  puts("I18N_HOST_OK");return 0;
}
