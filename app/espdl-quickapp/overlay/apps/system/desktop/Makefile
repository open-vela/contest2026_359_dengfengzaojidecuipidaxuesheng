############################################################################
# apps/system/desktop/Makefile
############################################################################

include $(APPDIR)/Make.defs
ifeq ($(CONFIG_SYSTEM_C6_DESKTOP),y)
CFLAGS += -I$(APPDIR)/system/c6probe
endif


PROGNAME  = $(CONFIG_SYSTEM_DESKTOP_PROGNAME)
PRIORITY  = $(CONFIG_SYSTEM_DESKTOP_PRIORITY)
STACKSIZE = $(CONFIG_SYSTEM_DESKTOP_STACKSIZE)

CFLAGS += -I$(APPDIR)/interpreters/quickjs/quickjs
CFLAGS += -I$(APPDIR)/system/espclaw/include

CSRCS   = desktop_font.c qpk_runtime.c qpk_homeassistant.c ouo_resource.c \
          camera_resource.c recorder_resource.c
MAINSRC = desktop_main.c

CSRCS += glass_file_service.c qpk_storage.c qpk_storage_selftest.c
CSRCS += glass_qpk_builder.c
CSRCS += qpk_hardware.c qpk_hw_platform.c qpk_hw_audio.c qpk_audio_store.c qpk_hw_probe.c
CFLAGS += -I$(TOPDIR)/arch/risc-v/src/common

CSRCS += glass_system_monitor.c
CSRCS += glass_system_platform.c

CSRCS += glass_settings_service.c
CSRCS += glass_weather.c glass_weather_parse.c glass_weather_https.c glass_weather_http.c glass_https.c
# Dependency generation needs the same headers as compilation.
CFLAGS += -I$(APPDIR)/netutils/cjson/cJSON $(ESPCLAW_TLS_CFLAGS)

CSRCS += qpk_tjpgd.c qpk_mjpeg.c qpk_camera_probe.c
CSRCS += espdl_resource.c qpk_espdl_service.c qpk_espdl_store.c
CSRCS += qpk_espdl_sha256.c qpk_espdl_track.c qpk_espdl_capture.c
CSRCS += qpk_espdl_probe.c

# Keep full JPEG quality; optimize only the pixel/entropy decoding hot paths.
qpk_tjpgd.c_CFLAGS += -O2
qpk_mjpeg.c_CFLAGS += -O2

CSRCS += glass_music_parse.c glass_music_http.c glass_music_service.c glass_music_pcm.c glass_ime.c glass_voice.c
CSRCS += glass_pinyin_resource.c
CSRCS += glass_portal_config.c glass_portal_files.c glass_portal_http.c glass_portal_resource.c
CSRCS += glass_chat_service.c glass_chat_store.c
CSRCS += pet_engine.c pet_sprites.c qpk_pet.c pet_lvgl.c dafeiyu_resource.c
CXXSRCS += glass_pinyin.cpp $(wildcard music_vendor/googlepinyin/src/*.cpp)
CXXEXT = .cpp
CXXFLAGS += -std=gnu++11
glass_music_service.c_CFLAGS += -O2

CSRCS += homeassistant_resource.c
ifeq ($(CONFIG_SYSTEM_HASS),y)
CSRCS += hass_qjs.c hass_portal.c
CFLAGS += -I$(APPDIR)/system/hass
endif

include $(APPDIR)/Application.mk
