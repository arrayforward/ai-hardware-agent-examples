/**
 * @file goldie_osal_host.c
 * @brief Host (pthread) implementation of goldie_osal for the E2E harness.
 *        On WS63 the real implementation lives in the LiteOS board package.
 */
#include "goldie_osal.h"

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

/* ---- memory ---- */
void *goldie_malloc(unsigned long size) { return malloc(size); }
void  goldie_free(void *addr)           { free(addr); }

/* ---- time ---- */
void goldie_msleep(int ms) { usleep((useconds_t)ms * 1000); }

void goldie_gettimeofday(goldie_timeval *tv)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    tv->tv_sec = t.tv_sec;
    tv->tv_usec = t.tv_usec;
}

/* ---- threads ---- */
typedef struct {
    goldie_thread_handler fn;
    void *arg;
    pthread_t tid;
} host_thread_t;

static void *host_thread_entry(void *p)
{
    host_thread_t *t = p;
    t->fn(t->arg);
    return NULL;
}

void *goldie_thread_create(goldie_thread_handler handler, void *data,
                           const char *name, unsigned int stack_size)
{
    (void)name;
    host_thread_t *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->fn = handler;
    t->arg = data;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size) pthread_attr_setstacksize(&attr, stack_size);
    int rc = pthread_create(&t->tid, &attr, host_thread_entry, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(t); return NULL; }
    return t;
}

int goldie_thread_set_priority(void *thread, unsigned int priority)
{
    (void)thread; (void)priority;
    return 0;
}

/* LiteOS semantics: force-delete the task. pthread_cancel points inside
 * the engine are sem_wait (cond_wait) and msleep (usleep). */
void goldie_thread_destroy(void *thread)
{
    host_thread_t *t = thread;
    if (!t) return;
    pthread_cancel(t->tid);
    pthread_detach(t->tid);
    free(t);
}

void goldie_thread_lock(void)   {}
void goldie_thread_unlock(void) {}

/* ---- mutex ---- */
void goldie_mutex_init(goldie_mutex *m)
{
    m->mutex = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init((pthread_mutex_t *)m->mutex, NULL);
}
void goldie_mutex_lock(goldie_mutex *m)   { pthread_mutex_lock((pthread_mutex_t *)m->mutex); }
void goldie_mutex_unlock(goldie_mutex *m) { pthread_mutex_unlock((pthread_mutex_t *)m->mutex); }
void goldie_mutex_destroy(goldie_mutex *m)
{
    pthread_mutex_destroy((pthread_mutex_t *)m->mutex);
    free(m->mutex);
    m->mutex = NULL;
}

/* ---- counting semaphore (mutex + cond) ---- */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int count;
} host_sem_t;

void goldie_sem_init(goldie_sem *s)
{
    host_sem_t *h = malloc(sizeof(*h));
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    h->count = 0;
    s->sem = h;
}

void goldie_sem_wait(goldie_sem *s)
{
    host_sem_t *h = s->sem;
    pthread_mutex_lock(&h->mu);
    while (h->count <= 0) pthread_cond_wait(&h->cv, &h->mu);
    h->count--;
    pthread_mutex_unlock(&h->mu);
}

void goldie_sem_post(goldie_sem *s)
{
    host_sem_t *h = s->sem;
    pthread_mutex_lock(&h->mu);
    h->count++;
    pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

void goldie_sem_destroy(goldie_sem *s)
{
    host_sem_t *h = s->sem;
    if (!h) return;
    pthread_mutex_destroy(&h->mu);
    pthread_cond_destroy(&h->cv);
    free(h);
    s->sem = NULL;
}

/* ---- MSVC secure-lib shim: see vsnprintf_s_compat.c ---- */
