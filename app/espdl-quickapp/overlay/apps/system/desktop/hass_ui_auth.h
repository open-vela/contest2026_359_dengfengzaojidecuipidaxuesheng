/* Generated from the reviewed built-in Home Assistant UI. */
#pragma once
#include "qpk_espdl.h"
#include <string.h>
static const unsigned char g_hass_ui_sha256[32] = {0x5c, 0xf7, 0x9a, 0x1c, 0x8a, 0x91, 0x15, 0x38, 0x19, 0xad, 0x47, 0x57, 0xe4, 0xe8, 0x13, 0xa0, 0x77, 0x75, 0xf3, 0xa2, 0x25, 0x4d, 0x35, 0x28, 0x6b, 0xdc, 0x3d, 0x1b, 0x4f, 0x58, 0x64, 0x92};
static unsigned hass_ui_grants(const char *package, const char *source, size_t source_len)
{
  if (package == NULL || source == NULL ||
      strcmp(package, "com.openvela.homeassistant") != 0) return 0;
  unsigned char digest[32];
  qpk_dl_sha256(source, source_len, digest);
  unsigned difference = 0;
  for (unsigned i = 0; i < sizeof(digest); i++)
    difference |= digest[i] ^ g_hass_ui_sha256[i];
  memset(digest, 0, sizeof(digest));
  return difference == 0 ? HASS_READ | HASS_CONTROL | HASS_CONFIGURE : 0;
}
