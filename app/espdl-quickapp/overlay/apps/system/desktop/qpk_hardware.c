/* SPDX-License-Identifier: Apache-2.0 */
#include "qpk_hardware.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct hw_job {
  struct hw_job *next;
  unsigned id;
  bool running, done;
  int error;
  char operation[32];
  cJSON *arguments, *result;
  struct qpk_hw_progress progress;
};
struct qpk_hw_session {
  pthread_mutex_t lock;
  pthread_cond_t condition;
  bool closing;
  unsigned sequence, workers;
  pthread_t threads[2];
  struct hw_job *jobs;
  void *platform;
};
struct hw_worker { struct qpk_hw_session *session; bool audio; };
static void free_job(struct hw_job *job)
{ cJSON_Delete(job->arguments); cJSON_Delete(job->result); free(job); }

static void *hardware_worker(void *arg)
{
  struct hw_worker *worker = arg;
  struct qpk_hw_session *s = worker->session;
  bool audio = worker->audio; free(worker);
  pthread_mutex_lock(&s->lock);
  while (!s->closing) {
    struct hw_job *job = s->jobs;
    while (job && (job->done || job->running || (!strncmp(job->operation, "audio.", 6)) != audio)) job = job->next;
    if (!job) { pthread_cond_wait(&s->condition, &s->lock); continue; }
    job->running = true;
    pthread_mutex_unlock(&s->lock);
    job->result = cJSON_CreateObject();
    int ret = !job->result ? -ENOMEM : atomic_load(&job->progress.cancel) ? -ECANCELED :
      qpk_hw_platform_execute(s->platform, job->operation, job->arguments, job->result, &job->progress);
    if (atomic_load(&job->progress.cancel)) ret = -ECANCELED;
    pthread_mutex_lock(&s->lock);
    job->error = ret; job->done = true; job->running = false;
    cJSON_Delete(job->arguments); job->arguments = NULL;
  }
  bool last = --s->workers == 0;
  pthread_mutex_unlock(&s->lock);
  if (!last) return NULL;
  qpk_hw_platform_destroy(s->platform);
  while (s->jobs) { struct hw_job *next = s->jobs->next; free_job(s->jobs); s->jobs = next; }
  pthread_cond_destroy(&s->condition); pthread_mutex_destroy(&s->lock); free(s);
  return NULL;
}

struct qpk_hw_session *qpk_hw_create_at(const char *package,const char *data_root)
{
  struct qpk_hw_session *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->platform = data_root?qpk_hw_platform_create_at(package,data_root):qpk_hw_platform_create(package);
  if (!s->platform) { free(s); return NULL; }
  int ret = pthread_mutex_init(&s->lock, NULL);
  if (ret) goto fail;
  ret = pthread_cond_init(&s->condition, NULL);
  if (ret) { pthread_mutex_destroy(&s->lock); goto fail; }
  pthread_attr_t attr; pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 32768);
  pthread_mutex_lock(&s->lock);
  for (unsigned i = 0; i < 2; i++) {
    struct hw_worker *worker = malloc(sizeof(*worker));
    if (!worker) { ret = ENOMEM; break; }
    worker->session = s; worker->audio = i != 0;
    ret = pthread_create(&s->threads[i], &attr, hardware_worker, worker);
    if (ret) { free(worker); break; }
    s->workers++;
  }
  if (ret && s->workers) {
    for (unsigned i = 0; i < s->workers; i++) pthread_detach(s->threads[i]);
    s->closing = true; pthread_cond_broadcast(&s->condition);
    pthread_mutex_unlock(&s->lock); pthread_attr_destroy(&attr); return NULL;
  }
  pthread_mutex_unlock(&s->lock);
  pthread_attr_destroy(&attr);
  if (!ret) return s;
  pthread_cond_destroy(&s->condition); pthread_mutex_destroy(&s->lock);
fail:
  qpk_hw_platform_destroy(s->platform); free(s); return NULL;
}

struct qpk_hw_session *qpk_hw_create(const char *package){return qpk_hw_create_at(package,NULL);}

static void release_session(struct qpk_hw_session *s, bool wait)
{
  if (!s) return;
  pthread_t threads[2];
  pthread_mutex_lock(&s->lock);
  memcpy(threads, s->threads, sizeof(threads));
  if (!wait) for (unsigned i = 0; i < 2; i++) pthread_detach(threads[i]);
  s->closing = true;
  for (struct hw_job *j = s->jobs; j; j = j->next) atomic_store(&j->progress.cancel, true);
  pthread_cond_broadcast(&s->condition); pthread_mutex_unlock(&s->lock);
  /* The last worker owns s and may already have freed it here. */
  if (wait) for (unsigned i = 0; i < 2; i++) pthread_join(threads[i], NULL);
}

void qpk_hw_release(struct qpk_hw_session *s) { release_session(s, false); }
void qpk_hw_release_wait(struct qpk_hw_session *s) { release_session(s, true); }

int qpk_hw_submit(struct qpk_hw_session *s, const char *operation, const char *arguments)
{
  if (!s || !operation || strlen(operation) >= sizeof(((struct hw_job *)0)->operation)) return -EINVAL;
  cJSON *root = cJSON_ParseWithOpts(arguments ? arguments : "{}", NULL, true);
  if (!cJSON_IsObject(root)) { cJSON_Delete(root); return -EINVAL; }
  struct hw_job *j = calloc(1, sizeof(*j));
  if (!j) { cJSON_Delete(root); return -ENOMEM; }
  atomic_init(&j->progress.cancel, false); atomic_init(&j->progress.finish, false);
  atomic_init(&j->progress.bytes, 0); atomic_init(&j->progress.elapsed_ms, 0); atomic_init(&j->progress.peak, 0); atomic_init(&j->progress.stage, 0);
  j->arguments = root; strcpy(j->operation, operation);
  pthread_mutex_lock(&s->lock);
  if (s->closing || s->sequence == INT_MAX) {
    pthread_mutex_unlock(&s->lock); free_job(j); return -ECANCELED;
  }
  j->id = ++s->sequence;
  struct hw_job **tail = &s->jobs; while (*tail) tail = &(*tail)->next;
  *tail = j;
  int id = j->id;
  pthread_cond_broadcast(&s->condition); pthread_mutex_unlock(&s->lock); return id;
}

char *qpk_hw_poll(struct qpk_hw_session *s, unsigned id)
{
  if (!s) return NULL;
  cJSON *out = cJSON_CreateObject(); if (!out) return NULL;
  pthread_mutex_lock(&s->lock);
  struct hw_job **link = &s->jobs; while (*link && (*link)->id != id) link = &(*link)->next;
  struct hw_job *j = *link;
  cJSON_AddNumberToObject(out, "id", id);
  cJSON_AddBoolToObject(out, "done", !j || j->done);
  cJSON_AddBoolToObject(out, "ok", j && j->done && !j->error);
  cJSON_AddStringToObject(out, "state", !j ? "unknown" : j->done ? "done" : j->running ? "running" : "queued");
  if (j) {
    cJSON_AddNumberToObject(out, "bytes", atomic_load(&j->progress.bytes));
    cJSON_AddNumberToObject(out, "elapsedMs", atomic_load(&j->progress.elapsed_ms));
    cJSON_AddNumberToObject(out, "peak", atomic_load(&j->progress.peak));
    cJSON_AddNumberToObject(out, "stage", atomic_load(&j->progress.stage));
  }
  if (!j || (j->done && j->error)) {
    int error = j ? j->error : -ENOENT;
    cJSON_AddNumberToObject(out, "error", error);
    cJSON_AddStringToObject(out, "message", strerror(-error));
  }
  if (j && j->done) {
    if (j->result) { cJSON_AddItemToObject(out, "result", j->result); j->result = NULL; }
    *link = j->next; free_job(j);
  }
  pthread_mutex_unlock(&s->lock);
  char *text = cJSON_PrintUnformatted(out); cJSON_Delete(out); return text;
}

int qpk_hw_cancel(struct qpk_hw_session *s, unsigned id, bool finish)
{
  if (!s) return -ENOENT;
  pthread_mutex_lock(&s->lock);
  struct hw_job *j = s->jobs; while (j && j->id != id) j = j->next;
  int ret = !j ? -ENOENT : j->done && !finish ? -EALREADY : 0;
  if (!ret && !j->done) atomic_store(finish ? &j->progress.finish : &j->progress.cancel, true);
  pthread_mutex_unlock(&s->lock); return ret;
}
