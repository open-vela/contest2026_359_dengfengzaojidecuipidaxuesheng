/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETSURF_OPENVELA_HTTP_NUTTX_H
#define NETSURF_OPENVELA_HTTP_NUTTX_H
#include "openvela_http.h"

struct ov_http_nuttx_config {
	/* NUL-terminated PEM bundle, size includes the NUL. Remains valid until
	 * all browser workers stop. TLS uses the desktop's glass_https service. */
	const unsigned char *ca_pem;
	size_t ca_size;
};
int ov_http_nuttx_perform(void *config, const struct ov_http_request *request);
#endif
