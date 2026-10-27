/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "qpk_hardware.h"
#include "qpk_board_pins.h"
#include "qpk_volume.h"
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "glass_ble_channel.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/spi/spi.h>
#include <nuttx/timers/pwm.h>
#include "espressif/esp_gpio.h"
#include "espressif/esp_i2c.h"
#include "espressif/esp_spi.h"
#include <arch/chip/gpio_sig_map.h>

struct hw_platform {
  char package[48],data_root[256];
  unsigned gpio, outputs;
  struct i2c_master_s *i2c;
  struct spi_dev_s *spi;
  int uart, pwm[4];
  bool ble;
  uint32_t channel_epoch,ble_scan,ble_links[GLASS_BLE_CENTRAL_LIMIT];
  void *usb;
};
static pthread_mutex_t claims_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hw_platform *pin_owner[7], *ble_owner;
static const unsigned pwm_pins[] = {CONFIG_ESPRESSIF_LEDC_CHANNEL0_PIN,CONFIG_ESPRESSIF_LEDC_CHANNEL1_PIN,CONFIG_ESPRESSIF_LEDC_CHANNEL2_PIN,CONFIG_ESPRESSIF_LEDC_CHANNEL3_PIN};
extern int qpk_hw_audio_execute_at(const char *,const char *,const char *,const cJSON *,cJSON *,struct qpk_hw_progress *);
extern int qpk_usb_execute(void **,const char *,const cJSON *,cJSON *,struct qpk_hw_progress *);
extern void qpk_usb_destroy(void *);

static const char *string_arg(const cJSON *a, const char *key, const char *fallback)
{ cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key); return !v ? fallback : cJSON_IsString(v) ? v->valuestring : NULL; }
static int number_arg(const cJSON *a, const char *key, int fallback, int low, int high)
{
  cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key);
  if (!v) return fallback;
  if (!cJSON_IsNumber(v) || !(v->valuedouble >= low && v->valuedouble <= high) || v->valuedouble != (int)v->valuedouble) return -1;
  return v->valueint;
}
static int bytes_arg(const cJSON *a, const char *key, unsigned char **out, size_t *length)
{
  *out = NULL; *length = 0;
  cJSON *v = cJSON_GetObjectItemCaseSensitive(a, key);
  if (!v) return 0;
  if (!cJSON_IsArray(v)) return -EINVAL;
  int n = cJSON_GetArraySize(v);
  unsigned char *bytes = malloc(n ? (size_t)n : 1); if (!bytes) return -ENOMEM;
  int i = 0;
  cJSON *item;
  cJSON_ArrayForEach(item, v) {
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0 && item->valuedouble <= 255) || item->valuedouble != item->valueint) { free(bytes); return -EINVAL; }
    bytes[i++] = item->valueint;
  }
  *out = bytes; *length = n; return 0;
}
static int result_bytes(cJSON *out, const unsigned char *bytes, size_t length)
{
  cJSON *a = cJSON_AddArrayToObject(out, "data"); if (!a) return -ENOMEM;
  for (size_t i = 0; i < length; i++) { cJSON *n = cJSON_CreateNumber(bytes[i]); if (!n) return -ENOMEM; cJSON_AddItemToArray(a, n); }
  cJSON_AddNumberToObject(out, "bytes", length); return 0;
}
static uint64_t milliseconds(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static int claim(struct hw_platform *p, unsigned pins)
{
  if(!pins||(pins&~QPK_HEADER_GPIO_MASK))return -EACCES;
  pthread_mutex_lock(&claims_lock);
  int ret = 0;
  for (unsigned i = 0; i < 7; i++) if ((pins & (1u << i)) && pin_owner[i]) { ret = -EBUSY; break; }
  if (!ret) for (unsigned i = 0; i < 7; i++) if (pins & (1u << i)) pin_owner[i] = p;
  pthread_mutex_unlock(&claims_lock); return ret;
}
static void unclaim(struct hw_platform *p, unsigned pins)
{
  pthread_mutex_lock(&claims_lock);
  for (unsigned i = 0; i < 7; i++) if ((pins & (1u << i)) && pin_owner[i] == p) {
    esp_configgpio(i, INPUT);
    esp_gpio_matrix_out(i, SIG_GPIO_OUT_IDX, false, false);
    pin_owner[i] = NULL;
  }
  pthread_mutex_unlock(&claims_lock);
}
void *qpk_hw_platform_create_at(const char *package,const char *data_root)
{
  if(!data_root||(strcmp(data_root,qpk_volume_data_root(QPK_VOLUME_INTERNAL))&&strcmp(data_root,qpk_volume_data_root(QPK_VOLUME_SD))))return NULL;
  if (!package || !*package || strlen(package) >= 48) return NULL;
  struct hw_platform *p = calloc(1, sizeof(*p)); if (!p) return NULL;
  strcpy(p->package, package);strcpy(p->data_root,data_root);p->uart = -1;
  for (unsigned i = 0; i < 4; i++) p->pwm[i] = -1;
  return p;
}
void *qpk_hw_platform_create(const char *package)
{return qpk_hw_platform_create_at(package,qpk_volume_data_root(QPK_VOLUME_INTERNAL));}
static bool ble_owned(const struct hw_platform *p,uint32_t generation)
{
  if(!generation)return false;
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)if(p->ble_links[i]==generation)return true;
  return false;
}
static void ble_prune(struct hw_platform *p,const struct glass_ble_state *s)
{
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)if(p->ble_links[i]){
    bool found=false;
    for(unsigned n=0;n<s->session_count&&n<GLASS_BLE_CENTRAL_LIMIT;n++)
      if(s->sessions[n].generation==p->ble_links[i])found=true;
    if(!found)p->ble_links[i]=0;
  }
}
static void ble_cleanup_owned(struct hw_platform *p)
{
  if(!p->ble)return;
  if(p->channel_epoch)(void)glass_ble_channel_clear(p->channel_epoch);
  if(p->ble_scan)(void)glass_ble_central_scan_stop(p->ble_scan);
  for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)if(p->ble_links[i])
    (void)glass_ble_central_disconnect_generation(p->ble_links[i]);
}
void qpk_hw_platform_destroy(void *opaque)
{
  struct hw_platform *p = opaque; if (!p) return;
  qpk_usb_destroy(p->usb);
  if (p->i2c) esp_i2cbus_uninitialize(p->i2c);
  if (p->spi) esp_spibus_uninitialize(p->spi);
  if (p->uart >= 0) close(p->uart);
  for (unsigned i = 0; i < 4; i++) if (p->pwm[i] >= 0) { ioctl(p->pwm[i], PWMIOC_STOP, 0); close(p->pwm[i]); }
  ble_cleanup_owned(p);
  pthread_mutex_lock(&claims_lock); if (ble_owner == p) ble_owner = NULL; pthread_mutex_unlock(&claims_lock);
  unclaim(p, 0x7f); free(p);
}

static int gpio_operation(struct hw_platform *p, const char *op, const cJSON *a, cJSON *out)
{
  int pin = number_arg(a, "pin", -1, 0, 6); if (pin < 0) return -EINVAL;
  unsigned bit = 1u << pin;
  if (!strcmp(op, "open")) {
    const char *mode = string_arg(a, "mode", "input"), *pull = string_arg(a, "pull", "none");
    int value = number_arg(a, "value", 0, 0, 1);
    if (!mode || !pull || value < 0) return -EINVAL;
    unsigned flags = !strcmp(mode, "input") ? INPUT : !strcmp(mode, "output") ? INPUT | OUTPUT :
      !strcmp(mode, "openDrain") ? INPUT | OUTPUT | OPEN_DRAIN : 0;
    if (!flags) return -EINVAL;
    if (!strcmp(pull, "up")) flags |= PULLUP;
    else if (!strcmp(pull, "down")) flags |= PULLDOWN;
    else if (strcmp(pull, "none")) return -EINVAL;
    int ret = claim(p, bit); if (ret) return ret;
    esp_gpiowrite(pin, value); esp_gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false);
    ret = esp_configgpio(pin, flags);
    if (ret) { unclaim(p, bit); return ret; }
    p->gpio |= bit; if (flags & OUTPUT) p->outputs |= bit;
  } else {
    if (!(p->gpio & bit)) return -EBADF;
    if (!strcmp(op, "read")) cJSON_AddNumberToObject(out, "value", esp_gpioread(pin));
    else if (!strcmp(op, "write")) {
      int value = number_arg(a, "value", -1, 0, 1);
      if (value < 0) return -EINVAL;
      if (!(p->outputs & bit)) return -EPERM;
      esp_gpiowrite(pin, value);
    } else if (!strcmp(op, "close")) { p->gpio &= ~bit; p->outputs &= ~bit; unclaim(p, bit); }
    else return -ENOSYS;
  }
  return 0;
}

static int i2c_operation(struct hw_platform *p, const char *op, const cJSON *a, cJSON *out)
{
  if (number_arg(a, "bus", 1, 1, 1) != 1) return -EINVAL;
  if (!strcmp(op, "open")) {
    int ret = claim(p, QPK_I2C_MASK); if (ret) return ret;
    p->i2c = esp_i2cbus_initialize(1);
    if (!p->i2c) { unclaim(p, QPK_I2C_MASK); return -ENODEV; }
    return 0;
  }
  if (!p->i2c) return -EBADF;
  if (!strcmp(op, "close")) { int ret = esp_i2cbus_uninitialize(p->i2c); if (ret) return ret; p->i2c = NULL; unclaim(p, QPK_I2C_MASK); return 0; }
  if (strcmp(op, "transfer")) return -ENOSYS;
  int address = number_arg(a, "address", -1, 8, 119), count = number_arg(a, "readLength", 0, 0, 65535);
  int hz = number_arg(a, "frequency", 100000, 10000, 400000);
  if (address < 0 || count < 0 || hz < 0) return -EINVAL;
  unsigned char *tx = NULL, *rx = NULL; size_t length = 0;
  int ret = bytes_arg(a, "data", &tx, &length); if (ret) return ret;
  if (length > 65535 || (!length && !count)) { free(tx); return -EINVAL; }
  if (count && !(rx = malloc(count))) { free(tx); return -ENOMEM; }
  struct i2c_msg_s messages[2]; int n = 0;
  if (length) messages[n++] = (struct i2c_msg_s){.frequency = hz, .addr = address, .flags = 0, .buffer = tx, .length = length};
  if (count) messages[n++] = (struct i2c_msg_s){.frequency = hz, .addr = address, .flags = I2C_M_READ, .buffer = rx, .length = count};
  ret = I2C_TRANSFER(p->i2c, messages, n);
  if (ret >= 0) ret = result_bytes(out, rx, count);
  free(tx); free(rx); return ret;
}

static int spi_operation(struct hw_platform *p, const char *op, const cJSON *a, cJSON *out)
{
  if (number_arg(a, "bus", 2, 2, 2) != 2) return -EINVAL;
  if (!strcmp(op, "open")) {
    int ret = claim(p, QPK_SPI_MASK); if (ret) return ret;
    p->spi = esp_spibus_initialize(2);
    if (!p->spi) { unclaim(p, QPK_SPI_MASK); return -ENODEV; }
    return 0;
  }
  if (!p->spi) return -EBADF;
  if (!strcmp(op, "close")) { int ret = esp_spibus_uninitialize(p->spi); if (ret) return ret; p->spi = NULL; unclaim(p, QPK_SPI_MASK); return 0; }
  if (strcmp(op, "transfer")) return -ENOSYS;
  int mode = number_arg(a, "mode", 0, 0, 3), hz = number_arg(a, "frequency", 1000000, 1000, 20000000);
  if (mode < 0 || hz < 0) return -EINVAL;
  unsigned char *tx = NULL, *rx; size_t length;
  int ret = bytes_arg(a, "data", &tx, &length); if (ret) return ret;
  if (!length) { free(tx); return -EINVAL; }
  rx = malloc(length); if (!rx) { free(tx); return -ENOMEM; }
  ret = SPI_LOCK(p->spi, true);
  if (!ret) {
    SPI_SETMODE(p->spi, mode); SPI_SETBITS(p->spi, 8);
    uint32_t frequency = SPI_SETFREQUENCY(p->spi, hz);
    SPI_SELECT(p->spi, SPIDEV_USER(0), true);
    SPI_EXCHANGE(p->spi, tx, rx, length);
    SPI_SELECT(p->spi, SPIDEV_USER(0), false); SPI_LOCK(p->spi, false);
    cJSON_AddNumberToObject(out, "frequency", frequency); ret = result_bytes(out, rx, length);
  }
  free(tx); free(rx); return ret;
}

static int uart_operation(struct hw_platform *p, const char *op, const cJSON *a, cJSON *out, struct qpk_hw_progress *progress)
{
  if (number_arg(a, "port", 1, 1, 1) != 1) return -EINVAL;
  if (!strcmp(op, "open")) {
    int baud = number_arg(a, "baudRate", 115200, 1200, 921600);
    if (baud < 0) return -EINVAL;
    int ret = claim(p, QPK_UART_MASK); if (ret) return ret;
    /* The serial driver's reopen path does not restore the GPIO matrix. */
    esp_gpiowrite(CONFIG_ESPRESSIF_UART1_TXPIN, true);
    esp_configgpio(CONFIG_ESPRESSIF_UART1_TXPIN, OUTPUT); esp_gpio_matrix_out(CONFIG_ESPRESSIF_UART1_TXPIN, UART1_TXD_PAD_OUT_IDX, false, false);
    esp_configgpio(CONFIG_ESPRESSIF_UART1_RXPIN, INPUT | PULLUP); esp_gpio_matrix_in(CONFIG_ESPRESSIF_UART1_RXPIN, UART1_RXD_PAD_IN_IDX, false);
    p->uart = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
    if (p->uart < 0) { ret = -errno; unclaim(p, QPK_UART_MASK); return ret; }
    struct termios t;
    ret = tcgetattr(p->uart, &t);
    if (!ret) {
      cfmakeraw(&t); t.c_cflag &= ~(CSIZE | PARENB | CSTOPB); t.c_cflag |= CS8 | CREAD | CLOCAL;
      cfsetispeed(&t, baud); cfsetospeed(&t, baud); ret = tcsetattr(p->uart, TCSANOW, &t);
    }
    if (ret) { ret = -errno; close(p->uart); p->uart = -1; unclaim(p, QPK_UART_MASK); }
    return ret;
  }
  if (p->uart < 0) return -EBADF;
  if (!strcmp(op, "close")) { close(p->uart); p->uart = -1; unclaim(p, QPK_UART_MASK); return 0; }
  bool reading = !strcmp(op, "read"); if (!reading && strcmp(op, "write")) return -ENOSYS;
  int timeout = number_arg(a, "timeoutMs", 1000, 0, INT_MAX);
  if (timeout < 0) return -EINVAL;
  unsigned char *bytes = NULL; size_t length = 0;
  if (reading) {
    int n = number_arg(a, "length", 256, 1, INT_MAX); if (n < 0) return -EINVAL;
    length = n; bytes = malloc(length); if (!bytes) return -ENOMEM;
  } else { int ret = bytes_arg(a, "data", &bytes, &length); if (ret) return ret; }
  size_t used = 0; int ret = 0; uint64_t end = milliseconds() + timeout;
  do {
    if (atomic_load(&progress->cancel)) { ret = -ECANCELED; break; }
    ssize_t n = reading ? read(p->uart, bytes + used, length - used) : write(p->uart, bytes + used, length - used);
    if (n > 0) used += n;
    else if (n < 0 && errno != EAGAIN && errno != EINTR) { ret = -errno; break; }
    uint64_t now = milliseconds();
    if (used == length || (reading && used) || now >= end) break;
    struct pollfd fd = {.fd = p->uart, .events = reading ? POLLIN : POLLOUT};
    uint64_t left = end - now; poll(&fd, 1, left < 50 ? left : 50);
  } while (true);
  if (!ret && reading) ret = result_bytes(out, bytes, used);
  else { cJSON_AddNumberToObject(out, "bytes", used); if (!ret && used < length) ret = -ETIMEDOUT; }
  free(bytes); return ret;
}

static int pwm_operation(struct hw_platform *p, const char *op, const cJSON *a, cJSON *out, bool servo)
{
  int channel = number_arg(a, "channel", 0, 0, 3); if (channel < 0) return -EINVAL;
  unsigned bit = 1u << pwm_pins[channel];
  if (!strcmp(op, "stop")) {
    if (p->pwm[channel] >= 0) { ioctl(p->pwm[channel], PWMIOC_STOP, 0); close(p->pwm[channel]); p->pwm[channel] = -1; unclaim(p, bit); }
    return 0;
  }
  if (strcmp(op, "write")) return -ENOSYS;
  int frequency = number_arg(a, "frequency", servo ? 50 : 1000, 1, 100000);
  if (frequency < 0) return -EINVAL;
  double duty;
  if (servo) {
    int pulse = number_arg(a, "pulseUs", 1500, 100, 5000);
    if (pulse < 0 || (double)pulse * frequency >= 1000000) return -EINVAL;
    duty = (double)pulse * frequency / 1000000;
  } else {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(a, "duty");
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= 1)) return -EINVAL;
    duty = v->valuedouble;
  }
  bool opened = p->pwm[channel] < 0;
  if (opened) {
    int ret = claim(p, bit); if (ret) return ret;
    char path[24]; snprintf(path, sizeof(path), "/dev/pwm%d", channel);
    p->pwm[channel] = open(path, O_RDWR);
    if (p->pwm[channel] < 0) { ret = -errno; unclaim(p, bit); return ret; }
  }
  struct pwm_info_s info = {.frequency = frequency, .duty = duty >= 1 ? 65535 : (uint32_t)(duty * 65536)};
  int ret = ioctl(p->pwm[channel], PWMIOC_SETCHARACTERISTICS, (uintptr_t)&info) < 0 ? -errno : 0;
  if (!ret && ioctl(p->pwm[channel], PWMIOC_START, 0) < 0) ret = -errno;
  if (ret) { ioctl(p->pwm[channel], PWMIOC_STOP, 0); close(p->pwm[channel]); p->pwm[channel] = -1; unclaim(p, bit); }
  cJSON_AddNumberToObject(out, "pin", pwm_pins[channel]); return ret;
}

extern int qpk_ble_channel_execute(const char *,const cJSON *,cJSON *,struct qpk_hw_progress *);
extern int qpk_ble_gatt_execute(const char *,const cJSON *,cJSON *,struct qpk_hw_progress *);
static int ble_operation(struct hw_platform *p, const char *op, const cJSON *a, cJSON *out,struct qpk_hw_progress *progress)
{
  if(!strcmp(op,"channelStatus"))return qpk_ble_channel_execute(op,a,out,progress);
  struct glass_ble_state *s = malloc(sizeof(*s)); if (!s) return -ENOMEM;
  glass_ble_read(s);
  ble_prune(p,s);
  int ret = 0;
  if (!strcmp(op, "status")) {
    cJSON_AddBoolToObject(out, "ready", s->ready); cJSON_AddBoolToObject(out, "scanning", s->scanning);
    cJSON_AddBoolToObject(out, "connecting", s->connecting); cJSON_AddBoolToObject(out, "connected", s->connected);
    cJSON_AddNumberToObject(out, "error", s->error);
    cJSON_AddNumberToObject(out,"generation",s->generation);cJSON_AddNumberToObject(out,"scanGeneration",s->scan_generation);
    cJSON_AddNumberToObject(out,"mtu",s->mtu);cJSON_AddBoolToObject(out,"encrypted",s->encrypted);cJSON_AddBoolToObject(out,"bonded",s->bonded);
    cJSON_AddNumberToObject(out,"connectionCount",s->connection_count);
    cJSON_AddNumberToObject(out,"centralCapacity",s->central_capacity);
    cJSON_AddBoolToObject(out,"mousePaused",s->mouse_paused);
    cJSON_AddBoolToObject(out,"restoring",s->restoring);
    cJSON *restores=cJSON_AddArrayToObject(out,"restores");
    if(!restores){free(s);return -ENOMEM;}
    for(unsigned i=0;i<s->restore_count&&i<GLASS_BLE_CENTRAL_LIMIT;i++){
      const struct glass_ble_restore_result *r=&s->restores[i];
      cJSON *d=cJSON_CreateObject();if(!d){free(s);return -ENOMEM;}
      cJSON_AddNumberToObject(d,"generation",r->generation);
      cJSON_AddBoolToObject(d,"attempted",r->attempted);
      cJSON_AddBoolToObject(d,"connected",r->connected);
      cJSON_AddNumberToObject(d,"error",r->error);
      cJSON_AddItemToArray(restores,d);
    }
    cJSON *sessions=cJSON_AddArrayToObject(out,"sessions");
    if(!sessions){free(s);return -ENOMEM;}
    for(unsigned i=0;i<s->session_count&&i<GLASS_BLE_CENTRAL_LIMIT;i++){
      const struct glass_ble_session *v=&s->sessions[i];cJSON *d=cJSON_CreateObject();
      if(!d){free(s);return -ENOMEM;}
      cJSON_AddNumberToObject(d,"generation",v->generation);cJSON_AddBoolToObject(d,"owned",ble_owned(p,v->generation));
      cJSON_AddBoolToObject(d,"connecting",v->connecting);cJSON_AddBoolToObject(d,"connected",v->connected);
      cJSON_AddBoolToObject(d,"disconnecting",v->disconnecting);cJSON_AddBoolToObject(d,"encrypted",v->encrypted);
      cJSON_AddBoolToObject(d,"mousePaused",v->mouse_paused);
      cJSON_AddBoolToObject(d,"bonded",v->bonded);cJSON_AddNumberToObject(d,"mtu",v->mtu);
      cJSON_AddStringToObject(d,"name",v->peer.name);cJSON_AddItemToArray(sessions,d);
    }
    cJSON *list = cJSON_AddArrayToObject(out, "devices");
    if (!list) { free(s); return -ENOMEM; }
    for (unsigned i = 0; i < s->count && i < GLASS_BLE_LIMIT; i++) {
      cJSON *d = cJSON_CreateObject(); if (!d) { ret = -ENOMEM; break; }
      char address[18]; struct glass_ble_device *v = &s->devices[i];
      snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x", v->address[5],v->address[4],v->address[3],v->address[2],v->address[1],v->address[0]);
      cJSON_AddNumberToObject(d, "index", i); cJSON_AddStringToObject(d, "name", v->name); cJSON_AddStringToObject(d, "address", address);
      cJSON_AddNumberToObject(d, "addressType", v->type); cJSON_AddNumberToObject(d, "rssi", v->rssi); cJSON_AddItemToArray(list, d);
    }
  } else {
    pthread_mutex_lock(&claims_lock);
    if ((ble_owner && ble_owner != p) || (!p->ble && (s->scanning || s->session_count || s->connecting || s->connected))) ret = -EBUSY;
    else { ble_owner = p; p->ble = true; }
    pthread_mutex_unlock(&claims_lock);
    if (!ret) {
      if(!strcmp(op,"channelSend")||!strcmp(op,"channelReceive")){
        ret=qpk_ble_channel_execute(op,a,out,progress);
        if(!ret){const cJSON *epoch=cJSON_GetObjectItemCaseSensitive(a,"epoch");p->channel_epoch=epoch->valueint;}
      }else if (!strcmp(op, "start")) ret = glass_ble_start();
      else if (!strcmp(op, "scan")) {
        uint32_t accepted=0;ret=glass_ble_central_scan_session(NULL,&accepted);if(!ret)p->ble_scan=accepted;
      }else if (!strcmp(op, "connect")) {
        int index=number_arg(a,"index",-1,0,GLASS_BLE_LIMIT-1);
        int scan=number_arg(a,"scanGeneration",0,0,INT_MAX);uint32_t accepted=0;
        unsigned slot=0;while(slot<GLASS_BLE_CENTRAL_LIMIT&&p->ble_links[slot])slot++;
        ret=index<0||scan<0?-EINVAL:slot==GLASS_BLE_CENTRAL_LIMIT?-ENOSPC:
          glass_ble_central_connect_session(index,scan,&accepted);
        if(!ret){
          p->ble_links[slot]=accepted;
          cJSON_AddNumberToObject(out,"generation",accepted);
          cJSON_AddStringToObject(out,"scope","accepted_not_connected");
        }
      }else if (!strcmp(op,"cancel")) {
        if(p->ble_scan)ret=glass_ble_central_scan_stop(p->ble_scan);
        for(unsigned i=0;i<s->session_count&&i<GLASS_BLE_CENTRAL_LIMIT;i++)
          if(s->sessions[i].connecting&&ble_owned(p,s->sessions[i].generation)){
            int rc=glass_ble_central_disconnect_generation(s->sessions[i].generation);if(!ret)ret=rc;
          }
      }else{
        int generation=number_arg(a,"generation",0,0,INT_MAX);
        if(!strcmp(op,"disconnect")&&!generation){
          unsigned owned=0;
          for(unsigned i=0;i<GLASS_BLE_CENTRAL_LIMIT;i++)if(p->ble_links[i]){generation=p->ble_links[i];owned++;}
          if(owned>1)generation=-1;
        }
        if(generation<0)ret=-EINVAL;
        else if(!ble_owned(p,generation))ret=-EACCES;
        else if(!strcmp(op,"disconnect"))ret=glass_ble_central_disconnect_generation(generation);
        else ret=qpk_ble_gatt_execute(op,a,out,progress);
      }
    }
  }
  free(s); return ret;
}

int qpk_hw_platform_execute(void *opaque, const char *op, const cJSON *a, cJSON *out, struct qpk_hw_progress *progress)
{
  struct hw_platform *p = opaque;
  extern int qpk_network_status(cJSON *);
  if(!strcmp(op,"net.status"))return qpk_network_status(out);
  if (!strncmp(op,"usb.",4))return qpk_usb_execute(&p->usb,op+4,a,out,progress);
  if (!strncmp(op, "gpio.", 5)) return gpio_operation(p, op + 5, a, out);
  if (!strncmp(op, "i2c.", 4)) return i2c_operation(p, op + 4, a, out);
  if (!strncmp(op, "spi.", 4)) return spi_operation(p, op + 4, a, out);
  if (!strncmp(op, "uart.", 5)) return uart_operation(p, op + 5, a, out, progress);
  if (!strncmp(op, "pwm.", 4)) return pwm_operation(p, op + 4, a, out, false);
  if (!strncmp(op, "servo.", 6)) return pwm_operation(p, op + 6, a, out, true);
  if (!strncmp(op, "ble.", 4)) return ble_operation(p, op + 4, a, out,progress);
  if (!strncmp(op, "audio.", 6)) return qpk_hw_audio_execute_at(p->data_root,p->package, op + 6, a, out, progress);
  return -ENOSYS;
}
