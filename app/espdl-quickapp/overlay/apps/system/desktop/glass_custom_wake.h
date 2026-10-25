/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
struct glass_custom_wake;
int glass_custom_wake_create(const char *root,struct glass_custom_wake **out);
void glass_custom_wake_destroy(struct glass_custom_wake *wake);
void glass_custom_wake_reset(struct glass_custom_wake *wake);
/* Streaming, 16kHz mono; fires after an utterance and a brief pause. */
int glass_custom_wake_feed(struct glass_custom_wake *wake,const int16_t *pcm,size_t samples);
unsigned glass_custom_wake_evaluations(const struct glass_custom_wake *wake);
float glass_custom_wake_score(const struct glass_custom_wake *wake);

/* Training-voice absolute-distance envelope, including a 50% margin. */
#define GLASS_WAKE_MAX_POSITIVE_DISTANCE 0.95f
