/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define JBOX_SCRIPT_SOURCE_MAX 32768
#define JBOX_SCRIPT_CODE_MAX 4096
struct jbox_script_program {
  uint8_t code[JBOX_SCRIPT_CODE_MAX];
  unsigned count, arguments;
  char name[64], parameters[4][64];
  size_t error_offset;
};
/* Bounded data parser: never executes source or accesses hardware. */
int jbox_script_compile(const char *source, struct jbox_script_program *out);
const char *jbox_script_example(const char *name);
