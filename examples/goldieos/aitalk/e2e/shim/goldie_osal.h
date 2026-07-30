/**
 * @file goldie_osal.h (host shim for e2e tests on Linux)
 * @brief Maps the WS63 goldie_osal API to pthreads/POSIX so the
 *        aitalk open SDK can be exercised on a host without hardware.
 *        NOT used in the WS63 firmware build.
 */
#ifndef GOLDIE_OSAL_H
#define GOLDIE_OSAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_TYPE_WIN 1

typedef int (*goldie_thread_handler)(void *data);

typedef struct { void *sem;   } goldie_sem;
typedef struct { void *mutex; } goldie_mutex;

typedef struct { long tv_sec; long tv_usec; } goldie_timeval;

/* ---- thread ---- */
static inline void *goldie_thread_create(goldie_thread_handler h, void *data,
                                         const char *name, unsigned int stack_size)
{
    (void)name; (void)stack_size;
    pthread_t *t = malloc(sizeof(pthread_t));
    if (!t) return NULL;
    if (pthread_create(t, NULL, (void *(*)(void *))h, data) != 0) {
        free(t);
        return NULL;
    }
    return t;
}
static inline int goldie_thread_set_priority(void *thread, unsigned int priority)
{ (void)thread; (void)priority; return 0; }
static inline void goldie_thread_destroy(void *thread)
{ if (thread) { pthread_cancel(*(pthread_t *)thread); pthread_join(*(pthread_t *)thread, NULL); free(thread); } }
static inline void goldie_thread_lock(void) {}
static inline void goldie_thread_unlock(void) {}

/* ---- mutex ---- */
static inline void goldie_mutex_init(goldie_mutex *m)
{ m->mutex = malloc(sizeof(pthread_mutex_t)); pthread_mutex_init(m->mutex, NULL); }
static inline void goldie_mutex_lock(goldie_mutex *m)
{ pthread_mutex_lock(m->mutex); }
static inline void goldie_mutex_unlock(goldie_mutex *m)
{ pthread_mutex_unlock(m->mutex); }
static inline void goldie_mutex_destroy(goldie_mutex *m)
{ pthread_mutex_destroy(m->mutex); free(m->mutex); m->mutex = NULL; }

/* ---- semaphore (binary, counting via counter+cond) ---- */
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int count; } host_sem_t;
static inline void goldie_sem_init(goldie_sem *s)
{
    host_sem_t *hs = malloc(sizeof(host_sem_t));
    pthread_mutex_init(&hs->mu, NULL);
    pthread_cond_init(&hs->cv, NULL);
    hs->count = 0;
    s->sem = hs;
}
static inline void goldie_sem_wait(goldie_sem *s)
{
    host_sem_t *hs = s->sem;
    pthread_mutex_lock(&hs->mu);
    while (hs->count <= 0) pthread_cond_wait(&hs->cv, &hs->mu);
    hs->count--;
    pthread_mutex_unlock(&hs->mu);
}
static inline void goldie_sem_post(goldie_sem *s)
{
    host_sem_t *hs = s->sem;
    pthread_mutex_lock(&hs->mu);
    hs->count++;
    pthread_cond_signal(&hs->cv);
    pthread_mutex_unlock(&hs->mu);
}
static inline void goldie_sem_destroy(goldie_sem *s)
{
    host_sem_t *hs = s->sem;
    pthread_mutex_destroy(&hs->mu);
    pthread_cond_destroy(&hs->cv);
    free(hs);
    s->sem = NULL;
}

/* ---- time/sleep/memory ---- */
static inline void goldie_gettimeofday(goldie_timeval *tv)
{
    struct timeval t;
    gettimeofday(&t, NULL);
    tv->tv_sec = t.tv_sec;
    tv->tv_usec = t.tv_usec;
}
static inline void goldie_msleep(int ms) { usleep((useconds_t)ms * 1000); }
static inline void *goldie_malloc(unsigned long size) { return malloc(size); }
static inline void goldie_free(void *addr) { free(addr); }

#define GOLDIE_INIT_CALL_(funx)

#ifdef __cplusplus
}
#endif

#endif /* GOLDIE_OSAL_H */
