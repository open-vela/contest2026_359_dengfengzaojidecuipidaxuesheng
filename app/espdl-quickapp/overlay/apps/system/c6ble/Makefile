include $(APPDIR)/Make.defs
PROGNAME = c6ble
PRIORITY = 100
STACKSIZE = 8192
MODULE = $(CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL)
CFLAGS += -I$(APPDIR)/system/c6probe
CFLAGS += -I$(APPDIR)/wireless/bluetooth/nimble
MAINSRC = ble_main.c
CSRCS = ble_driver.c ble_hosted.c ble_socket_register.c
include $(APPDIR)/Application.mk
