#ifndef AFX_TEST_MUTEX_H
#define AFX_TEST_MUTEX_H
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t mutex_t;
#if defined(__APPLE__)
#define RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#else
#define RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#endif
static inline int mutex_trylock(mutex_t *mutex) { return pthread_mutex_trylock(mutex); }
static inline int mutex_lock(mutex_t *mutex) { return pthread_mutex_lock(mutex); }
static inline int mutex_unlock(mutex_t *mutex) { return pthread_mutex_unlock(mutex); }
/* KOS's mutex_lock_timed(): 0 once taken, nonzero after timeout milliseconds.
 * macOS has no pthread_mutex_timedlock, so it retries every 50 us. */
static inline int afx_mutex_lock_timed(pthread_mutex_t *mutex, unsigned int timeout) {
    for (unsigned int waited_us = 0;; waited_us += 50u) {
        if (!pthread_mutex_trylock(mutex)) return 0;
        if (waited_us >= timeout * 1000u) return -1;
        nanosleep(&(struct timespec){0, 50000}, NULL);
    }
}
static inline int mutex_lock_timed(mutex_t *mutex, unsigned int timeout) {
    return afx_mutex_lock_timed(mutex, timeout);
}
#endif
