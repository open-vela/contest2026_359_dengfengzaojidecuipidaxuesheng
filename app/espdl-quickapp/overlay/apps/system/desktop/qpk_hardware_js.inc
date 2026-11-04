/* SPDX-License-Identifier: Apache-2.0 */
#include "qpk_hardware.h"
static struct qpk_hw_session *g_qpk_hardware;
static const char *const qpk_hw_operations[] = {
  "gpio.open", "gpio.read", "gpio.write", "gpio.close",
  "i2c.open", "i2c.transfer", "i2c.close",
  "spi.open", "spi.transfer", "spi.close",
  "uart.open", "uart.read", "uart.write", "uart.close",
  "pwm.write", "pwm.stop", "servo.write", "servo.stop",
  "audio.record", "audio.play", "audio.tone", "audio.save", "audio.info", "audio.remove", "audio.list", "audio.transcribe", "audio.transcript",
  "ble.start", "ble.scan", "ble.connect", "ble.disconnect", "ble.cancel", "ble.status", "ble.services", "ble.characteristics", "ble.descriptors",
  "ble.read", "ble.write", "ble.subscribe", "ble.notification", "ble.pair", "ble.mtu", "net.status",
  "usb.list", "usb.info", "usb.mode", "usb.hidRead", "usb.hidReport",
  "usb.audioStart", "usb.audioRead", "usb.audioWrite", "usb.audioStop", "usb.cdcRead", "usb.cdcWrite",
  "usb.mount", "usb.unmount", "usb.files", "usb.readFile", "usb.writeFile",
  "jbox.discover", "jbox.pair", "jbox.connect", "jbox.disconnect", "jbox.caps", "jbox.status", "jbox.safe",
  "jbox.routeSet", "jbox.connectNodes", "jbox.removeNode", "jbox.routeClear",
  "jbox.gpioConfig", "jbox.gpioWrite", "jbox.gpioRead", "jbox.adcRead", "jbox.pwm",
  "jbox.i2cSetup", "jbox.i2cTransfer", "jbox.i2cClose",
  "jbox.spiSetup", "jbox.spiTransfer", "jbox.spiClose",
  "jbox.uartSetup", "jbox.uartWrite", "jbox.uartRead", "jbox.uartClose",
  "jbox.functionLoad", "jbox.functionCall", "jbox.functionClear"
};
static JSValue js_hw_submit(JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int magic)
{
  (void)self;
  JSValue encoded = argc ? JS_JSONStringify(c, argv[0], JS_UNDEFINED, JS_UNDEFINED) : JS_NewString(c, "{}");
  if (JS_IsException(encoded)) return encoded;
  const char *text = JS_ToCString(c, encoded);
  if (!text) { JS_FreeValue(c, encoded); return JS_EXCEPTION; }
  if (!g_qpk_hardware) g_qpk_hardware = qpk_hw_create_at(g_qpk.package,QPK_STORAGE_ROOT);
  int id = g_qpk_hardware ? qpk_hw_submit(g_qpk_hardware, qpk_hw_operations[magic], text) : -ENOMEM;
  JS_FreeCString(c, text); JS_FreeValue(c, encoded);
  if (id < 0) return JS_ThrowInternalError(c, "hardware submit: %d (%s)", id, strerror(-id));
  return JS_NewInt32(c, id);
}
static JSValue js_hw_control(JSContext *c, JSValueConst self, int argc, JSValueConst *argv, int magic)
{
  (void)self;
  if (magic == 3) {
    char *text = qpk_hw_capabilities(); if (!text) return JS_ThrowOutOfMemory(c);
    JSValue value = JS_ParseJSON(c, text, strlen(text), "hardware.capabilities"); free(text); return value;
  }
  int32_t id;
  if (argc != 1 || JS_ToInt32(c, &id, argv[0]) || id <= 0) return JS_ThrowTypeError(c, "positive task id required");
  if (!magic) {
    char *text = qpk_hw_poll(g_qpk_hardware, id);
    if (!text) return JS_ThrowInternalError(c, "hardware task not available");
    JSValue value = JS_ParseJSON(c, text, strlen(text), "hardware.poll"); free(text); return value;
  }
  int ret = qpk_hw_cancel(g_qpk_hardware, id, magic == 2);
  if (ret) return JS_ThrowInternalError(c, "hardware cancel: %d (%s)", ret, strerror(-ret));
  return JS_UNDEFINED;
}
static void qpk_hw_install(JSContext *c, JSValue system)
{
  const char *families[] = {"gpio", "i2c", "spi", "uart", "pwm", "servo", "audio", "ble", "usb", "net", "jbox"};
  for (unsigned f = 0; f < sizeof(families) / sizeof(families[0]); f++) {
    JSValue object = JS_NewObject(c); size_t length = strlen(families[f]);
    for (unsigned i = 0; i < sizeof(qpk_hw_operations) / sizeof(qpk_hw_operations[0]); i++) {
      const char *op = qpk_hw_operations[i];
      if (!strncmp(op, families[f], length) && op[length] == '.')
        JS_SetPropertyStr(c, object, op + length + 1, JS_NewCFunctionMagic(c, js_hw_submit, op + length + 1, 1, JS_CFUNC_generic_magic, i));
    }
    if (!strcmp(families[f], "audio"))
      JS_SetPropertyStr(c, object, "stop", JS_NewCFunctionMagic(c, js_hw_control, "stop", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(c, system, families[f], object);
  }
  JSValue object = JS_NewObject(c);
  const char *controls[] = {"poll", "cancel", "finish", "capabilities"};
  for (unsigned i = 0; i < 4; i++) JS_SetPropertyStr(c, object, controls[i], JS_NewCFunctionMagic(c, js_hw_control, controls[i], i == 3 ? 0 : 1, JS_CFUNC_generic_magic, i));
  JS_SetPropertyStr(c, system, "hardware", object);
}
