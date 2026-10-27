/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define QPK_PHOTO_COPY_MAX (2u * 1024u * 1024u)
enum qpk_photo_owner { QPK_PHOTO_PREVIEW, QPK_PHOTO_ASSISTANT, QPK_PHOTO_OWNERS };
enum qpk_photo_format { QPK_PHOTO_JPEG, QPK_PHOTO_BMP565, QPK_PHOTO_BMP24,
                        QPK_PHOTO_JPEG565 };
struct qpk_photo_input {
  enum qpk_photo_format format;
  const void *data;
  size_t bytes;
  unsigned width, height, stride; /* RGB565 input; JPEG ignores these. */
  unsigned zoom, style;
  unsigned aspect,image_width,image_height; /* Zero keeps the original canvas. */
  const char *root_override; /* Host fixtures only; NULL selects mounted storage. */
};
struct qpk_photo_result {
  uint32_t ticket;
  bool done;
  int error;
  uint32_t copy_us, write_us;
  char path[128];
};
/* Initialize from the permanent desktop task group, never an NSH caller. */
int qpk_photo_store_initialize(void);
int qpk_photo_store_submit(enum qpk_photo_owner owner,
                            const struct qpk_photo_input *input, uint32_t *ticket);
int qpk_photo_store_status(enum qpk_photo_owner owner, uint32_t ticket,
                            struct qpk_photo_result *result);
void qpk_photo_store_cancel(enum qpk_photo_owner owner, uint32_t ticket);
void qpk_photo_store_shutdown(void);
