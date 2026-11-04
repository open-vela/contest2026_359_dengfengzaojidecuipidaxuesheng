#pragma once
#include <stdint.h>
#include <stddef.h>
#include "board.h"

typedef struct { uint64_t net[BOARD_NETS]; } routes_t;

enum {
    J_OK=0, J_BAD_ARG=-1, J_BUSY=-2, J_HARDWARE=-3,
    J_FORBIDDEN=-4, J_UNAVAILABLE=-5, J_TIMEOUT=-6,
    J_AUTH=-7, J_INTERNAL=-8, J_OVERFLOW=-9, J_NOT_READY=-10,
};

// Pure portable validation / allocator. Unit tests run without ESP-IDF.
int routes_validate(const routes_t *r);
int routes_connect(routes_t *r, unsigned a, unsigned b, int *out_net);
int routes_remove(routes_t *r, unsigned a);
int routes_net_of(const routes_t *r, unsigned a);
