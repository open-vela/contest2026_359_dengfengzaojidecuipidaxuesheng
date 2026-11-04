#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// The internal 256-bit control key is never printed or manually entered.
// It is released once to a LAN controller only while the physical pairing
// button has enabled a short, single-claim window.
void secrets_init(void);
bool secrets_is_paired(void);
const uint8_t *secrets_key(void);
esp_err_t secrets_claim_key(uint8_t result[32]);
