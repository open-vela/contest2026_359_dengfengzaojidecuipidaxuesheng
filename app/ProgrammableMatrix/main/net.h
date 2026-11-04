#pragma once
#include <stdbool.h>
#include <stdint.h>
void net_start(void);
// Opens exactly one key-claim operation for a fixed period after physical press.
void net_pair_open(unsigned duration_seconds);
void net_pair_close(void);
bool net_pair_is_open(void);
