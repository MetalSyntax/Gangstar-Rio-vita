/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022      GrapheneCt
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/pthr.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdatomic.h>

#include "utils/utils.h"
#include "utils/logger.h"
#include "reimpl/errno.h"

#define BIONIC_PTHREAD_COND_INITIALIZER              0
#define BIONIC_PTHREAD_MUTEX_INITIALIZER             0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER   0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER  0x8000

enum {
    BIONIC_PTHREAD_MUTEX_NORMAL = 0,
    BIONIC_PTHREAD_MUTEX_RECURSIVE = 1,
    BIONIC_PTHREAD_MUTEX_ERRORCHECK = 2,

    BIONIC_PTHREAD_MUTEX_ERRORCHECK_NP = BIONIC_PTHREAD_MUTEX_ERRORCHECK,
    BIONIC_PTHREAD_MUTEX_RECURSIVE_NP  = BIONIC_PTHREAD_MUTEX_RECURSIVE,

    BIONIC_PTHREAD_MUTEX_DEFAULT = BIONIC_PTHREAD_MUTEX_NORMAL
};

#define PTHR_INLINE static inline __attribute__((always_inline))

#define PTHR_MAX_OBJECTS 8192
#define PTHR_REGISTRY_BUCKETS 16384
#define PTHR_BUCKET_EMPTY ((uint16_t)0)
#define PTHR_BUCKET_TOMBSTONE UINT16_MAX

#if (PTHR_REGISTRY_BUCKETS & (PTHR_REGISTRY_BUCKETS - 1)) != 0
#error PTHR_REGISTRY_BUCKETS must be a power of two
#endif

#if PTHR_REGISTRY_BUCKETS <= PTHR_MAX_OBJECTS
#error PTHR_REGISTRY_BUCKETS must exceed PTHR_MAX_OBJECTS
#endif

enum initialized_object_kind {
    PTHR_OBJECT_NONE = 0,
    PTHR_OBJECT_MUTEX = 1,
    PTHR_OBJECT_COND = 2,
};

typedef struct userspace_mutex {
    volatile uintptr_t owner;
    volatile int depth;
    volatile int waiters;
    int kind;
} userspace_mutex;

typedef struct userspace_cond {
    volatile unsigned int sequence;
    volatile int waiters;
} userspace_cond;

typedef struct userspace_sem {
    volatile int value;
} userspace_sem;

void * initializedObjects[PTHR_MAX_OBJECTS] = {0};
static void * initializedStates[PTHR_MAX_OBJECTS] = {0};
static unsigned char initializedObjectKinds[PTHR_MAX_OBJECTS] = {0};
static uint16_t initializedObjectBuckets[PTHR_REGISTRY_BUCKETS] = {0};
static unsigned int initializedObjectCount = 0;
static unsigned int initializedObjectHighWater = 0;
static unsigned int initializedObjectTombstones = 0;
static volatile int pthr_registry_lock = 0;

#define PTHR_CACHE_SIZE 2048u
#define PTHR_CACHE_MASK (PTHR_CACHE_SIZE - 1u)
static atomic_uintptr_t pthr_init_cache[PTHR_CACHE_SIZE];

PTHR_INLINE unsigned _pthr_cache_slot(const void * obj) {
    uint32_t h = (uint32_t) (uintptr_t) obj;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h & PTHR_CACHE_MASK;
}

PTHR_INLINE int _pthr_cache_hit(const void * obj) {
    return atomic_load_explicit(&pthr_init_cache[_pthr_cache_slot(obj)],
                                memory_order_acquire) == (uintptr_t) obj;
}

PTHR_INLINE void _pthr_cache_put(const void * obj) {
    atomic_store_explicit(&pthr_init_cache[_pthr_cache_slot(obj)],
                          (uintptr_t) obj, memory_order_release);
}

PTHR_INLINE void _pthr_cache_drop(const void * obj) {
    unsigned slot = _pthr_cache_slot(obj);
    uintptr_t expected = (uintptr_t) obj;
    atomic_compare_exchange_strong_explicit(&pthr_init_cache[slot], &expected, 0,
                                            memory_order_release,
                                            memory_order_relaxed);
}

static void registry_lock(void) {
    while (__sync_lock_test_and_set(&pthr_registry_lock, 1))
        sceKernelDelayThread(50);
}

static void registry_unlock(void) {
    __sync_lock_release(&pthr_registry_lock);
}

static long pthr_n_mutex_ok = 0, pthr_n_mutex_destroy = 0;
static long pthr_n_cond_ok = 0, pthr_n_cond_destroy = 0;
static long pthr_mutex_next_report = 128, pthr_cond_next_report = 64;

PTHR_INLINE void _pthr_report_locked(const char *what, long live, long ok,
                                     long destroyed, long *next) {
    if (live >= *next) {
        l_info("pthr: [userspace] %s live=%ld (ok=%ld destroyed=%ld)", what, live,
               ok, destroyed);
        *next *= 2;
    }
}

PTHR_INLINE unsigned int object_bucket_start(const void *object) {
    uint32_t key = (uint32_t)(uintptr_t)object >> 2;
    key ^= key >> 16;
    key *= 0x45D9F3Bu;
    key ^= key >> 16;
    return key & (PTHR_REGISTRY_BUCKETS - 1U);
}

static int find_object_bucket_locked(const void *object, unsigned int *bucket_out) {
    if (!object) return -1;
    unsigned int bucket = object_bucket_start(object);
    for (unsigned int probe = 0; probe < PTHR_REGISTRY_BUCKETS; ++probe) {
        uint16_t entry = initializedObjectBuckets[bucket];
        if (entry == PTHR_BUCKET_EMPTY)
            return -1;
        if (entry != PTHR_BUCKET_TOMBSTONE) {
            unsigned int index = (unsigned int)entry - 1U;
            if (index < initializedObjectHighWater &&
                initializedObjects[index] == object) {
                if (bucket_out) *bucket_out = bucket;
                return (int)index;
            }
        }
        bucket = (bucket + 1U) & (PTHR_REGISTRY_BUCKETS - 1U);
    }
    return -1;
}

static int find_object_locked(const void *object) {
    return find_object_bucket_locked(object, NULL);
}

static int place_object_bucket_locked(void *object, unsigned int index) {
    unsigned int bucket = object_bucket_start(object);
    unsigned int first_tombstone = PTHR_REGISTRY_BUCKETS;

    for (unsigned int probe = 0; probe < PTHR_REGISTRY_BUCKETS; ++probe) {
        uint16_t entry = initializedObjectBuckets[bucket];
        if (entry == PTHR_BUCKET_TOMBSTONE) {
            if (first_tombstone == PTHR_REGISTRY_BUCKETS)
                first_tombstone = bucket;
        } else if (entry == PTHR_BUCKET_EMPTY) {
            unsigned int destination =
                first_tombstone == PTHR_REGISTRY_BUCKETS ? bucket : first_tombstone;
            initializedObjectBuckets[destination] = (uint16_t)(index + 1U);
            if (destination == first_tombstone)
                --initializedObjectTombstones;
            return 0;
        } else {
            unsigned int existing_index = (unsigned int)entry - 1U;
            if (existing_index < initializedObjectHighWater &&
                initializedObjects[existing_index] == object)
                return existing_index == index ? 0 : -1;
        }
        bucket = (bucket + 1U) & (PTHR_REGISTRY_BUCKETS - 1U);
    }

    if (first_tombstone != PTHR_REGISTRY_BUCKETS) {
        initializedObjectBuckets[first_tombstone] = (uint16_t)(index + 1U);
        --initializedObjectTombstones;
        return 0;
    }
    return -1;
}

static int rebuild_object_buckets_locked(void) {
    memset(initializedObjectBuckets, 0, sizeof(initializedObjectBuckets));
    initializedObjectTombstones = 0;
    for (unsigned int index = 0; index < initializedObjectHighWater; ++index) {
        if (initializedObjects[index] &&
            place_object_bucket_locked(initializedObjects[index], index) != 0)
            return -1;
    }
    return 0;
}

static int remember_object_locked(void *object, void *state, unsigned char kind) {
    int selected_index = -1;
    for (unsigned int i = 0; i < initializedObjectHighWater; ++i) {
        if (initializedObjects[i] == 0) {
            selected_index = (int)i;
            break;
        }
    }

    int grew_high_water = 0;
    if (selected_index < 0) {
        if (initializedObjectHighWater >= PTHR_MAX_OBJECTS)
            return -1;
        selected_index = (int)initializedObjectHighWater++;
        grew_high_water = 1;
    }

    if (initializedObjectTombstones > PTHR_REGISTRY_BUCKETS / 4U &&
        initializedObjectTombstones > initializedObjectCount &&
        rebuild_object_buckets_locked() != 0) {
        if (grew_high_water)
            --initializedObjectHighWater;
        return -1;
    }

    unsigned int index = (unsigned int)selected_index;
    initializedObjects[index] = object;
    initializedStates[index] = state;
    initializedObjectKinds[index] = kind;
    if (place_object_bucket_locked(object, index) != 0) {
        initializedObjects[index] = 0;
        initializedStates[index] = 0;
        initializedObjectKinds[index] = PTHR_OBJECT_NONE;
        if (grew_high_water)
            --initializedObjectHighWater;
        return -1;
    }

    ++initializedObjectCount;
    return selected_index;
}

static int forget_object_locked(const void *object, void **state, unsigned char *kind) {
    unsigned int bucket = 0;
    int index = find_object_bucket_locked(object, &bucket);
    if (index >= 0) {
        if (state) *state = initializedStates[index];
        if (kind) *kind = initializedObjectKinds[index];
        initializedObjects[index] = 0;
        initializedStates[index] = 0;
        initializedObjectKinds[index] = PTHR_OBJECT_NONE;
        initializedObjectBuckets[bucket] = PTHR_BUCKET_TOMBSTONE;
        ++initializedObjectTombstones;
        if (initializedObjectCount > 0)
            --initializedObjectCount;
        while (initializedObjectHighWater > 0 &&
               initializedObjects[initializedObjectHighWater - 1] == 0) {
            --initializedObjectHighWater;
        }
        if (initializedObjectTombstones > PTHR_REGISTRY_BUCKETS / 4U &&
            initializedObjectTombstones > initializedObjectCount)
            rebuild_object_buckets_locked();
        return 1;
    }
    return 0;
}

static userspace_mutex *reusable_mutex_state_locked(int index) {
    if (index < 0 || initializedObjectKinds[index] != PTHR_OBJECT_MUTEX)
        return NULL;
    userspace_mutex *state = (userspace_mutex *)initializedStates[index];
    if (!state) return NULL;
    if (__atomic_load_n(&state->owner, __ATOMIC_ACQUIRE) != 0 ||
        __atomic_load_n(&state->waiters, __ATOMIC_ACQUIRE) != 0)
        return NULL;
    return state;
}

static userspace_cond *reusable_cond_state_locked(int index) {
    if (index < 0 || initializedObjectKinds[index] != PTHR_OBJECT_COND)
        return NULL;
    userspace_cond *state = (userspace_cond *)initializedStates[index];
    if (!state) return NULL;
    if (__atomic_load_n(&state->waiters, __ATOMIC_ACQUIRE) != 0)
        return NULL;
    return state;
}

int isObjectInitialized(const void * mut) {
    if (_pthr_cache_hit(mut)) return 1;
    registry_lock();
    int idx = find_object_locked(mut);
    registry_unlock();
    return idx >= 0;
}

int forgetObject(const void * mut) {
    _pthr_cache_drop(mut);
    void *registered_state = NULL;
    unsigned char registered_kind = PTHR_OBJECT_NONE;
    registry_lock();
    int ret = forget_object_locked(mut, &registered_state, &registered_kind);
    registry_unlock();
    if (registered_state) {
        if (registered_kind == PTHR_OBJECT_MUTEX) pthr_n_mutex_destroy++;
        else if (registered_kind == PTHR_OBJECT_COND) pthr_n_cond_destroy++;
        free(registered_state);
    }
    return ret;
}

static userspace_mutex *get_userspace_mutex(const pthread_mutex_t_bionic *mutex) {
    return mutex ? (userspace_mutex *)mutex->real_ptr : NULL;
}

static uintptr_t userspace_thread_id(void) {
    return (uintptr_t)pthread_self();
}

static int userspace_mutex_acquire(userspace_mutex *entry, int try_only) {
    if (!entry) return EINVAL;

    __atomic_add_fetch(&entry->waiters, 1, __ATOMIC_ACQ_REL);
    const uintptr_t self = userspace_thread_id();
    unsigned int wait_us = 50;
    int result = 0;
    for (;;) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&entry->owner, &expected, self, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&entry->depth, 1, __ATOMIC_RELAXED);
            result = 0;
            break;
        }

        if (expected == self) {
            if (entry->kind == PTHREAD_MUTEX_RECURSIVE) {
                __atomic_add_fetch(&entry->depth, 1, __ATOMIC_RELAXED);
                result = 0;
                break;
            }
            if (entry->kind == PTHREAD_MUTEX_ERRORCHECK) {
                result = try_only ? EBUSY : EDEADLK;
                break;
            }
        }

        if (try_only) {
            result = EBUSY;
            break;
        }
        sceKernelDelayThread(wait_us);
        if (wait_us < 1000) wait_us *= 2;
        if (wait_us > 1000) wait_us = 1000;
    }

    __atomic_sub_fetch(&entry->waiters, 1, __ATOMIC_ACQ_REL);
    return result;
}

static int userspace_mutex_release(userspace_mutex *entry) {
    if (!entry) return EINVAL;

    const uintptr_t self = userspace_thread_id();
    if (__atomic_load_n(&entry->owner, __ATOMIC_ACQUIRE) != self)
        return EPERM;

    int depth = __atomic_load_n(&entry->depth, __ATOMIC_RELAXED);
    if (entry->kind == PTHREAD_MUTEX_RECURSIVE && depth > 1) {
        __atomic_sub_fetch(&entry->depth, 1, __ATOMIC_RELAXED);
        return 0;
    }

    __atomic_store_n(&entry->depth, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&entry->owner, 0, __ATOMIC_RELEASE);
    return 0;
}

static userspace_cond *get_userspace_cond(const pthread_cond_t_bionic *cond) {
    return cond ? (userspace_cond *)cond->real_ptr : NULL;
}

static int userspace_cond_wait(userspace_cond *cond,
                               userspace_mutex *mutex,
                               const struct timespec *abstime) {
    if (!cond || !mutex) return EINVAL;

    unsigned int sequence = __atomic_load_n(&cond->sequence, __ATOMIC_ACQUIRE);
    __atomic_add_fetch(&cond->waiters, 1, __ATOMIC_ACQ_REL);
    int result = userspace_mutex_release(mutex);
    if (result != 0) {
        __atomic_sub_fetch(&cond->waiters, 1, __ATOMIC_ACQ_REL);
        return result;
    }

    int timed_out = 0;
    unsigned int wait_us = 100;
    for (;;) {
        if (__atomic_load_n(&cond->sequence, __ATOMIC_ACQUIRE) != sequence)
            break;
        if (abstime) {
            const uint64_t deadline_us =
                (uint64_t)abstime->tv_sec * 1000000ULL +
                (uint64_t)abstime->tv_nsec / 1000ULL;
            if (current_timestamp_ms() * 1000ULL >= deadline_us) {
                timed_out = 1;
                break;
            }
        }
        sceKernelDelayThread(wait_us);
        if (wait_us < 2000) wait_us *= 2;
        if (wait_us > 2000) wait_us = 2000;
    }

    __atomic_sub_fetch(&cond->waiters, 1, __ATOMIC_ACQ_REL);
    result = userspace_mutex_acquire(mutex, 0);
    if (result != 0) return result;
    return timed_out ? ETIMEDOUT : 0;
}

static userspace_sem *get_userspace_sem(const int *guest_sem) {
    if (!guest_sem || *guest_sem == 0)
        return NULL;
    return (userspace_sem *)(uintptr_t)(uint32_t)*guest_sem;
}

PTHR_INLINE int is_bionic_mutex_initializer_value(uintptr_t value) {
    return value == BIONIC_PTHREAD_MUTEX_INITIALIZER ||
           value == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER ||
           value == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
}

PTHR_INLINE int _attr_t_static_init(pthread_attr_t_bionic * attr) {
    if (attr->magic != 0x42424242) {
        attr->magic = 0x42424242;
        attr->real_ptr = malloc(sizeof(pthread_attr_t));
        return pthread_attr_init(attr->real_ptr);
    }
    return 0;
}

PTHR_INLINE int _mutex_t_static_init(pthread_mutex_t_bionic * mutex, const pthread_mutexattr_t * attr) {
    if (!mutex) return EINVAL;

    if (_pthr_cache_hit(mutex)) return 0;

    int kind = PTHREAD_MUTEX_NORMAL;
    userspace_mutex *entry = NULL;

    registry_lock();
    int existing_index = find_object_locked(mutex);
    if (existing_index >= 0) {
        uintptr_t stored_state = (uintptr_t)mutex->real_ptr;
        if (!is_bionic_mutex_initializer_value(stored_state) &&
            initializedObjectKinds[existing_index] == PTHR_OBJECT_MUTEX &&
            mutex->real_ptr == (pthread_mutex_t *)initializedStates[existing_index]) {
            _pthr_cache_put(mutex);
            registry_unlock();
            return 0;
        }
        entry = reusable_mutex_state_locked(existing_index);
    }

    if (attr) {
        pthread_mutexattr_gettype((pthread_mutexattr_t *) attr, &kind);
    } else {
        uintptr_t val = (uintptr_t) mutex->real_ptr;
        if (val == BIONIC_PTHREAD_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_NORMAL;
        else if (val == BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_RECURSIVE;
        else if (val == BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) kind = PTHREAD_MUTEX_ERRORCHECK;
    }

    if (!entry) {
        entry = malloc(sizeof(userspace_mutex));
        if (!entry) {
            l_error("userspace mutex allocation for %p failed", mutex);
            registry_unlock();
            return ENOMEM;
        }
    }

    entry->owner = 0;
    entry->depth = 0;
    entry->waiters = 0;
    entry->kind = kind;
    mutex->real_ptr = (pthread_mutex_t *)entry;

    if (existing_index >= 0) {
        initializedStates[existing_index] = entry;
        initializedObjectKinds[existing_index] = PTHR_OBJECT_MUTEX;
    } else if (remember_object_locked(mutex, entry, PTHR_OBJECT_MUTEX) < 0) {
        free(entry);
        mutex->real_ptr = NULL;
        l_error("userspace mutex registry full (%d) for %p", PTHR_MAX_OBJECTS, mutex);
        registry_unlock();
        return ENOSPC;
    }

    _pthr_cache_put(mutex);
    pthr_n_mutex_ok++;
    _pthr_report_locked("mutex", pthr_n_mutex_ok - pthr_n_mutex_destroy,
                        pthr_n_mutex_ok, pthr_n_mutex_destroy,
                        &pthr_mutex_next_report);
    registry_unlock();
    return 0;
}

PTHR_INLINE int _cond_t_static_init(pthread_cond_t_bionic * cond, const pthread_condattr_t * attr) {
    if (!cond) return EINVAL;

    if (_pthr_cache_hit(cond)) return 0;

    userspace_cond *entry = NULL;

    registry_lock();
    int existing_index = find_object_locked(cond);
    if (existing_index >= 0) {
        if (cond->real_ptr != NULL &&
            initializedObjectKinds[existing_index] == PTHR_OBJECT_COND &&
            cond->real_ptr == (pthread_cond_t *)initializedStates[existing_index]) {
            _pthr_cache_put(cond);
            registry_unlock();
            return 0;
        }
        entry = reusable_cond_state_locked(existing_index);
    }

    if (!entry) {
        entry = malloc(sizeof(userspace_cond));
        if (!entry) {
            l_error("userspace cond allocation for %p failed", cond);
            registry_unlock();
            return ENOMEM;
        }
    }

    entry->sequence = 0;
    entry->waiters = 0;
    cond->real_ptr = (pthread_cond_t *)entry;

    if (existing_index >= 0) {
        initializedStates[existing_index] = entry;
        initializedObjectKinds[existing_index] = PTHR_OBJECT_COND;
    } else if (remember_object_locked(cond, entry, PTHR_OBJECT_COND) < 0) {
        free(entry);
        cond->real_ptr = NULL;
        l_error("userspace cond registry full (%d) for %p", PTHR_MAX_OBJECTS, cond);
        registry_unlock();
        return ENOSPC;
    }

    _pthr_cache_put(cond);
    pthr_n_cond_ok++;
    _pthr_report_locked("cond", pthr_n_cond_ok - pthr_n_cond_destroy,
                        pthr_n_cond_ok, pthr_n_cond_destroy,
                        &pthr_cond_next_report);
    registry_unlock();
    return 0;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr, void *(*start)(void *), void *param) {
    int ret;

    if (!attr) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 512 * 1024);
        ret = pthread_create(thread, &a, start, param);
        pthread_attr_destroy(&a);
    } else {
        _attr_t_static_init((pthread_attr_t_bionic *) attr);
        pthread_attr_setstacksize(attr->real_ptr, 512 * 1024);
        ret = pthread_create(thread, attr->real_ptr, start, param);
    }

    if (ret == 0) {
        sceKernelDelayThread(1000);
    }

    return ret;
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_init(attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type)
{
    return pthread_mutexattr_settype(attr, type);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr)
{
    return pthread_mutexattr_destroy(attr);
}

int pthread_kill_soloader(pthread_t thread, int sig)
{
    return pthread_kill(thread, sig);
}

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr)
{
    if (!uid) return EINVAL;
    _pthr_cache_drop(uid);
    void *registered_state = NULL;
    unsigned char registered_kind = PTHR_OBJECT_NONE;
    registry_lock();
    forget_object_locked(uid, &registered_state, &registered_kind);
    registry_unlock();
    if (registered_state) {
        if (registered_kind == PTHR_OBJECT_MUTEX) pthr_n_mutex_destroy++;
        else if (registered_kind == PTHR_OBJECT_COND) pthr_n_cond_destroy++;
        free(registered_state);
    }
    uid->real_ptr = NULL;
    return _mutex_t_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return 0;
    _pthr_cache_drop(mutex);
    void *registered_state = NULL;
    unsigned char registered_kind = PTHR_OBJECT_NONE;
    registry_lock();
    forget_object_locked(mutex, &registered_state, &registered_kind);
    mutex->real_ptr = NULL;
    registry_unlock();
    if (registered_state && registered_kind == PTHR_OBJECT_MUTEX) {
        pthr_n_mutex_destroy++;
        free(registered_state);
    }
    return 0;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    int result = _mutex_t_static_init(mutex, NULL);
    if (result != 0) return result;
    return userspace_mutex_acquire(get_userspace_mutex(mutex), 0);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    int result = _mutex_t_static_init(mutex, NULL);
    if (result != 0) return result;
    return userspace_mutex_acquire(get_userspace_mutex(mutex), 1);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex)
{
    if (!mutex) return EINVAL;
    if (!mutex->real_ptr) return EINVAL;
    return userspace_mutex_release(get_userspace_mutex(mutex));
}

int pthread_join_soloader(pthread_t thread, void **value_ptr)
{
    return pthread_join(thread, value_ptr);
}

int pthread_condattr_init_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_init(attr);
}

int pthread_condattr_destroy_soloader(pthread_condattr_t *attr)
{
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(attr);
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr)
{
    if (!cond) return EINVAL;
    _pthr_cache_drop(cond);
    void *registered_state = NULL;
    unsigned char registered_kind = PTHR_OBJECT_NONE;
    registry_lock();
    forget_object_locked(cond, &registered_state, &registered_kind);
    registry_unlock();
    if (registered_state) {
        if (registered_kind == PTHR_OBJECT_MUTEX) pthr_n_mutex_destroy++;
        else if (registered_kind == PTHR_OBJECT_COND) pthr_n_cond_destroy++;
        free(registered_state);
    }
    cond->real_ptr = NULL;
    return _cond_t_static_init(cond, attr);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return 0;
    _pthr_cache_drop(cond);
    void *registered_state = NULL;
    unsigned char registered_kind = PTHR_OBJECT_NONE;
    registry_lock();
    int existing_index = find_object_locked(cond);
    userspace_cond *real_ptr =
        existing_index >= 0 &&
        initializedObjectKinds[existing_index] == PTHR_OBJECT_COND
            ? (userspace_cond *)initializedStates[existing_index]
            : NULL;
    if (real_ptr && __atomic_load_n(&real_ptr->waiters, __ATOMIC_ACQUIRE) > 0) {
        registry_unlock();
        return EBUSY;
    }
    forget_object_locked(cond, &registered_state, &registered_kind);
    cond->real_ptr = NULL;
    registry_unlock();
    if (registered_state && registered_kind == PTHR_OBJECT_COND) {
        pthr_n_cond_destroy++;
        free(registered_state);
    }
    return 0;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond)
{
    if (!cond) return EINVAL;

    int result = _cond_t_static_init(cond, NULL);
    if (result != 0) return result;
    userspace_cond *real = get_userspace_cond(cond);
    if (real) {
        __atomic_add_fetch(&real->sequence, 1, __ATOMIC_RELEASE);
    }
    return 0;
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond)
{
    return pthread_cond_signal_soloader(cond);
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex, struct timespec *abstime)
{
    if (!cond || !mutex) return EINVAL;
    if (abstime && (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L)) {
        return EINVAL;
    }
    int result = _cond_t_static_init(cond, NULL);
    if (result != 0) return result;
    result = _mutex_t_static_init(mutex, NULL);
    if (result != 0) return result;

    result = userspace_cond_wait(get_userspace_cond(cond), get_userspace_mutex(mutex), abstime);
    return errno_soloader_translate_from_newlib(result);
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex)
{
    if (!cond || !mutex) return EINVAL;

    int result = _cond_t_static_init(cond, NULL);
    if (result != 0) return result;
    result = _mutex_t_static_init(mutex, NULL);
    if (result != 0) return result;

    result = userspace_cond_wait(get_userspace_cond(cond), get_userspace_mutex(mutex), NULL);
    return errno_soloader_translate_from_newlib(result);
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return EINVAL;

    return _attr_t_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr)
{
    if (!attr) return 0;
    if (attr->magic != 0x42424242) return 0;

    int ret = pthread_attr_destroy(attr->real_ptr);
    free(attr->real_ptr);
    attr->magic = 0x0;

    return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state)
{
    if (!attr) return -1;
    _attr_t_static_init(attr);
    state = !state; // pthread-embedded has JOINABLE/DETACHED swapped compared to BIONIC
    return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
    if (!attr) return -1;
    _attr_t_static_init(attr);
    return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}

int pthread_setschedparam_soloader(pthread_t thread, int policy,
                                   const struct sched_param *param)
{
   return pthread_setschedparam(thread, policy, param);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy,
                                   struct sched_param *param)
{
    return pthread_getschedparam(thread, policy, param);
}

int pthread_detach_soloader(pthread_t thread)
{
    return pthread_detach(thread);
}

int pthread_equal_soloader(const pthread_t t1, const pthread_t t2)
{
    if (t1 == t2)
        return 1;
    if (!t1 || !t2)
        return 0;
    return pthread_equal(t1, t2);
}

pthread_t pthread_self_soloader()
{
    return pthread_self();
}

int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine)
        return EINVAL;

    int state = __atomic_load_n(once_control, __ATOMIC_ACQUIRE);
    if (state == 2)
        return 0;

    if (state == 0 && __sync_bool_compare_and_swap(once_control, 0, 1)) {
        (*init_routine)();
        __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
        return 0;
    }

    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 2)
        sceKernelDelayThread(100);
    return 0;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(pthread_t thread, const char* thread_name) {
    if (thread == 0 || thread_name == NULL) {
        return EINVAL;
    }
    size_t thread_name_len = strlen(thread_name);
    if (thread_name_len >= MAX_TASK_COMM_LEN) {
        return ERANGE;
    }

    sceClibPrintf("PTHREAD: pthread_setname_np with name %s for thread:0x%x\n", thread_name, pthread_self());

    return 0;
}

int sem_init_soloader (int * uid, int pshared, unsigned int value) {
    if (!uid || pshared != 0 || value > INT_MAX) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    userspace_sem *entry = malloc(sizeof(*entry));
    if (!entry) {
        errno_soloader_set_from_newlib(ENOMEM);
        return -1;
    }

    entry->value = (int)value;
    *uid = (int)(uintptr_t)entry;
    return 0;
}

int sem_destroy_soloader(int * uid) {
    if (!uid) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    userspace_sem *entry = get_userspace_sem(uid);
    if (!entry) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    free(entry);
    *uid = 0;
    return 0;
}

int sem_getvalue_soloader (int * uid, int * sval) {
    if (!uid || !sval) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    userspace_sem *entry = get_userspace_sem(uid);
    if (!entry) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    *sval = __atomic_load_n(&entry->value, __ATOMIC_ACQUIRE);
    return 0;
}

int sem_post_soloader (int * uid) {
    if (!uid) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    userspace_sem *entry = get_userspace_sem(uid);
    if (!entry) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    __atomic_add_fetch(&entry->value, 1, __ATOMIC_RELEASE);
    return 0;
}

int sem_trywait_soloader (int * uid) {
    if (!uid) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    userspace_sem *entry = get_userspace_sem(uid);
    if (!entry) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    int current = __atomic_load_n(&entry->value, __ATOMIC_ACQUIRE);
    while (current > 0) {
        if (__atomic_compare_exchange_n(&entry->value, &current, current - 1,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }

    errno_soloader_set_from_newlib(EAGAIN);
    return -1;
}

int sem_wait_soloader (int * uid) {
    if (!uid) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    userspace_sem *entry = get_userspace_sem(uid);
    if (!entry) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    unsigned int wait_us = 50;
    for (;;) {
        int current = __atomic_load_n(&entry->value, __ATOMIC_ACQUIRE);
        while (current > 0) {
            if (__atomic_compare_exchange_n(&entry->value, &current, current - 1,
                                            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return 0;
        }
        sceKernelDelayThread(wait_us);
        if (wait_us < 1000) wait_us *= 2;
        if (wait_us > 1000) wait_us = 1000;
    }
}

int sem_timedwait_soloader (int * uid, const struct timespec * abstime) {
    if (!uid || !abstime) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    userspace_sem *entry = get_userspace_sem(uid);
    if (!entry) {
        errno_soloader_set_from_newlib(EINVAL);
        return -1;
    }

    const uint64_t deadline_us =
        (uint64_t)abstime->tv_sec * 1000000ULL +
        (uint64_t)abstime->tv_nsec / 1000ULL;
    unsigned int wait_us = 50;
    for (;;) {
        int current = __atomic_load_n(&entry->value, __ATOMIC_ACQUIRE);
        while (current > 0) {
            if (__atomic_compare_exchange_n(&entry->value, &current, current - 1,
                                            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return 0;
        }

        if (current_timestamp_ms() * 1000ULL >= deadline_us) {
            errno_soloader_set_from_newlib(ETIMEDOUT);
            return -1;
        }

        sceKernelDelayThread(wait_us);
        if (wait_us < 1000) wait_us *= 2;
        if (wait_us > 1000) wait_us = 1000;
    }
}
