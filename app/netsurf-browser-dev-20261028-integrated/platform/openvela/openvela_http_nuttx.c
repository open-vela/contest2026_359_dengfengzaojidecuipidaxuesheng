/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_http_nuttx.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <poll.h>
#include <unistd.h>
#include <zlib.h>
#include <netutils/webclient.h>
#include "glass_https.h"

#ifndef OV_HTTP_CONNECT_MS
#define OV_HTTP_CONNECT_MS 15000
#endif
#ifndef OV_HTTP_IDLE_MS
#define OV_HTTP_IDLE_MS 60000
#endif
#ifndef OV_HTTP_TCP_MS
#define OV_HTTP_TCP_MS 5000
#endif

struct transfer {
	struct webclient_context client;
	const struct ov_http_request *request;
	bool status_sent;
	bool redirect;
	bool encoding_seen, inflate_ready, stream_end;
	enum { IDENTITY, GZIP, DEFLATE } encoding;
	z_stream inflater;
	size_t wire_bytes, decoded_bytes;

	bool secure;
	int64_t deadline_ms;
};
static void progress(struct transfer *t)
{
	/* Call after delivering to the bounded owner queue: UI backpressure is
	 * not network inactivity. TLS maintains its own socket progress clock. */
	if (!t->secure) t->deadline_ms = glass_https_milliseconds() + OV_HTTP_IDLE_MS;
}
static int emit(struct transfer *t, const uint8_t *bytes, size_t length)
{
	if (atomic_load(t->request->cancelled)) return -ECANCELED;
	if (length > OV_HTTP_MAX_BODY - t->decoded_bytes) return -EFBIG;
	t->decoded_bytes += length;
	return length ? t->request->data(t->request->sink, bytes, length) : 0;
}
static int decode(struct transfer *t, const uint8_t *bytes, size_t length)
{
	if (length > OV_HTTP_MAX_BODY - t->wire_bytes) return -EFBIG;
	t->wire_bytes += length;
	if (t->encoding == IDENTITY) return emit(t, bytes, length);
	if (!t->inflate_ready) {
		int result = inflateInit2(&t->inflater, t->encoding == GZIP ? 31 : 15);
		if (result != Z_OK) return result == Z_MEM_ERROR ? -ENOMEM : -EPROTO;
		t->inflate_ready = true;
	}
	t->inflater.next_in = (Bytef *)bytes;
	t->inflater.avail_in = (uInt)length;
	uint8_t output[OV_HTTP_CHUNK];
	for (;;) {
		if (atomic_load(t->request->cancelled)) return -ECANCELED;
		if (t->stream_end) {
			if (!t->inflater.avail_in) return 0;
			if (t->encoding != GZIP) return -EPROTO;
			/* gzip permits concatenated members; preserve the input while
			 * resetting the checksum/window state for the following member. */
			Bytef *next = t->inflater.next_in;
			uInt available = t->inflater.avail_in;
			if (inflateReset2(&t->inflater, 31) != Z_OK) return -EPROTO;
			t->inflater.next_in = next;
			t->inflater.avail_in = available;
			t->stream_end = false;
		}
		uInt before = t->inflater.avail_in;
		t->inflater.next_out = output;
		t->inflater.avail_out = sizeof(output);
		int result = inflate(&t->inflater, Z_NO_FLUSH);
		size_t made = sizeof(output) - t->inflater.avail_out;
		if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
			return result == Z_MEM_ERROR ? -ENOMEM : -EPROTO;
		int sent = emit(t, output, made);
		if (sent) return sent;
		if (result == Z_STREAM_END) t->stream_end = true;
		if (t->inflater.avail_in == before && !made) {
			if (t->inflater.avail_in) return -EPROTO;
			return 0;
		}
		if (!t->inflater.avail_in && t->inflater.avail_out) return 0;
	}
}
static int status(struct transfer *t)
{
	if (atomic_load(t->request->cancelled)) return -ECANCELED;
	if (t->status_sent) return 0;
	t->status_sent = true;
	return t->request->status(t->request->sink, t->client.http_status);
}
static int header(const char *line, bool truncated, void *ctx)
{
	struct transfer *t = ctx;
	progress(t);
	int result = status(t);
	if (result) return result;
	if (truncated) return -E2BIG;
	/* Match the existing Windows fetcher's normalized body contract:
	 * decoding/framing is handled by the transport, not by HTML or images. */
	if (!strncasecmp(line, "Content-Encoding:", 17)) {
		if (t->encoding_seen) return -ENOTSUP;
		t->encoding_seen = true;
		const char *value = line + 17;
		while (*value == ' ' || *value == '\t') value++;
		size_t length = strlen(value);
		while (length && (value[length - 1] == ' ' || value[length - 1] == '\t')) length--;
		if (length == 4 && !strncasecmp(value, "gzip", length)) t->encoding = GZIP;
		else if (length == 7 && !strncasecmp(value, "deflate", length)) t->encoding = DEFLATE;
		else if (length != 8 || strncasecmp(value, "identity", length)) return -ENOTSUP;
		return 0;
	}
	if (!strncasecmp(line, "Content-Length:", 15) || !strncasecmp(line, "Transfer-Encoding:", 18)) return 0;
	result = t->request->header(t->request->sink, line);
	if (result) return result;
	unsigned code = t->client.http_status;
	if (!strncasecmp(line, "Location:", 9) &&
	    (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)) {
		/* NetSurf applies redirect/cookie/method policy on its owner thread.
		 * Stop webclient before its automatic redirect path runs. */
		t->redirect = true;
		return -ECANCELED;
	}
	return 0;
}
static int data(char **buffer, int offset, int end, int *capacity, void *ctx)
{
	(void)capacity;
	struct transfer *t = ctx;
	int result = status(t);
	if (result) return result;
	if (offset < 0 || end < offset) return -EPROTO;
	result = decode(t, (uint8_t *)*buffer + offset, (size_t)(end - offset));
	if (!result && end > offset) progress(t);
	return result;
}

static bool retry_connection(const struct transfer *t,
		const struct glass_https_diagnostic *diagnostic, int result)
{
	/* Only retry a GET whose TLS connector failed before any HTTP send.
	 * Never replay POST, partial responses, certificate failures or cancellation. */
	/* The existing connector classifies net BIO failures as EPROTO. The
	 * mbedTLS NET_SEND_FAILED/NET_RECV_FAILED/NET_CONN_RESET codes identify
	 * transport failures, not malformed TLS records. Only the pre-HTTP
	 * handshake may retry these; any verification failure still forbids it.
	 * Values are the public mbedTLS net_sockets.h ABI used by this loader. */
	bool bio_error = result == -EPROTO && diagnostic->stage &&
		!strcmp(diagnostic->stage, "handshake") &&
		(diagnostic->tls_error == -0x004e || diagnostic->tls_error == -0x004c ||
		 diagnostic->tls_error == -0x0050) &&
		(!diagnostic->verify_flags || diagnostic->verify_flags == UINT32_MAX);
	return !t->request->is_post && !atomic_load(t->request->cancelled) &&
		!t->status_sent && !t->wire_bytes && diagnostic->stage &&
		(!strcmp(diagnostic->stage, "tcp") || !strcmp(diagnostic->stage, "handshake")) &&
		(result == -ETIMEDOUT || result == -ECONNRESET || result == -ECONNREFUSED || bio_error);
}

static void configure_client(struct transfer *t, char *buffer, size_t buffer_size,
		struct glass_https_bounded_request *tls, bool secure)
{
	t->client = (struct webclient_context){0};
	webclient_set_defaults(&t->client);
	t->client.url = t->request->url;
	t->client.method = t->request->is_post ? "POST" : "GET";
	t->client.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
	t->client.headers = t->request->headers;
	t->client.nheaders = t->request->header_count;
	t->client.timeout_sec = 10;
	t->client.buflen = buffer_size;
	t->client.buffer = buffer;
	t->client.header_callback = header;
	t->client.header_callback_arg = t;
	t->client.sink_callback = data;
	t->client.sink_callback_arg = t;
	if (!secure) t->client.flags |= WEBCLIENT_FLAG_NON_BLOCKING;
	if (secure) {
		t->client.tls_ops = glass_https_bounded_tls_ops();
		t->client.tls_ctx = tls;
	}
	if (t->request->is_post)
		webclient_set_static_body(&t->client, t->request->post, t->request->post_length);
}

static int perform_plain(struct transfer *t)
{
	for (;;) {
		int result = webclient_perform(&t->client);
		if (result != -EAGAIN) return result;
		/* Only an EAGAIN context may be aborted. Every timeout, cancellation
		 * and poll failure below releases the socket/state exactly once. */
		struct webclient_poll_info info;
		result = webclient_get_poll_info(&t->client, &info);
		if (!result) {
			struct pollfd pfd = {.fd = info.fd};
			if (info.flags & WEBCLIENT_POLL_INFO_WANT_READ) pfd.events |= POLLIN;
			if (info.flags & WEBCLIENT_POLL_INFO_WANT_WRITE) pfd.events |= POLLOUT;
			if (!pfd.events) result = -EPROTO;
			while (!result) {
				if (atomic_load(t->request->cancelled)) { result = -ECANCELED; break; }
				int64_t left = t->deadline_ms - glass_https_milliseconds();
				if (left <= 0) { result = -ETIMEDOUT; break; }
				int ready = poll(&pfd, 1, left > 100 ? 100 : (int)left);
				if (ready < 0 && errno == EINTR) continue;
				if (ready < 0) { result = -errno; break; }
				if (ready && (pfd.revents & POLLNVAL)) { result = -EBADF; break; }
				/* HUP/ERR must go through webclient to consume buffered bytes
				 * or obtain the socket's actual connection/read error. */
				if (ready) break;
			}
		}
		if (result) { webclient_abort(&t->client); return result; }
	}
}
int ov_http_nuttx_perform(void *ctx, const struct ov_http_request *request)
{
	const struct ov_http_nuttx_config *config = ctx;
	if (!request || !request->cancelled || !config) return -EINVAL;
	bool secure = !strncasecmp(request->url, "https://", 8);
	if (!secure && strncasecmp(request->url, "http://", 7)) return -EINVAL;
	if (atomic_load(request->cancelled)) return -ECANCELED;
	if (secure && (!config->ca_pem || !config->ca_size ||
	    config->ca_pem[config->ca_size - 1] != 0)) return -EACCES;
#ifdef OV_HTTP_NETWORK_TRACE
	int64_t started = glass_https_milliseconds();
	const char *host = strstr(request->url, "://") + 3;
	size_t host_length = strcspn(host, "/?#");
	/* Never include credentials, path, query, request/response body or input. */
	const char *at = memchr(host, '@', host_length);
	if (at) { host_length -= (size_t)(at + 1 - host); host = at + 1; }
	if (host_length > 255) host_length = 255;
	fprintf(stderr, "[browser-net] start host=%.*s secure=%d\n", (int)host_length, host, secure);
#endif
	struct transfer t = {.request = request, .secure = secure};
	struct glass_https_diagnostic diagnostic = {0};
	struct glass_https_bounded_request tls = {.request = {
		.ca_pem = config->ca_pem, .ca_size = config->ca_size,
		.abort_flag = request->cancelled,
		/* Reuse glass_https's existing progress-based receive deadline, as
		 * WinHTTP does: a healthy stream must not expire 30 s after opening.
		 * The connector keeps a separate initial 15 s budget; all TLS waits
		 * still check cancellation every 100 ms. Body quotas remain enforced. */
		.idle_timeout_ms = OV_HTTP_IDLE_MS,
		.diagnostic = &diagnostic,
	}, .connect_timeout_ms = OV_HTTP_TCP_MS};
	/* webclient serialises the whole request into this buffer. Reserve for
	 * accepted request headers as well as URL/Host and its fixed fields. */
	size_t buffer_size = strlen(request->url) + 4096u;
	if (buffer_size > 32768u) return -E2BIG;
	if (request->header_count > OV_HTTP_MAX_HEADERS) return -E2BIG;
	for (size_t i = 0; i < request->header_count; i++) {
		size_t bytes = strlen(request->headers[i]) + 2u;
		if (bytes > 32768u - buffer_size) return -E2BIG;
		buffer_size += bytes;
	}
	if (buffer_size < 8192u) buffer_size = 8192u;
	char *buffer = malloc(buffer_size);
	if (!buffer) return -ENOMEM;
	int result;
	int64_t connect_end = glass_https_milliseconds() + 2 * OV_HTTP_CONNECT_MS;
	for (unsigned attempt = 0; ; attempt++) {
		if (attempt && glass_https_milliseconds() >= connect_end) break;
		diagnostic = (struct glass_https_diagnostic){0};
		tls.request.deadline_ms = glass_https_milliseconds() + OV_HTTP_CONNECT_MS;
		if (tls.request.deadline_ms > connect_end) tls.request.deadline_ms = connect_end;
		t.deadline_ms = tls.request.deadline_ms;
		configure_client(&t, buffer, buffer_size, &tls, secure);
		result = secure ? webclient_perform(&t.client) : perform_plain(&t);
#ifdef OV_HTTP_NETWORK_TRACE
		fprintf(stderr, "[browser-net] end host=%.*s attempt=%u result=%d status=%u wire=%lu decoded=%lu elapsed_ms=%lld stage=%s\n",
			(int)host_length, host, attempt + 1, result, t.client.http_status,
			(unsigned long)t.wire_bytes, (unsigned long)t.decoded_bytes,
			(long long)(glass_https_milliseconds() - started), diagnostic.stage ? diagnostic.stage : "http");
#endif
		if (!secure || attempt >= 2 || !retry_connection(&t, &diagnostic, result)) break;
		/* A fresh TCP flow may survive a transient drop. Bound retries by
		 * both attempt count and the original aggregate connection budget;
		 * small cancellable backoff avoids immediate refusal loops. */
		int64_t backoff = glass_https_milliseconds() + (attempt + 1) * 250;
		if (backoff >= connect_end) break;
		while (glass_https_milliseconds() < backoff && !atomic_load(request->cancelled))
			usleep(50000);
		if (atomic_load(request->cancelled)) { result = -ECANCELED; break; }
	}
	if (t.redirect && !atomic_load(request->cancelled)) result = 0;
	if (!result) result = status(&t);
	if (!result && !t.redirect && t.encoding != IDENTITY && !t.stream_end &&
	    t.client.http_status != 204 && t.client.http_status != 304) result = -EPROTO;
	if (secure && result)
		fprintf(stderr, "[browser-tls] stage=%s result=%d tls=%d verify=%lx\n",
			diagnostic.stage ? diagnostic.stage : "request", result,
			diagnostic.tls_error, (unsigned long)diagnostic.verify_flags);
	/* Both paths have released their webclient state before returning. */
	free(buffer);
	if (t.inflate_ready) inflateEnd(&t.inflater);
	return result;
}
