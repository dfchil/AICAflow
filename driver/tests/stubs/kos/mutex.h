#ifndef AFX_TEST_MUTEX_H
#define AFX_TEST_MUTEX_H
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
#if defined(__APPLE__)
#define RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#else
#define RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#endif
static inline int mutex_trylock(mutex_t *mutex) { return pthread_mutex_trylock(mutex); }
static inline int mutex_lock(mutex_t *mutex) { return pthread_mutex_lock(mutex); }
static inline int mutex_unlock(mutex_t *mutex) { return pthread_mutex_unlock(mutex); }
#endif
