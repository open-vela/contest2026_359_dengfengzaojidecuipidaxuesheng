/* SPDX-License-Identifier: GPL-2.0-only */
#include "openvela_http.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"
#include "utils/nsurl.h"

enum chunk_type { HTTP_STATUS, HTTP_HEADER, HTTP_DATA };
/* Request headers are not response body chunks. Site cookies can exceed 1KiB. */
#define REQUEST_HEADER_MAX 8192u
#define REQUEST_HEADERS_MAX 16384u
struct chunk {
	enum chunk_type type;
	size_t length;
	unsigned status;
	uint8_t bytes[OV_HTTP_CHUNK];
};
struct request {
	struct request *next;
	struct fetch *parent;
	struct ov_http_request io;
	char *headers[OV_HTTP_MAX_HEADERS];
	size_t header_bytes;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t space;
	/* NetSurf owns the pending queue. Only running workers need body slots. */
	struct chunk *chunks;
	size_t head, count, bytes;
	atomic_bool cancelled;
	bool started, done, only_2xx;
	int result;
	unsigned status;
	char location[OV_HTTP_MAX_URL];
};
static struct request *requests;
static struct ov_http_transport transport;
static bool polling;
static unsigned workers;

static int push(struct request *r, const struct chunk *chunk)
{
	pthread_mutex_lock(&r->lock);
	while (r->count == OV_HTTP_SLOTS && !atomic_load(&r->cancelled))
		pthread_cond_wait(&r->space, &r->lock);
	if (atomic_load(&r->cancelled)) {
		pthread_mutex_unlock(&r->lock);
		return -ECANCELED;
	}
	r->chunks[(r->head + r->count++) % OV_HTTP_SLOTS] = *chunk;
	pthread_mutex_unlock(&r->lock);
	return 0;
}
static int receive_status(void *ctx, unsigned status)
{
	if (status < 200 || status > 599) return -EPROTO;
	return push(ctx, &(struct chunk){.type = HTTP_STATUS, .status = status});
}
static int receive_header(void *ctx, const char *line)
{
	size_t length = strlen(line);
	if (length > OV_HTTP_CHUNK - 3u || strchr(line, '\r') || strchr(line, '\n'))
		return -E2BIG;
	struct chunk chunk = {.type = HTTP_HEADER, .length = length + 2};
	memcpy(chunk.bytes, line, length);
	memcpy(chunk.bytes + length, "\r\n", 3);
	return push(ctx, &chunk);
}
static int receive_data(void *ctx, const uint8_t *data, size_t length)
{
	struct request *r = ctx;
	if (length > OV_HTTP_MAX_BODY - r->bytes) return -EFBIG;
	r->bytes += length;
	while (length) {
		struct chunk chunk = {.type = HTTP_DATA};
		chunk.length = length < OV_HTTP_CHUNK ? length : OV_HTTP_CHUNK;
		memcpy(chunk.bytes, data, chunk.length);
		int result = push(r, &chunk);
		if (result) return result;
		data += chunk.length;
		length -= chunk.length;
	}
	return 0;
}
static void *worker(void *ctx)
{
	struct request *r = ctx;
	int result = transport.perform(transport.ctx, &r->io);
	pthread_mutex_lock(&r->lock);
	r->result = result;
	r->done = true;
	pthread_mutex_unlock(&r->lock);
	return NULL;
}
static void cancel_request(void *ctx)
{
	struct request *r = ctx;
	atomic_store(&r->cancelled, true);
	pthread_mutex_lock(&r->lock);
	if (!r->started) { r->done = true; r->result = -ECANCELED; }
	pthread_cond_broadcast(&r->space);
	pthread_mutex_unlock(&r->lock);
}
static void free_request(void *ctx)
{
	struct request *r = ctx;
	cancel_request(r);
	if (r->started) {
		pthread_join(r->thread, NULL);
		workers--;
	}
	struct request **link = &requests;
	while (*link && *link != r) link = &(*link)->next;
	if (*link) *link = r->next;
	for (size_t i = 0; i < r->io.header_count; i++) free(r->headers[i]);
	free((void *)r->io.url);
	free((void *)r->io.post);
	free(r->chunks);
	pthread_cond_destroy(&r->space);
	pthread_mutex_destroy(&r->lock);
	free(r);
}
static bool add_header(struct request *r, const char *line)
{
	size_t length = strlen(line);
	if (r->io.header_count == OV_HTTP_MAX_HEADERS ||
	    length > REQUEST_HEADER_MAX || length + 2u > REQUEST_HEADERS_MAX - r->header_bytes ||
	    strchr(line, '\r') || strchr(line, '\n'))
		return false;
	char *copy = strdup(line);
	if (!copy) return false;
	r->headers[r->io.header_count++] = copy;
	r->header_bytes += length + 2u;
	return true;
}
static void *setup(struct fetch *parent, struct nsurl *url, bool only_2xx,
		bool downgrade_tls, const char *post, size_t post_length, bool raw_post,
		const struct fetch_multipart_data *multipart, const char **headers)
{
	(void)downgrade_tls;
	if (multipart || strlen(nsurl_access(url)) >= OV_HTTP_MAX_URL ||
	    post_length > OV_HTTP_MAX_POST) return NULL;
	struct request *r = calloc(1, sizeof(*r));
	if (!r) return NULL;
	if (pthread_mutex_init(&r->lock, NULL)) { free(r); return NULL; }
	if (pthread_cond_init(&r->space, NULL)) {
		pthread_mutex_destroy(&r->lock); free(r); return NULL;
	}
	atomic_init(&r->cancelled, false);
	r->parent = parent;
	r->only_2xx = only_2xx;
	r->io = (struct ov_http_request){
		.url = strdup(nsurl_access(url)), .headers = (const char *const *)r->headers,
		.cancelled = &r->cancelled, .sink = r, .status = receive_status,
		.header = receive_header, .data = receive_data, .is_post = post != NULL,
	};
	if (!r->io.url) goto fail;
	if (post) {
		if (!raw_post) post_length = strlen(post);
		if (post_length > OV_HTTP_MAX_POST) goto fail;
		uint8_t *body = malloc(post_length ? post_length : 1);
		if (!body) goto fail;
		memcpy(body, post, post_length);
		r->io.post = body;
		r->io.post_length = post_length;
		if (!raw_post && !add_header(r, "Content-Type: application/x-www-form-urlencoded")) goto fail;
	}
	if (!add_header(r, "Accept-Encoding: gzip, deflate")) goto fail;
	if (headers) for (size_t i = 0; headers[i]; i++) {
		if (!strncasecmp(headers[i], "Accept-Encoding:", 16)) continue;
		if (!add_header(r, headers[i])) goto fail;
	}
	char *cookie = urldb_get_cookie(url, true);
	if (cookie) {
		size_t bytes = strlen(cookie);
		if (bytes > REQUEST_HEADER_MAX - 8u) { free(cookie); goto fail; }
		char *line = malloc(bytes + 9u);
		if (!line) { free(cookie); goto fail; }
		memcpy(line, "Cookie: ", 8);
		memcpy(line + 8, cookie, bytes + 1u);
		free(cookie);
		bool added = add_header(r, line);
		free(line);
		if (!added) goto fail;
	}
	r->next = requests;
	requests = r;
	return r;
fail:
	free_request(r);
	return NULL;
}
static bool start(void *ctx)
{
	struct request *r = ctx;
	if (r->started) return true;
	if (workers >= 2) return false;
	/* setup() returning NULL is a permanent BAD_URL in the core, not a
	 * backpressure signal. Keep queued metadata, and let start() defer the
	 * bounded two-worker buffers until a slot and memory are available. */
	r->chunks = calloc(OV_HTTP_SLOTS, sizeof(*r->chunks));
	if (!r->chunks) return false;
	pthread_attr_t attr;
	if (pthread_attr_init(&attr)) { free(r->chunks); r->chunks = NULL; return false; }
	int error = pthread_attr_setstacksize(&attr, 64u * 1024u);
	if (!error) error = pthread_create(&r->thread, &attr, worker, r);
	pthread_attr_destroy(&attr);
	if (error) { free(r->chunks); r->chunks = NULL; return false; }
	r->started = true;
	workers++;
	return true;
}
static bool initialise(lwc_string *scheme) { (void)scheme; return true; }
static bool acceptable(const struct nsurl *url)
{
	lwc_string *host = nsurl_get_component(url, NSURL_HOST);
	bool valid = host != NULL;
	if (host) lwc_string_unref(host);
	return valid;
}
static bool redirect(unsigned status)
{
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
static void deliver(struct request *r, const struct chunk *chunk)
{
	if (chunk->type == HTTP_STATUS) {
		r->status = chunk->status;
		fetch_set_http_code(r->parent, (http_response_code)r->status);
		return;
	}
	if (chunk->type == HTTP_HEADER) {
		const char *line = (const char *)chunk->bytes;
		if (!strncasecmp(line, "Location:", 9)) {
			const char *value = line + 9;
			while (*value == ' ' || *value == '\t') value++;
			size_t len = strcspn(value, "\r\n");
			memcpy(r->location, value, len);
			r->location[len] = 0;
		}
		if (!strncasecmp(line, "Set-Cookie:", 11)) fetch_set_cookie(r->parent, line + 11);
	}
	if (chunk->type == HTTP_DATA && (redirect(r->status) || r->status == 304 ||
	    (r->only_2xx && r->status / 100 != 2))) return;
	fetch_msg msg = {.type = chunk->type == HTTP_HEADER ? FETCH_HEADER : FETCH_DATA};
	msg.data.header_or_data.buf = chunk->bytes;
	msg.data.header_or_data.len = chunk->length;
	fetch_send_callback(&msg, r->parent);
}
static void finish(struct request *r)
{
	if (!atomic_load(&r->cancelled)) {
		fetch_msg msg = {0};
		if (r->result || !r->status) {
			msg.type = r->result == -ETIMEDOUT ? FETCH_TIMEDOUT : FETCH_ERROR;
			msg.data.error = r->result == -EACCES ? "TLS certificate verification failed" :
				r->result == -EFBIG ? "Response exceeds memory limit" :
				r->result == -ENOTSUP ? "Unsupported response compression" :
				r->result == -EPROTO ? "Invalid or incomplete HTTP response" : "HTTP transport failed";
		} else if (redirect(r->status) && r->location[0]) {
			msg.type = FETCH_REDIRECT;
			msg.data.redirect = r->location;
		} else if (r->status == 304) {
			msg.type = FETCH_NOTMODIFIED;
		} else if (r->only_2xx && r->status / 100 != 2) {
			msg.type = FETCH_ERROR;
			msg.data.error = "HTTP response was not successful";
		} else msg.type = FETCH_FINISHED;
		fetch_send_callback(&msg, r->parent);
	}
	fetch_remove_from_queues(r->parent);
	fetch_free(r->parent);
}
static void poll_requests(lwc_string *scheme)
{
	(void)scheme;
	if (polling) return;
	polling = true;
	struct request *r = requests;
	while (r) {
		/* Callbacks may create requests; keep the current node alive until all
		 * its notifications finish. Abort only sets an atomic flag. */
		struct request *next = r->next;
		for (unsigned i = 0; i < OV_HTTP_SLOTS; i++) {
			struct chunk chunk;
			pthread_mutex_lock(&r->lock);
			bool has_chunk = r->count != 0;
			if (has_chunk) {
				chunk = r->chunks[r->head];
				r->head = (r->head + 1) % OV_HTTP_SLOTS;
				r->count--;
				pthread_cond_signal(&r->space);
			}
			pthread_mutex_unlock(&r->lock);
			if (!has_chunk) break;
			if (!atomic_load(&r->cancelled)) deliver(r, &chunk);
		}
		pthread_mutex_lock(&r->lock);
		bool done = r->done && r->count == 0;
		pthread_mutex_unlock(&r->lock);
		if (done) finish(r);
		r = next;
	}
	polling = false;
}
void ov_http_stop(void)
{
	for (struct request *r = requests; r; r = r->next) cancel_request(r);
	while (requests) {
		struct fetch *parent = requests->parent;
		fetch_remove_from_queues(parent);
		fetch_free(parent);
	}
}
static void finalise(lwc_string *scheme) { (void)scheme; ov_http_stop(); }
nserror ov_http_register(const struct ov_http_transport *ops)
{
	if (!ops || !ops->perform || transport.perform) return NSERROR_BAD_PARAMETER;
	transport = *ops;
	static const struct fetcher_operation_table table = {
		.initialise = initialise, .acceptable = acceptable, .setup = setup,
		.start = start, .abort = cancel_request, .free = free_request,
		.poll = poll_requests, .finalise = finalise,
	};
	const char *schemes[] = {"http", "https"};
	for (size_t i = 0; i < 2; i++) {
		lwc_string *scheme;
		if (lwc_intern_string(schemes[i], strlen(schemes[i]), &scheme) != lwc_error_ok)
			return NSERROR_NOMEM;
		nserror error = fetcher_add(scheme, &table);
		if (error) { lwc_string_unref(scheme); return error; }
	}
	return NSERROR_OK;
}
