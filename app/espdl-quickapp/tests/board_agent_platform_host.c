/* SPDX-License-Identifier: Apache-2.0
 * Deterministic platform fixture; the actual async hardware scheduler is linked. */
#include "qpk_hardware.h"
#include "glass_board.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
void *qpk_hw_platform_create(const char *package){return !strcmp(package,"com.openvela.espclaw.control")?malloc(1):NULL;}
void *qpk_hw_platform_create_at(const char *package,const char *root){(void)root;return qpk_hw_platform_create(package);}
void qpk_hw_platform_destroy(void *p){free(p);}
int qpk_hw_platform_execute(void *p,const char *operation,const cJSON *a,cJSON *out,struct qpk_hw_progress *progress)
{(void)p;(void)a;(void)progress;
 if(!strcmp(operation,"jbox.status")){cJSON_AddNumberToObject(out,"heapFree",456);return 0;}
 if(strcmp(operation,"gpio.read"))return -EINVAL;cJSON_AddNumberToObject(out,"value",1);return 0;}
char *qpk_hw_capabilities(void){return strdup("{\"gpioPins\":[0,1,2,3,4,5,6]}");}
unsigned glass_board_ui_calls;
char *glass_board_ui(const char *op,const cJSON *a,atomic_bool *cancel)
{(void)a;(void)cancel;glass_board_ui_calls++;return strdup(!strcmp(op,"device_status")?"{\"ok\":true,\"page\":\"home\"}":"{\"error\":\"unsupported_in_host_fixture\"}");}
