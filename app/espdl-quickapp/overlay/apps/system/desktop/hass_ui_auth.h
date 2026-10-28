/* Generated from the reviewed built-in Home Assistant UI. */
#pragma once
#include "qpk_espdl.h"
#include <string.h>
static const unsigned char g_hass_ui_sha256[32] = {0x02, 0x1e, 0xfd, 0x90, 0x0b, 0x88, 0xd1, 0xe0, 0xf2, 0x3f, 0xc0, 0x14, 0xb9, 0x4c, 0xb0, 0x22, 0x46, 0xe2, 0xfe, 0x96, 0x2b, 0xa9, 0xc7, 0x24, 0x00, 0xb6, 0x42, 0x5b, 0x47, 0xf0, 0xae, 0x56};
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
