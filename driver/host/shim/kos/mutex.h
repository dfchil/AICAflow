#ifndef AICAFLOW_HOST_SHIM_MUTEX_H
#define AICAFLOW_HOST_SHIM_MUTEX_H
#include "../platform_shim.h"
#if AFX_HOST_THREADS
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER)
#define RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#else
#define RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#endif
#define mutex_trylock(mutex) pthread_mutex_trylock(mutex)
#define mutex_lock(mutex) pthread_mutex_lock(mutex)
#define mutex_unlock(mutex) pthread_mutex_unlock(mutex)
#else
/* One thread: the lock is always free to its only user. Taking it from
 * outside is where a KOS worker would have run, so the chip catches up and a
 * pending interrupt is handled first (afx_host_poll, src/platform.c). */
typedef struct { int depth; } mutex_t;
#define RECURSIVE_MUTEX_INITIALIZER {0}
void afx_host_poll(void);
static inline int afx_host_mutex_take(mutex_t *mutex) {
    if (!mutex->depth) afx_host_poll();
    ++mutex->depth;
    return 0;
}
static inline int afx_host_mutex_give(mutex_t *mutex) { --mutex->depth; return 0; }
#define mutex_trylock(mutex) afx_host_mutex_take(mutex)
#define mutex_lock(mutex) afx_host_mutex_take(mutex)
#define mutex_unlock(mutex) afx_host_mutex_give(mutex)
#endif
#endif
