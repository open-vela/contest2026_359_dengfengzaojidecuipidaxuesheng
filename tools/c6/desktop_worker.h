/* SPDX-License-Identifier: Apache-2.0 */
#ifndef C6_DESKTOP_WORKER_H
#define C6_DESKTOP_WORKER_H

#include <pthread.h>
#include <stdbool.h>
#include "scan_results.h"

enum c6_desktop_operation
{
  C6_DESKTOP_SCAN = 1, C6_DESKTOP_CONNECT,
  C6_DESKTOP_STATUS, C6_DESKTOP_DISCONNECT
};
struct c6_desktop_link
{
  bool initialized;
  bool associated;
  bool carrier_ready;
  bool ipv4_ready;
  uint8_t ipv4[4];
};
struct c6_desktop_result
{
  unsigned long sequence;
  enum c6_desktop_operation operation;
  bool busy;
  int error;
  struct c6_desktop_link link;
  size_t count;
  struct c6_scan_ap records[C6_SCAN_LIMIT];
};

struct c6_desktop_backend
{
  int (*scan)(struct c6_scan_ap *, size_t, size_t *);
  /* Zero means request accepted, not association or IP acquisition. */
  int (*connect)(const char *, const char *);
  /* Optional callbacks. Missing capability returns -ENOTSUP, never success.
   * Status must use observed link/IP state, not the last accepted command. */
  int (*status)(struct c6_desktop_link *);
  int (*disconnect)(void);
};

struct c6_desktop_worker
{
  pthread_mutex_t lock;
  pthread_cond_t wake;
  pthread_t thread;
  struct c6_desktop_backend backend;
  struct c6_desktop_result result;
  bool stopping;
  bool queued;
  char ssid[33];
  char password[65];
};

/* Owner initializes/stops once. Stop must not run on the UI thread: it waits
 * for the backend to return. Backend operations must have bounded timeouts.
 * No public calls may overlap stop; closing a page only drops UI references.
 */
int c6_desktop_worker_start(struct c6_desktop_worker *worker,
                            const struct c6_desktop_backend *backend);
int c6_desktop_worker_submit(struct c6_desktop_worker *worker,
                             enum c6_desktop_operation operation,
                             const char *ssid, const char *password);
int c6_desktop_worker_read(struct c6_desktop_worker *worker,
                           struct c6_desktop_result *result);
int c6_desktop_worker_stop(struct c6_desktop_worker *worker);
#endif
