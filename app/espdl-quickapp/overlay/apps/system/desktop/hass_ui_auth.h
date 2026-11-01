/* Generated from the reviewed built-in Home Assistant UI. */
#pragma once
#include "qpk_espdl.h"
#include <string.h>
static const unsigned char g_hass_ui_sha256[32] = {0xfc, 0xa6, 0xc2, 0xff, 0xf9, 0xc5, 0x1d, 0xc4, 0xc3, 0x4e, 0x2c, 0xef, 0x3f, 0x8e, 0x06, 0x04, 0xd5, 0xa1, 0xb6, 0xc7, 0xb0, 0xaa, 0x4d, 0xf8, 0xd5, 0xcf, 0x5e, 0x42, 0x0c, 0x81, 0xf9, 0xc9};
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
