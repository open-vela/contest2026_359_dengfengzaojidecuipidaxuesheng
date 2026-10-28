/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_HTTP_H
#define NETSURF_OPENVELA_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "utils/errors.h"

#define OV_HTTP_CHUNK 1024u
#define OV_HTTP_SLOTS 8u
#define OV_HTTP_MAX_URL 2048u
#define OV_HTTP_MAX_BODY (4u * 1024u * 1024u)
#define OV_HTTP_MAX_POST (64u * 1024u)
#define OV_HTTP_MAX_HEADERS 24u

/* The worker owns this immutable request until perform returns. It must never
 * call NetSurf or LVGL. Callbacks return a negative errno on cancellation or
 * resource exhaustion; the transport must stop and release its connection. */
struct ov_http_request {
	const char *url;
	const char *const *headers;
	size_t header_count;
	const uint8_t *post;
	size_t post_length;
	bool is_post;
	atomic_bool *cancelled;
	void *sink;
	int (*status)(void *sink, unsigned status);
	int (*header)(void *sink, const char *line);
	int (*data)(void *sink, const uint8_t *data, size_t length);
};

struct ov_http_transport {
	/* Blocking worker call. Enforce TLS chain/hostname verification, finite
	 * timeouts and cancellation; deliver redirects without following them. */
	int (*perform)(void *ctx, const struct ov_http_request *request);
	void *ctx;
};

/* Owner thread, after netsurf_init and before opening any HTTP(S) window.
 * The transport and its trust store must outlive netsurf_exit(). */
nserror ov_http_register(const struct ov_http_transport *transport);
/* Cancel/join workers before netsurf_exit; only call on the core owner. */
void ov_http_stop(void);

#endif
