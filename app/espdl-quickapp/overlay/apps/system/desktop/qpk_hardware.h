/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <cJSON.h>

struct qpk_hw_session;
struct qpk_hw_progress {
  atomic_bool cancel, finish;
  atomic_uint bytes, elapsed_ms, peak;
};
typedef int (*qpk_pcm_sink_t)(void *cookie,const uint8_t *pcm,size_t bytes);
/* Dedicated caller-owned worker; same codec reservation and cleanup as WAV. */
int qpk_audio_capture_pcm(qpk_pcm_sink_t sink,void *cookie,struct qpk_hw_progress *progress);
/* Bounded read-only recorder signal/timing diagnostics; contains no PCM. */
int qpk_audio_recording_status(char *buffer, size_t size);
struct qpk_hw_session *qpk_hw_create(const char *package);
struct qpk_hw_session *qpk_hw_create_at(const char *package,const char *data_root);
void qpk_hw_release(struct qpk_hw_session *session);
/* For short-lived diagnostic tasks; the UI uses the nonblocking release. */
void qpk_hw_release_wait(struct qpk_hw_session *session);
int qpk_hw_submit(struct qpk_hw_session *session, const char *operation, const char *arguments);
char *qpk_hw_poll(struct qpk_hw_session *session, unsigned id);
int qpk_hw_cancel(struct qpk_hw_session *session, unsigned id, bool finish);
char *qpk_hw_capabilities(void);
char *qpk_board_interfaces(void);

/* Platform calls run on a dedicated worker, never on the LVGL/JS thread. */
void *qpk_hw_platform_create(const char *package);
void *qpk_hw_platform_create_at(const char *package,const char *data_root);
void qpk_hw_platform_destroy(void *platform);
int qpk_hw_platform_execute(void *platform, const char *operation,
                            const cJSON *arguments, cJSON *result,
                            struct qpk_hw_progress *progress);
