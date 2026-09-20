############################################################################
# apps/wireless/bluetooth/nimble/Makefile
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0.
############################################################################

include $(APPDIR)/Make.defs

NIMBLE_UNPACKDIR = mynewt-nimble
NIMBLE_ROOT = $(APPDIR)/wireless/bluetooth/nimble/$(NIMBLE_UNPACKDIR)
CONFIG_NIMBLE_REF := $(patsubst "%",%,$(strip $(CONFIG_NIMBLE_REF)))
NIMBLE_TAR := $(CONFIG_NIMBLE_REF).tar.gz
NIMBLE_URL := https://github.com/apache/mynewt-nimble/archive/$(NIMBLE_TAR)

ifneq ($(CONFIG_NIMBLE_PORTING_EXAMPLE),)
-include $(NIMBLE_ROOT)/porting/examples/nuttx/Make.defs
PRIORITY  = 255
STACKSIZE = $(CONFIG_NIMBLE_PORTING_EXAMPLE_STACKSIZE)
CFLAGS += -Wno-pointer-to-int-cast -Wno-undef
endif

$(NIMBLE_TAR):
	$(Q) curl -L $(NIMBLE_URL) -o $(NIMBLE_TAR)

$(NIMBLE_UNPACKDIR): $(NIMBLE_TAR)
	$(Q) tar zxf $(NIMBLE_TAR)
	$(Q) mv mynewt-nimble-$(CONFIG_NIMBLE_REF) $(NIMBLE_UNPACKDIR)
	$(Q) touch $(NIMBLE_UNPACKDIR)

ifeq ($(wildcard $(NIMBLE_UNPACKDIR)/.git),)
context:: $(NIMBLE_UNPACKDIR)

distclean::
	$(call DELFILE,$(NIMBLE_TAR))
	$(call DELDIR,$(NIMBLE_UNPACKDIR))
endif

ifeq ($(CONFIG_SYSTEM_GLASS_BLE),y)
include $(NIMBLE_ROOT)/porting/examples/nuttx/Make.defs
CSRCS := $(filter-out %/porting/examples/nuttx/ble.c,$(CSRCS))
CSRCS += glass_ble.c glass_ble_hid.c glass_ble_store.c mouse_core.c
MAINSRC =
PROGNAME =
CFLAGS += -I$(APPDIR)/system/c6ble
CFLAGS += -DMYNEWT_VAL_BLE_ROLE_CENTRAL=1 -DMYNEWT_VAL_BLE_ROLE_OBSERVER=1
CFLAGS += -Wno-pointer-to-int-cast -Wno-undef
endif

include $(APPDIR)/Application.mk
