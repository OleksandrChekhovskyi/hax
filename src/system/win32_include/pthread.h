/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_WIN32_INCLUDE_PTHREAD_H
#define HAX_SYSTEM_WIN32_INCLUDE_PTHREAD_H

#include <errno.h>
#include <process.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define SIG_BLOCK                 0
#define SIG_SETMASK               1

typedef HANDLE pthread_t;
typedef SRWLOCK pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

struct hax_thread_start {
    void *(*fn)(void *);
    void *arg;
};

static unsigned __stdcall hax_thread_trampoline(void *opaque)
{
    struct hax_thread_start *start = opaque;
    void *(*fn)(void *) = start->fn;
    void *arg = start->arg;
    free(start);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attributes, void *(*fn)(void *),
                                 void *arg)
{
    (void)attributes;
    struct hax_thread_start *start = malloc(sizeof(*start));
    if (!start)
        return ENOMEM;
    start->fn = fn;
    start->arg = arg;
    uintptr_t handle = _beginthreadex(NULL, 0, hax_thread_trampoline, start, 0, NULL);
    if (!handle) {
        free(start);
        return errno ? errno : EAGAIN;
    }
    *thread = (HANDLE)handle;
    return 0;
}

static inline int pthread_join(pthread_t thread, void **result)
{
    (void)result;
    DWORD wait = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return wait == WAIT_OBJECT_0 ? 0 : EINVAL;
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes)
{
    (void)attributes;
    InitializeSRWLock(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *cond, const void *attributes)
{
    (void)attributes;
    InitializeConditionVariable(cond);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

static inline int pthread_cond_signal(pthread_cond_t *cond)
{
    WakeConditionVariable(cond);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cond)
{
    WakeAllConditionVariable(cond);
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    return SleepConditionVariableSRW(cond, mutex, INFINITE, 0) ? 0 : EINVAL;
}

static inline int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                         const struct timespec *deadline)
{
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    int64_t remaining_ms = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000 +
                           (deadline->tv_nsec - now.tv_nsec + 999999) / 1000000;
    DWORD timeout = remaining_ms <= 0           ? 0
                    : remaining_ms > UINT32_MAX ? UINT32_MAX
                                                : (DWORD)remaining_ms;
    if (SleepConditionVariableSRW(cond, mutex, timeout, 0))
        return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}

static inline int pthread_sigmask(int how, const void *set, void *old_set)
{
    (void)how;
    (void)set;
    (void)old_set;
    return 0;
}

#endif /* HAX_SYSTEM_WIN32_INCLUDE_PTHREAD_H */
