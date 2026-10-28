/* SPDX-License-Identifier: GPL-2.0-only */
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

extern unsigned char _browser_exec_start[], _browser_exec_end[];
static atomic_bool occupied;

/* NuttX loads a module's code/constants in one allocation. This pool belongs
 * to native code, is excluded from the user heap and has its own PMP bounds.
 * A browser module stays resident; other modules must fail instead of taking
 * executable memory from the ordinary non-executable PSRAM heap. */
void *up_textheap_memalign(size_t alignment, size_t bytes)
{
  uintptr_t start = (uintptr_t)_browser_exec_start;
  size_t capacity = (uintptr_t)_browser_exec_end - start;
  bool expected = false;
  if (!alignment || (alignment & (alignment - 1)) ||
      (start & (alignment - 1)) || !bytes || bytes > capacity ||
      !atomic_compare_exchange_strong(&occupied, &expected, true))
    return NULL;
  return (void *)start;
}

void up_textheap_free(void *ptr)
{
  if (ptr == _browser_exec_start)
    atomic_store(&occupied, false);
}

bool up_textheap_heapmember(void *ptr)
{
  return (uintptr_t)ptr >= (uintptr_t)_browser_exec_start &&
         (uintptr_t)ptr < (uintptr_t)_browser_exec_end;
}
