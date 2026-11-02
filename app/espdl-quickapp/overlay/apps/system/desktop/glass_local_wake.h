/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "glass_local_model.h"
#ifdef __cplusplus
extern "C" {
#endif
struct glv_wake;
int glv_wake_create(struct glv_wake **);
int glv_wake_feed(struct glv_wake *,const int16_t *,size_t);
void glv_wake_reset(struct glv_wake *);
void glv_wake_destroy(struct glv_wake *);
unsigned glv_wake_evaluations(const struct glv_wake *);
float glv_wake_score(const struct glv_wake *);
#ifdef GLV_WAKE_TEST
void glv_wake_test_skew(unsigned);
#endif
#ifdef __cplusplus
}
#endif
