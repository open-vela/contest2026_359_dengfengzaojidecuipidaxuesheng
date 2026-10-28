/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define ALBUM_FILES 256
#define ALBUM_PATH 512
#define ALBUM_NAME 128
#define ALBUM_PAGE 8
#define ALBUM_DIRECTORIES 16384
#ifndef ALBUM_ROOT
#define ALBUM_ROOT "/sdcard"
#endif
#ifndef ALBUM_LOCAL_ROOT
#define ALBUM_LOCAL_ROOT "/data/photos"
#endif
enum album_kind { ALBUM_FOLDER, ALBUM_PHOTO, ALBUM_VIDEO };
enum album_format { ALBUM_UNKNOWN, ALBUM_JPEG, ALBUM_PNG, ALBUM_BMP,
  ALBUM_HEIC, ALBUM_AVIF, ALBUM_WEBP, ALBUM_GIF, ALBUM_TIFF,
  ALBUM_MP4, ALBUM_MOV, ALBUM_MKV, ALBUM_AVI };
struct album_entry {
  char name[ALBUM_NAME];
  /* Offset of the shared directory prefix; zero means listing.path. */
  uint16_t directory;
  enum album_kind kind; time_t modified; enum album_format format;
};
struct album_listing {
  char path[ALBUM_PATH];
  unsigned count;
  int error;
  bool truncated;
  unsigned directory_bytes;
  char directories[ALBUM_DIRECTORIES];
  struct album_entry entries[ALBUM_FILES];
};
struct album_image { uint16_t *pixels; unsigned width, height; int error; };
bool local_album_valid_path(const char *path);
int local_album_kind(const char *name);
enum album_format local_album_name_format(const char *name);
enum album_format local_album_probe(const char *path);
/* Scan worker has already validated the path and lstat'ed a regular file. */
enum album_format local_album_probe_regular(const char *path);
bool local_album_entry_path(const struct album_listing *listing,unsigned index,char *path,size_t size);
int local_album_format_kind(enum album_format format);
const char *local_album_format_label(enum album_format format);
bool local_album_format_supported(enum album_format format);
/* Single, temporary worker. Returned results transfer ownership to the UI.
 * Cancellation invalidates old generations without waiting on SD I/O. */
int local_album_scan(const char *path);
/* Recursively collect one media kind, with bounded results and cancellation. */
int local_album_search(const char *path,enum album_kind kind);
int local_album_decode(const char *path, unsigned max_width, unsigned max_height);
struct album_listing *local_album_take_listing(void);
bool local_album_take_image(struct album_image *out);
void local_album_cancel(void);
