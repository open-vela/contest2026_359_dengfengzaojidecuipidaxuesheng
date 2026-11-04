#pragma once
#include "route_plan.h"
void matrix_init(void);
int matrix_replace(const routes_t *new_routes);
const routes_t *matrix_get(void);
int matrix_reset(void);
