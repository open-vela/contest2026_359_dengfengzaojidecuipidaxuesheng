/* SPDX-License-Identifier: Apache-2.0
 * In-process MCP provider: no network listener or shell interpreter. */
/* Completed GPIO/bus/PWM state survives a model response. Only one request
 * owns the controller at a time; aborted/incomplete work releases everything. */
static pthread_mutex_t board_hardware_lock=PTHREAD_MUTEX_INITIALIZER;
static struct qpk_hw_session *board_hardware;
static struct glass_agent *board_hardware_owner;
static int board_hardware_acquire(struct glass_agent *a)
{
  pthread_mutex_lock(&board_hardware_lock);
  int ret=0;
  if(board_hardware_owner&&board_hardware_owner!=a)ret=-EBUSY;
  else {
    if(!board_hardware)board_hardware=qpk_hw_create("com.openvela.espclaw.control");
    if(!board_hardware)ret=-ENOMEM;
    else {board_hardware_owner=a;a->hardware=board_hardware;}
  }
  pthread_mutex_unlock(&board_hardware_lock);return ret;
}
static void board_hardware_detach(struct glass_agent *a)
{
  pthread_mutex_lock(&board_hardware_lock);
  if(board_hardware_owner==a){
    if(cancelled(a)||a->board_jobs){qpk_hw_release(board_hardware);board_hardware=NULL;}
    board_hardware_owner=NULL;a->hardware=NULL;
  }
  pthread_mutex_unlock(&board_hardware_lock);
}
static const char *board_operations[] = {
  "gpio.open", "gpio.read", "gpio.write", "gpio.close",
  "i2c.open", "i2c.transfer", "i2c.close", "spi.open", "spi.transfer", "spi.close",
  "uart.open", "uart.read", "uart.write", "uart.close", "pwm.write", "pwm.stop",
  "servo.write", "servo.stop", "audio.record", "audio.play", "audio.tone",
  "audio.save", "audio.info", "audio.list", "audio.remove", "ble.start",
  "ble.scan", "ble.status", "ble.connect", "ble.cancel", "ble.disconnect", "ble.services", "ble.characteristics", "ble.descriptors",
  "ble.read", "ble.write", "ble.subscribe", "ble.notification", "ble.pair", "ble.mtu", "net.status",
  "usb.list", "usb.info", "usb.mode", "usb.hidRead", "usb.hidReport",
  "usb.audioStart", "usb.audioRead", "usb.audioWrite", "usb.audioStop", "usb.cdcRead", "usb.cdcWrite",
  "usb.mount", "usb.unmount", "usb.files", "usb.readFile", "usb.writeFile",
  "jbox.discover", "jbox.connect", "jbox.disconnect", "jbox.caps", "jbox.status", "jbox.safe",
  "jbox.routeSet", "jbox.connectNodes", "jbox.removeNode", "jbox.routeClear",
  "jbox.gpioConfig", "jbox.gpioWrite", "jbox.gpioRead", "jbox.adcRead", "jbox.pwm",
  "jbox.i2cSetup", "jbox.i2cTransfer", "jbox.i2cClose",
  "jbox.spiSetup", "jbox.spiTransfer", "jbox.spiClose",
  "jbox.uartSetup", "jbox.uartWrite", "jbox.uartRead", "jbox.uartClose",
  "jbox.functionLoad", "jbox.functionCompile", "jbox.functionCall", "jbox.functionClear"
};
static const char board_tool_catalog[] =
"{\"tools\":["
"{\"name\":\"apps_list\",\"description\":\"List built-in and installed apps. Paginate using offset, 16 per page.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"offset\":{\"type\":\"integer\",\"minimum\":0}},\"additionalProperties\":false}},"
"{\"name\":\"app_open\",\"description\":\"Open an exact id returned by apps_list on the board display. Opening an app may leave the chat page. Does not start recording/capture automatically.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"],\"additionalProperties\":false}},"
"{\"name\":\"device_status\",\"description\":\"Read current board navigation state.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
"{\"name\":\"ui_read\",\"description\":\"Read up to 64 visible labels on the board. Input values are excluded.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
"{\"name\":\"ui_click\",\"description\":\"Click an exact visible button label from ui_read. Only perform the user requested action; recording or capture requires that request.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"label\":{\"type\":\"string\",\"maxLength\":128}},\"required\":[\"label\"],\"additionalProperties\":false}},"
"{\"name\":\"hardware_capabilities\",\"description\":\"Read actual board pin/bus/audio/BLE capabilities and supported operation names.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
"{\"name\":\"hardware_reference\",\"description\":\"Read real hardware arguments before submitting. Sections 1=async, 2=GPIO, 3=I2C, 4=SPI, 5=UART, 6=PWM/servo, 7=audio, 8=BLE/USB/network/JBOX-S3. Names system.X.Y(options) map to operation X.Y and arguments options.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"section\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}},\"required\":[\"section\"],\"additionalProperties\":false}},"
"{\"name\":\"hardware_submit\",\"description\":\"Submit a documented hardware operation. Read capabilities and reference first. Returns job id, NOT completion. Poll until done and ok. Completed pin/bus/PWM state persists across requests; close explicitly or hardware_release. Unfinished jobs or user cancellation release the controller. Use bounded durations. Only operate hardware requested by the user.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"operation\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"operation\",\"arguments\"],\"additionalProperties\":false}},"
"{\"name\":\"hardware_release\",\"description\":\"Stop and release all GPIO, buses, PWM, audio and BLE owned by this board controller. Other apps keep their own resources.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
"{\"name\":\"hardware_poll\",\"description\":\"Poll a submitted job. Completed result is consumed once. No retry of side effects after errors/timeouts.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"id\"],\"additionalProperties\":false}},"
"{\"name\":\"hardware_stop\",\"description\":\"Cancel job (finish=false, default); finish=true saves a recording normally. Then poll to completion.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":1},\"finish\":{\"type\":\"boolean\"}},\"required\":[\"id\"],\"additionalProperties\":false}}]}";

static bool board_integer(const cJSON *args,const char *key,unsigned low,unsigned high,unsigned *out)
{
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(args,key);
  if(!cJSON_IsNumber(v)||!(v->valuedouble>=low&&v->valuedouble<=high)||v->valuedouble!=(unsigned)v->valuedouble)return false;
  *out=(unsigned)v->valuedouble;return true;
}
static char *board_submit(struct glass_agent *a,const char *operation,const cJSON *options)
{
  if(a->board_jobs>=8)return error_result("invalid_operation_arguments_or_job_limit");
  char *encoded=cJSON_PrintUnformatted(options);cJSON *r=cJSON_CreateObject();
  if(!encoded||!r){free(encoded);cJSON_Delete(r);return NULL;}
  if(board_hardware_acquire(a)){free(encoded);cJSON_Delete(r);return error_result("hardware_controller_busy_or_unavailable");}
  int id=qpk_hw_submit(a->hardware,operation,encoded);free(encoded);
  cJSON_AddBoolToObject(r,"ok",id>0);
  if(id>0){a->board_jobs++;cJSON_AddNumberToObject(r,"id",id);cJSON_AddStringToObject(r,"state","submitted");}
  else cJSON_AddNumberToObject(r,"error",id);
  char *text=cJSON_PrintUnformatted(r);cJSON_Delete(r);return text;
}
static char *board_call(struct glass_agent *a,const char *tool,const cJSON *args)
{
  if(!cJSON_IsObject(args))return error_result("invalid_arguments");
  if(!strncmp(tool,"jbox_",5)){
    for(unsigned i=0;i<sizeof(board_jbox_tools)/sizeof(board_jbox_tools[0]);i++)if(!strcmp(tool,board_jbox_tools[i].name)){
      cJSON *definition=cJSON_Parse(board_jbox_tools[i].definition);if(!definition)return NULL;
      bool valid=jbox_schema_valid(args,cJSON_GetObjectItemCaseSensitive(definition,"inputSchema"));cJSON_Delete(definition);
      return valid?board_submit(a,board_jbox_tools[i].operation,args):error_result("invalid_arguments");
    }
    return error_result("unknown_tool_or_invalid_arguments");
  }
  /* Smart home tools run entirely inside the shared HA service: no page opens
   * and no LVGL object is touched, so the chat stays in place. */
  if(!strncmp(tool,"home_",5))return g_board_home&&g_board_home->call?
    g_board_home->call(tool,args,a->cancel):error_result("unknown_tool_or_invalid_arguments");
  if(!strncmp(tool,"music_",6))return g_board_music&&g_board_music->call?
    g_board_music->call(tool,args,a->cancel):error_result("unknown_tool_or_invalid_arguments");
  if(!strncmp(tool,"assistant_",10))return g_board_assistant&&g_board_assistant->call?
    g_board_assistant->call(tool,args,a->cancel):error_result("unknown_tool_or_invalid_arguments");
  if(!strcmp(tool,"hardware_release")&&cJSON_GetArraySize(args)==0){
    pthread_mutex_lock(&board_hardware_lock);
    if(board_hardware_owner&&board_hardware_owner!=a){pthread_mutex_unlock(&board_hardware_lock);return error_result("hardware_controller_busy");}
    qpk_hw_release(board_hardware);board_hardware=NULL;board_hardware_owner=NULL;a->hardware=NULL;a->board_jobs=0;
    pthread_mutex_unlock(&board_hardware_lock);return strdup("{\"ok\":true,\"state\":\"release_requested\"}");
  }
  if(!strcmp(tool,"apps_list")||!strcmp(tool,"app_open")||!strcmp(tool,"device_status")||!strcmp(tool,"ui_read")||!strcmp(tool,"ui_click")) {
    unsigned offset=0;
    bool valid=!strcmp(tool,"apps_list")?arguments_only(args,",offset,")&&
      (!cJSON_GetObjectItemCaseSensitive(args,"offset")||board_integer(args,"offset",0,100000,&offset)):
      !strcmp(tool,"app_open")?arguments_only(args,",id,")&&jstring(args,"id")[0]:
      !strcmp(tool,"ui_click")?arguments_only(args,",label,")&&jstring(args,"label")[0]&&strlen(jstring(args,"label"))<=128&&jstring(args,"label")[0]!='*':cJSON_GetArraySize(args)==0;
    return valid?glass_board_ui(tool,args,a->cancel):error_result("invalid_arguments");
  }
  if(!strcmp(tool,"hardware_reference")) {
    unsigned section;
    if(!arguments_only(args,",section,")||!board_integer(args,"section",1,8,&section))return error_result("invalid_arguments");
    return strdup(board_hardware_reference[section-1]);
  }
  if(!strcmp(tool,"hardware_capabilities")&&cJSON_GetArraySize(args)==0) {
    char *text=qpk_hw_capabilities();cJSON *r=text?cJSON_Parse(text):NULL;free(text);
    if(!r)return error_result("capabilities_unavailable");
    cJSON *ops=cJSON_AddArrayToObject(r,"operations");
    for(unsigned i=0;i<sizeof(board_operations)/sizeof(board_operations[0]);i++)cJSON_AddItemToArray(ops,cJSON_CreateString(board_operations[i]));
    text=cJSON_PrintUnformatted(r);cJSON_Delete(r);return text;
  }
  if(!strcmp(tool,"hardware_submit")) {
    const char *operation=jstring(args,"operation");bool known=false;
    const cJSON *options=cJSON_GetObjectItemCaseSensitive(args,"arguments");
    for(unsigned i=0;i<sizeof(board_operations)/sizeof(board_operations[0]);i++)if(!strcmp(operation,board_operations[i]))known=true;
    if(!arguments_only(args,",operation,,arguments,")||!known||!cJSON_IsObject(options)||a->board_jobs>=8)return error_result("invalid_operation_arguments_or_job_limit");
    return board_submit(a,operation,options);
  }
  unsigned id;
  if(!strcmp(tool,"hardware_poll")&&arguments_only(args,",id,")&&board_integer(args,"id",1,INT_MAX,&id)) {
    /* Pace successive model tool rounds without blocking the UI thread. */
    usleep(100000);
    char *text=qpk_hw_poll(a->hardware,id);cJSON *r=text?cJSON_Parse(text):NULL;
    if(r&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"done"))&&strcmp(jstring(r,"state"),"unknown")&&a->board_jobs)a->board_jobs--;
    cJSON_Delete(r);return text?text:error_result("unknown_job");
  }
  if(!strcmp(tool,"hardware_stop")&&arguments_only(args,",id,,finish,")&&board_integer(args,"id",1,INT_MAX,&id)) {
    const cJSON *finish=cJSON_GetObjectItemCaseSensitive(args,"finish");
    if(finish&&!cJSON_IsBool(finish))return error_result("invalid_arguments");
    int ret=qpk_hw_cancel(a->hardware,id,cJSON_IsTrue(finish));
    return ret?error_result("stop_failed"):strdup("{\"ok\":true,\"state\":\"stop_requested\"}");
  }
  return error_result("unknown_tool_or_invalid_arguments");
}
