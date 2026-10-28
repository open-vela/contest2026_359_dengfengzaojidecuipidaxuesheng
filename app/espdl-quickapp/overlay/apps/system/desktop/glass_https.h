/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <netutils/webclient.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

struct glass_https_diagnostic {
  const char *stage;
  int transport_error, tls_error;
  uint32_t verify_flags;
  unsigned session_tickets;
};

/* One request owns its deadline/cancellation/diagnostics. Trust material stays
 * alive for the request; no credentials or request text belong in this struct. */
struct glass_https_request {
  const unsigned char *ca_pem;
  size_t ca_size;
  int64_t deadline_ms;
  /* Optional inactivity deadline, renewed by successful application reads.
   * Zero preserves the fixed deadline used by weather and other callers. */
  uint32_t idle_timeout_ms;
  atomic_bool *abort_flag;
  struct glass_https_diagnostic *diagnostic;
};

int glass_https_initialize(void);
int glass_https_validate_ca(const unsigned char *pem,size_t size);
int64_t glass_https_milliseconds(void);
const struct webclient_tls_ops *glass_https_tls_ops(void);
/* WebSocket users need readiness without entering a blocking TLS read. */
int glass_https_poll(struct webclient_tls_connection *connection,
                     short events, int timeout_ms);
void glass_https_set_deadline(struct webclient_tls_connection *connection,
                              int64_t deadline_ms);
void glass_https_set_abort_flag(struct webclient_tls_connection *connection,atomic_bool *flag);

/* Opt-in extension: original request layout and TLS ops remain unchanged. */
struct glass_https_bounded_request {
  struct glass_https_request request;
  uint32_t connect_timeout_ms;
};
const struct webclient_tls_ops *glass_https_bounded_tls_ops(void);
