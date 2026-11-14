// Orbis event queues (libkernel sceKernel*Equeue / sceKernel*Event).
//
// Orbis equeues are FreeBSD kqueues. Each guest equeue is backed by a host kqueue, which holds the
// knotes whose filter the host kernel implements with the same semantics: EVFILT_TIMER (periodic,
// EV_CLEAR, data = expirations since the last retrieval) and EVFILT_READ / EVFILT_WRITE on guest
// fds, which are host fds (data = bytes available / space left, EV_EOF, NOTE_LOWAT). Events of
// filters the host does not have (video out, graphics core, user events) are posted by the
// runtime into a per-queue FIFO; a post triggers the host EVFILT_USER "wake" knote so a waiter
// blocked in kevent64() returns and drains the FIFO.
#include "ps4_equeue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/event.h>

_Static_assert(EVFILT_READ == ORBIS_KERNEL_EVFILT_READ && EVFILT_WRITE == ORBIS_KERNEL_EVFILT_WRITE &&
                   EVFILT_TIMER == ORBIS_KERNEL_EVFILT_TIMER,
               "host and Orbis filter numbers");

#define MAX_EQUEUES 128
#define EQUEUE_CAPACITY 64

// Ident of the host EVFILT_USER knote that wakes waiters. Guest idents of user events are
// sign-extended 32-bit ids and never take this value.
#define EQUEUE_WAKE_IDENT ((uint64_t)1 << 40)

// FreeBSD strips the action flags from the kevent it keeps for a knote; XNU returns EV_ADD.
#define HOST_ACTION_FLAGS (EV_ADD | EV_DELETE | EV_ENABLE | EV_DISABLE)

typedef struct {
    int32_t id;
    int in_use;     // slot taken (until the last reference is dropped)
    int is_deleted; // sceKernelDeleteEqueue was called
    int refs;       // creation reference + one per call in flight
    int kq;         // host kqueue
    char name[64];
    pthread_mutex_t mutex; // guards the FIFO
    OrbisKernelEvent events[EQUEUE_CAPACITY];
    int head;
    int tail;
    int count;
} KernelEqueueInternal;

static KernelEqueueInternal g_equeues[MAX_EQUEUES];
static pthread_mutex_t g_eq_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_next_eq_id = 1;

// Returns the live queue with a reference taken, or NULL.
static KernelEqueueInternal *eq_acquire(OrbisKernelEqueue id) {
    if (id <= 0) return NULL;
    KernelEqueueInternal *found = NULL;
    pthread_mutex_lock(&g_eq_table_mutex);
    for (int i = 0; i < MAX_EQUEUES; i++) {
        KernelEqueueInternal *eq = &g_equeues[i];
        if (eq->in_use && !eq->is_deleted && eq->id == id) {
            eq->refs++;
            found = eq;
            break;
        }
    }
    pthread_mutex_unlock(&g_eq_table_mutex);
    return found;
}

static void eq_release(KernelEqueueInternal *eq) {
    pthread_mutex_lock(&g_eq_table_mutex);
    if (--eq->refs == 0) {
        close(eq->kq);
        pthread_mutex_destroy(&eq->mutex);
        eq->kq = -1;
        eq->in_use = 0;
    }
    pthread_mutex_unlock(&g_eq_table_mutex);
}

static void eq_wake(KernelEqueueInternal *eq) {
    struct kevent64_s kev;
    EV_SET64(&kev, EQUEUE_WAKE_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, 0, 0, 0);
    if (kevent64(eq->kq, &kev, 1, NULL, 0, 0, NULL) != 0) {
        fprintf(stderr, "[equeue] wake trigger on eq %d failed: %s\n", eq->id, strerror(errno));
        abort();
    }
}

static int orbis_error(int err) {
    return (int)(0x80020000u | (uint32_t)err);
}

int sceKernelCreateEqueue(OrbisKernelEqueue *eqOut, const char *name) {
    if (!eqOut) return (int)ORBIS_KERNEL_ERROR_EINVAL;

    int kq = kqueue();
    if (kq < 0) return orbis_error(errno);
    // XNU fixes a kqueue to the first of kevent/kevent64 used on it; all calls here are kevent64.
    struct kevent64_s wake;
    EV_SET64(&wake, EQUEUE_WAKE_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, 0, 0, 0);
    if (kevent64(kq, &wake, 1, NULL, 0, 0, NULL) != 0) {
        int err = errno;
        close(kq);
        return orbis_error(err);
    }

    pthread_mutex_lock(&g_eq_table_mutex);
    KernelEqueueInternal *eq = NULL;
    for (int i = 0; i < MAX_EQUEUES; i++) {
        if (!g_equeues[i].in_use) {
            eq = &g_equeues[i];
            break;
        }
    }
    if (!eq) {
        pthread_mutex_unlock(&g_eq_table_mutex);
        close(kq);
        return (int)ORBIS_KERNEL_ERROR_ENOMEM;
    }

    eq->id = g_next_eq_id++;
    eq->in_use = 1;
    eq->is_deleted = 0;
    eq->refs = 1;
    eq->kq = kq;
    eq->head = 0;
    eq->tail = 0;
    eq->count = 0;
    if (name) {
        strncpy(eq->name, name, sizeof(eq->name) - 1);
        eq->name[sizeof(eq->name) - 1] = '\0';
    } else {
        eq->name[0] = '\0';
    }
    pthread_mutex_init(&eq->mutex, NULL);

    *eqOut = eq->id;
    pthread_mutex_unlock(&g_eq_table_mutex);
    return 0;
}

// Marks the queue deleted, wakes its waiters (they return EBADF) and drops the creation
// reference; the host kqueue closes when the last call in flight returns.
static void eq_delete(KernelEqueueInternal *eq) {
    pthread_mutex_lock(&g_eq_table_mutex);
    __atomic_store_n(&eq->is_deleted, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_eq_table_mutex);
    eq_wake(eq);
    eq_release(eq); // the creation reference
}

int sceKernelDeleteEqueue(OrbisKernelEqueue id) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return (int)ORBIS_KERNEL_ERROR_EBADF;
    eq_delete(eq);
    eq_release(eq);
    return 0;
}

void ps4_equeue_destroy(void) {
    for (int i = 0; i < MAX_EQUEUES; i++) {
        pthread_mutex_lock(&g_eq_table_mutex);
        int live = g_equeues[i].in_use && !g_equeues[i].is_deleted;
        int32_t id = g_equeues[i].id;
        pthread_mutex_unlock(&g_eq_table_mutex);
        if (live) {
            sceKernelDeleteEqueue(id);
        }
    }
}

int ps4_equeue_exists(OrbisKernelEqueue id) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return 0;
    eq_release(eq);
    return 1;
}

int ps4_equeue_post_event(OrbisKernelEqueue id, uint64_t ident, int16_t filter, int64_t data, void *udata) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return -EINVAL;

    pthread_mutex_lock(&eq->mutex);
    int queued = 0;
    if (eq->count < EQUEUE_CAPACITY) {
        OrbisKernelEvent *ev = &eq->events[eq->tail];
        ev->ident = ident;
        ev->filter = filter;
        ev->flags = 0;
        ev->fflags = 0;
        ev->data = data;
        ev->udata = udata;
        eq->tail = (eq->tail + 1) % EQUEUE_CAPACITY;
        eq->count++;
        queued = 1;
    }
    pthread_mutex_unlock(&eq->mutex);
    if (queued) eq_wake(eq);
    eq_release(eq);
    return 0;
}

int ps4_equeue_trigger_event(OrbisKernelEqueue id, uint64_t ident, int16_t filter, int64_t data, void *udata) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return -EINVAL;

    pthread_mutex_lock(&eq->mutex);
    int coalesced = 0;
    for (int i = 0; i < eq->count; i++) {
        OrbisKernelEvent *ev = &eq->events[(eq->head + i) % EQUEUE_CAPACITY];
        if (ev->ident == ident && ev->filter == filter) {
            ev->data = data;
            ev->udata = udata;
            ev->fflags++;
            coalesced = 1;
            break;
        }
    }
    if (!coalesced && eq->count < EQUEUE_CAPACITY) {
        OrbisKernelEvent *ev = &eq->events[eq->tail];
        ev->ident = ident;
        ev->filter = filter;
        ev->flags = 0;
        ev->fflags = 1;
        ev->data = data;
        ev->udata = udata;
        eq->tail = (eq->tail + 1) % EQUEUE_CAPACITY;
        eq->count++;
    }
    pthread_mutex_unlock(&eq->mutex);
    eq_wake(eq);
    eq_release(eq);
    return 0;
}

static void remove_events_locked(KernelEqueueInternal *eq, uint64_t ident, int16_t filter) {
    int write_idx = 0;
    int original_count = eq->count;
    for (int i = 0; i < original_count; i++) {
        int idx = (eq->head + i) % EQUEUE_CAPACITY;
        if (eq->events[idx].filter == filter && eq->events[idx].ident == ident) {
            continue;
        }
        if (write_idx != i) {
            eq->events[(eq->head + write_idx) % EQUEUE_CAPACITY] = eq->events[idx];
        }
        write_idx++;
    }
    eq->count = write_idx;
    eq->tail = (eq->head + write_idx) % EQUEUE_CAPACITY;
}

int ps4_equeue_remove_events(OrbisKernelEqueue id, uint64_t ident, int16_t filter) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return -EINVAL;
    pthread_mutex_lock(&eq->mutex);
    remove_events_locked(eq, ident, filter);
    pthread_mutex_unlock(&eq->mutex);
    eq_release(eq);
    return 0;
}

// Moves up to max FIFO events to out and returns their count.
static int drain_fifo(KernelEqueueInternal *eq, OrbisKernelEvent *out, int max) {
    pthread_mutex_lock(&eq->mutex);
    int n = 0;
    while (eq->count > 0 && n < max) {
        out[n++] = eq->events[eq->head];
        eq->head = (eq->head + 1) % EQUEUE_CAPACITY;
        eq->count--;
    }
    pthread_mutex_unlock(&eq->mutex);
    return n;
}

static int fifo_count(KernelEqueueInternal *eq) {
    pthread_mutex_lock(&eq->mutex);
    int n = eq->count;
    pthread_mutex_unlock(&eq->mutex);
    return n;
}

// Copies the host events to Orbis events, dropping the wake knote. Returns the number of events
// written to out; *saw_wake reports whether the wake was among them.
static int convert_host_events(const struct kevent64_s *in, int n, OrbisKernelEvent *out, int *saw_wake) {
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (in[i].filter == EVFILT_USER && in[i].ident == EQUEUE_WAKE_IDENT) {
            *saw_wake = 1;
            continue;
        }
        OrbisKernelEvent *e = &out[kept++];
        e->ident = in[i].ident;
        e->filter = in[i].filter;
        e->flags = in[i].flags & (uint16_t)~HOST_ACTION_FLAGS;
        e->fflags = in[i].fflags;
        e->data = in[i].data;
        e->udata = (void *)(uintptr_t)in[i].udata;
    }
    return kept;
}

static int64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// Host events taken per kevent64() call; a wait may return fewer events than are ready, as
// kevent() does.
#define HOST_EVENTS_PER_WAIT 16

int sceKernelWaitEqueue(OrbisKernelEqueue id, OrbisKernelEvent *eventsOut, int numEvents, int *outCount, const uint32_t *timeout) {
    if (outCount) *outCount = 0;
    if (numEvents <= 0) return (int)ORBIS_KERNEL_ERROR_EINVAL;
    if (!eventsOut) return (int)ORBIS_KERNEL_ERROR_EFAULT;

    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return (int)ORBIS_KERNEL_ERROR_EBADF;

    const int64_t deadline = timeout ? monotonic_us() + (int64_t)*timeout : 0;
    int rc;
    for (;;) {
        int n = drain_fifo(eq, eventsOut, numEvents);

        // Collect host knotes: poll when there is something to return already or the time is up;
        // otherwise block until a knote fires, a post wakes us or the timeout passes. A zero
        // timespec costs XNU ~12 us on an empty queue; KEVENT_FLAG_IMMEDIATE ~0.1 us.
        struct kevent64_s host[HOST_EVENTS_PER_WAIT];
        int room = numEvents - n < HOST_EVENTS_PER_WAIT ? numEvents - n : HOST_EVENTS_PER_WAIT;
        int m = 0;
        if (room > 0) {
            struct timespec ts = {0, 0};
            const struct timespec *tsp = NULL;
            unsigned int kflags = 0;
            int64_t rem = timeout ? deadline - monotonic_us() : 1;
            if (n > 0 || rem <= 0) {
                kflags = KEVENT_FLAG_IMMEDIATE;
            } else if (timeout) {
                ts.tv_sec = (time_t)(rem / 1000000);
                ts.tv_nsec = (long)(rem % 1000000) * 1000;
                tsp = &ts;
            }
            m = kevent64(eq->kq, NULL, 0, host, room, kflags, tsp);
            if (m < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "[equeue] kevent64 on eq %d failed: %s\n", eq->id, strerror(errno));
                abort();
            }
        }
        int saw_wake = 0;
        m = convert_host_events(host, m, eventsOut + n, &saw_wake);

        int deleted = __atomic_load_n(&eq->is_deleted, __ATOMIC_ACQUIRE);
        if (saw_wake && (deleted || fifo_count(eq) > 0)) {
            // The wake is edge-triggered and one waiter takes it: pass it on while other waiters
            // still have something to see.
            eq_wake(eq);
        }
        if (deleted) {
            rc = (int)ORBIS_KERNEL_ERROR_EBADF;
            break;
        }
        if (n + m > 0) {
            if (outCount) *outCount = n + m;
            rc = 0;
            break;
        }
        if (timeout && monotonic_us() >= deadline) {
            rc = (int)ORBIS_KERNEL_ERROR_ETIMEDOUT;
            break;
        }
    }
    eq_release(eq);
    return rc;
}

// Registers, changes or deletes one host knote. Returns 0 or the Orbis error.
static int eq_change(OrbisKernelEqueue id, uint64_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
                     int64_t data, void *udata) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return (int)ORBIS_KERNEL_ERROR_EBADF;
    struct kevent64_s kev;
    EV_SET64(&kev, ident, filter, flags, fflags, data, (uint64_t)(uintptr_t)udata, 0, 0);
    int rc = kevent64(eq->kq, &kev, 1, NULL, 0, 0, NULL) == 0 ? 0 : orbis_error(errno);
    eq_release(eq);
    return rc;
}

// EVFILT_TIMER: periodic, implicitly EV_CLEAR; the returned data counts the expirations since the
// last retrieval. NOTE_CRITICAL keeps XNU from coalescing the timer (tens of ms late otherwise).
// Unverified against Orbis: the FreeBSD 9 base of Orbis keeps EVFILT_TIMER periods in ms, so a
// period that is not a whole number of ms may be rounded there; the host keeps it exact (GR2 only
// passes whole ms). Re-adding an existing timer restarts its period on XNU; FreeBSD 9 keeps the
// pending expiry and applies the new period after it.
int sceKernelAddTimerEvent(OrbisKernelEqueue eq, int id, uint32_t usec, void *udata) {
    return eq_change(eq, (uint64_t)(int64_t)id, EVFILT_TIMER, EV_ADD, NOTE_USECONDS | NOTE_CRITICAL,
                     (int64_t)usec, udata);
}

int sceKernelDeleteTimerEvent(OrbisKernelEqueue eq, int id) {
    return eq_change(eq, (uint64_t)(int64_t)id, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
}

// size != 0 sets NOTE_LOWAT with data = size (the low-water mark), as libkernel is reported to do
// (shadPS4); not verified against libkernel.prx.
static int add_fd_event(OrbisKernelEqueue eq, int fd, int16_t filter, size_t size, void *udata) {
    if (fd < 0) return (int)ORBIS_KERNEL_ERROR_EBADF;
    return eq_change(eq, (uint64_t)fd, filter, EV_ADD, size != 0 ? NOTE_LOWAT : 0, (int64_t)size, udata);
}

static int delete_fd_event(OrbisKernelEqueue eq, int fd, int16_t filter) {
    if (fd < 0) return (int)ORBIS_KERNEL_ERROR_EBADF;
    return eq_change(eq, (uint64_t)fd, filter, EV_DELETE, 0, 0, NULL);
}

int sceKernelAddReadEvent(OrbisKernelEqueue eq, int fd, size_t size, void *udata) {
    return add_fd_event(eq, fd, EVFILT_READ, size, udata);
}

int sceKernelDeleteReadEvent(OrbisKernelEqueue eq, int fd) {
    return delete_fd_event(eq, fd, EVFILT_READ);
}

int sceKernelAddWriteEvent(OrbisKernelEqueue eq, int fd, size_t size, void *udata) {
    return add_fd_event(eq, fd, EVFILT_WRITE, size, udata);
}

int sceKernelDeleteWriteEvent(OrbisKernelEqueue eq, int fd) {
    return delete_fd_event(eq, fd, EVFILT_WRITE);
}

int sceKernelAddUserEvent(OrbisKernelEqueue id, int ident) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return (int)ORBIS_KERNEL_ERROR_EBADF;
    eq_release(eq);
    return 0;
}

int sceKernelAddUserEventEdge(OrbisKernelEqueue id, int ident) {
    return sceKernelAddUserEvent(id, ident);
}

int sceKernelTriggerUserEvent(OrbisKernelEqueue id, int ident, void *udata) {
    return ps4_equeue_post_event(id, (uint64_t)ident, ORBIS_KERNEL_EVFILT_USER, 0, udata);
}

int sceKernelDeleteUserEvent(OrbisKernelEqueue id, int ident) {
    KernelEqueueInternal *eq = eq_acquire(id);
    if (!eq) return (int)ORBIS_KERNEL_ERROR_EBADF;
    pthread_mutex_lock(&eq->mutex);
    remove_events_locked(eq, (uint64_t)ident, ORBIS_KERNEL_EVFILT_USER);
    pthread_mutex_unlock(&eq->mutex);
    eq_release(eq);
    return 0;
}

uint64_t sceKernelGetEventId(const OrbisKernelEvent *ev) {
    if (!ev) return 0;
    return ev->ident;
}

int sceKernelGetEventFilter(const OrbisKernelEvent *ev) {
    if (!ev) return 0;
    return (int)ev->filter;
}

void *sceKernelGetEventUserData(const OrbisKernelEvent *ev) {
    return ev ? ev->udata : NULL;
}

int64_t sceKernelGetEventData(const OrbisKernelEvent *ev) {
    return ev ? ev->data : 0;
}

// Shims for guest execution

void shim_sceKernelCreateEqueue(GuestContext *ctx) {
    uint64_t eqOutGuest = ctx->rdi;
    uint64_t nameGuest = ctx->rsi;
    const char *name = nameGuest ? (const char *)(ctx->mem_base + nameGuest) : NULL;

    OrbisKernelEqueue eq = 0;
    int rc = sceKernelCreateEqueue(eqOutGuest ? &eq : NULL, name);
    if (rc == 0) {
        *(int32_t *)(ctx->mem_base + eqOutGuest) = eq;
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelDeleteEqueue(GuestContext *ctx) {
    OrbisKernelEqueue eq = (OrbisKernelEqueue)ctx->rdi;
    ctx->rax = (uint64_t)(int64_t)sceKernelDeleteEqueue(eq);
    SHIM_RETURN();
}

void shim_sceKernelWaitEqueue(GuestContext *ctx) {
    OrbisKernelEqueue eq = (OrbisKernelEqueue)ctx->rdi;
    uint64_t eventsOutGuest = ctx->rsi;
    int numEvents = (int)ctx->rdx;
    uint64_t outCountGuest = ctx->rcx;
    uint64_t timeoutGuest = ctx->r8;

    OrbisKernelEvent *eventsOut = eventsOutGuest ? (OrbisKernelEvent *)(ctx->mem_base + eventsOutGuest) : NULL;
    int *outCount = outCountGuest ? (int *)(ctx->mem_base + outCountGuest) : NULL;
    const uint32_t *timeout = timeoutGuest ? (const uint32_t *)(ctx->mem_base + timeoutGuest) : NULL;

    ctx->rax = (uint64_t)(int64_t)sceKernelWaitEqueue(eq, eventsOut, numEvents, outCount, timeout);
    SHIM_RETURN();
}

// udata of every event is an opaque guest value that comes back verbatim in the guest-visible
// OrbisKernelEvent; it stays a guest address, never a host pointer.

void shim_sceKernelAddTimerEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelAddTimerEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi,
                                                         (uint32_t)ctx->rdx, (void *)(uintptr_t)ctx->rcx);
    SHIM_RETURN();
}

void shim_sceKernelDeleteTimerEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelDeleteTimerEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi);
    SHIM_RETURN();
}

void shim_sceKernelAddReadEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelAddReadEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi,
                                                        (size_t)ctx->rdx, (void *)(uintptr_t)ctx->rcx);
    SHIM_RETURN();
}

void shim_sceKernelDeleteReadEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelDeleteReadEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi);
    SHIM_RETURN();
}

void shim_sceKernelAddWriteEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelAddWriteEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi,
                                                         (size_t)ctx->rdx, (void *)(uintptr_t)ctx->rcx);
    SHIM_RETURN();
}

void shim_sceKernelDeleteWriteEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelDeleteWriteEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi);
    SHIM_RETURN();
}

void shim_sceKernelAddUserEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelAddUserEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi);
    SHIM_RETURN();
}

void shim_sceKernelAddUserEventEdge(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelAddUserEventEdge((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi);
    SHIM_RETURN();
}

void shim_sceKernelTriggerUserEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelTriggerUserEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi,
                                                            (void *)(uintptr_t)ctx->rdx);
    SHIM_RETURN();
}

void shim_sceKernelDeleteUserEvent(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelDeleteUserEvent((OrbisKernelEqueue)ctx->rdi, (int)ctx->rsi);
    SHIM_RETURN();
}

static const OrbisKernelEvent *guest_event(GuestContext *ctx) {
    return (ctx->rdi && ctx->mem_base) ? (const OrbisKernelEvent *)(ctx->mem_base + ctx->rdi) : NULL;
}

void shim_sceKernelGetEventId(GuestContext *ctx) {
    ctx->rax = sceKernelGetEventId(guest_event(ctx));
    SHIM_RETURN();
}

void shim_sceKernelGetEventFilter(GuestContext *ctx) {
    ctx->rax = (uint64_t)(int64_t)sceKernelGetEventFilter(guest_event(ctx));
    SHIM_RETURN();
}

void shim_sceKernelGetEventUserData(GuestContext *ctx) {
    ctx->rax = (uint64_t)(uintptr_t)sceKernelGetEventUserData(guest_event(ctx));
    SHIM_RETURN();
}

void shim_sceKernelGetEventData(GuestContext *ctx) {
    ctx->rax = (uint64_t)sceKernelGetEventData(guest_event(ctx));
    SHIM_RETURN();
}
