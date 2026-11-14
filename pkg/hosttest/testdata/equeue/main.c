// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host test of the Orbis event queue (pkg/runtime/kernel/equeue/ps4_equeue.c): host-kqueue knotes
// (EVFILT_TIMER, EVFILT_READ, EVFILT_WRITE, EVFILT_USER), runtime-posted FIFO events, waking and
// deleting while threads wait, and the guest shims. Driven by equeue_test.go: one case per run,
// named by argv[1]; prints PASS or FAIL lines, exits non-zero on failure.
#include "ps4_equeue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);                                 \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            g_fail = 1;                                                                            \
        }                                                                                          \
    } while (0)

#define EV_CLEAR_FLAG 0x20
#define EV_EOF_FLAG 0x8000

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static OrbisKernelEqueue make_eq(void) {
    OrbisKernelEqueue eq = 0;
    int rc = sceKernelCreateEqueue(&eq, "test");
    CHECK(rc == 0 && eq > 0, "create rc=%#x eq=%d", rc, eq);
    return eq;
}

static int poll_eq(OrbisKernelEqueue eq, OrbisKernelEvent *ev, int n) {
    uint32_t zero = 0;
    int out = -1;
    int rc = sceKernelWaitEqueue(eq, ev, n, &out, &zero);
    if (rc == (int)ORBIS_KERNEL_ERROR_ETIMEDOUT) {
        CHECK(out == 0, "out=%d on timeout", out);
        return 0;
    }
    CHECK(rc == 0, "poll rc=%#x", rc);
    return out;
}

// A periodic timer: fires every period, reports EV_CLEAR and counts the expirations it missed.
static void case_timer_periodic(void) {
    OrbisKernelEqueue eq = make_eq();
    CHECK(sceKernelAddTimerEvent(eq, 5, 10000, (void *)0x1234) == 0, "add");
    OrbisKernelEvent ev;
    int out = 0;
    double t0 = now_ms();
    for (int i = 0; i < 5; i++) {
        int rc = sceKernelWaitEqueue(eq, &ev, 1, &out, NULL);
        CHECK(rc == 0 && out == 1, "wait rc=%#x out=%d", rc, out);
        CHECK(ev.filter == ORBIS_KERNEL_EVFILT_TIMER && ev.ident == 5, "filter=%d ident=%llu", ev.filter,
              (unsigned long long)ev.ident);
        CHECK(ev.flags == EV_CLEAR_FLAG, "flags=%#x", ev.flags);
        CHECK(ev.fflags == 0, "fflags=%#x", ev.fflags);
        CHECK(ev.data == 1, "data=%lld", (long long)ev.data);
        CHECK(ev.udata == (void *)0x1234, "udata=%p", ev.udata);
    }
    double dt = now_ms() - t0;
    CHECK(dt >= 49 && dt < 60, "5 periods of 10 ms took %.2f ms", dt);

    // Expirations missed while nobody waits are counted, not lost: data = elapsed periods.
    double t1 = now_ms();
    usleep(35000);
    CHECK(poll_eq(eq, &ev, 1) == 1, "missed expirations pending");
    double slept = now_ms() - t1;
    long long expect = (long long)(slept / 10);
    CHECK(ev.data >= expect - 1 && ev.data <= expect + 1, "data=%lld after %.2f ms", (long long)ev.data, slept);
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

static void case_timer_delete(void) {
    OrbisKernelEqueue eq = make_eq();
    CHECK(sceKernelAddTimerEvent(eq, -3, 5000, NULL) == 0, "add");
    usleep(12000);
    CHECK(sceKernelDeleteTimerEvent(eq, -3) == 0, "delete");
    OrbisKernelEvent ev;
    CHECK(poll_eq(eq, &ev, 1) == 0, "a deleted timer reports nothing");
    CHECK(sceKernelDeleteTimerEvent(eq, -3) == (int)ORBIS_KERNEL_ERROR_ENOENT, "second delete");
    CHECK(sceKernelAddTimerEvent(eq + 1000, 1, 5000, NULL) == (int)ORBIS_KERNEL_ERROR_EBADF, "bad eq");
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
    CHECK(sceKernelAddTimerEvent(eq, 1, 5000, NULL) == (int)ORBIS_KERNEL_ERROR_EBADF, "deleted eq");
}

// Read/write readiness of a socket: data is the byte count / space, EOF when the peer closes.
static void case_fd_socket(void) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    OrbisKernelEqueue eq = make_eq();
    CHECK(sceKernelAddReadEvent(eq, sv[0], 1, (void *)0x10) == 0, "add read");
    OrbisKernelEvent ev[4];
    CHECK(poll_eq(eq, ev, 4) == 0, "nothing to read yet");

    CHECK(sceKernelAddWriteEvent(eq, sv[0], 1, (void *)0x20) == 0, "add write");
    int n = poll_eq(eq, ev, 4);
    CHECK(n == 1 && ev[0].filter == ORBIS_KERNEL_EVFILT_WRITE && ev[0].ident == (uint64_t)sv[0],
          "n=%d filter=%d", n, ev[0].filter);
    CHECK(ev[0].data > 0 && ev[0].flags == 0 && ev[0].udata == (void *)0x20, "data=%lld flags=%#x",
          (long long)ev[0].data, ev[0].flags);
    CHECK(sceKernelDeleteWriteEvent(eq, sv[0]) == 0, "delete write");

    CHECK(write(sv[1], "abc", 3) == 3, "write");
    n = poll_eq(eq, ev, 4);
    CHECK(n == 1 && ev[0].filter == ORBIS_KERNEL_EVFILT_READ && ev[0].data == 3 && ev[0].flags == 0 &&
              ev[0].udata == (void *)0x10,
          "n=%d filter=%d data=%lld flags=%#x", n, ev[0].filter, (long long)ev[0].data, ev[0].flags);

    // A low-water mark above the bytes available keeps the read event quiet.
    CHECK(sceKernelAddReadEvent(eq, sv[0], 8, (void *)0x10) == 0, "re-add read with size 8");
    CHECK(poll_eq(eq, ev, 4) == 0, "3 bytes are below the low-water mark");

    close(sv[1]);
    n = poll_eq(eq, ev, 4);
    CHECK(n == 1 && ev[0].filter == ORBIS_KERNEL_EVFILT_READ && (ev[0].flags & EV_EOF_FLAG), "n=%d flags=%#x", n,
          ev[0].flags);
    CHECK(sceKernelDeleteReadEvent(eq, sv[0]) == 0, "delete read");
    CHECK(sceKernelDeleteReadEvent(eq, sv[0]) == (int)ORBIS_KERNEL_ERROR_ENOENT, "second delete");
    close(sv[0]);
    CHECK(sceKernelAddReadEvent(eq, sv[0], 0, NULL) == (int)ORBIS_KERNEL_ERROR_EBADF, "closed fd");
    CHECK(sceKernelAddWriteEvent(eq, -1, 0, NULL) == (int)ORBIS_KERNEL_ERROR_EBADF, "negative fd");
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

typedef struct {
    OrbisKernelEqueue eq;
    int rc;
    int out;
    OrbisKernelEvent ev;
} Waiter;

static void *waiter_main(void *arg) {
    Waiter *w = arg;
    w->rc = sceKernelWaitEqueue(w->eq, &w->ev, 1, &w->out, NULL);
    return NULL;
}

// Runtime-posted events wake a thread blocked in the host kqueue; two posts wake two waiters.
static void case_post_wakes(void) {
    OrbisKernelEqueue eq = make_eq();
    Waiter w[2] = {{.eq = eq, .rc = -1}, {.eq = eq, .rc = -1}};
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, waiter_main, &w[i]);
    usleep(20000);
    CHECK(ps4_equeue_post_event(eq, 6, ORBIS_KERNEL_EVFILT_VIDEO_OUT, 77, (void *)0x99) == 0, "post 1");
    CHECK(ps4_equeue_trigger_event(eq, 1, ORBIS_KERNEL_EVFILT_GRAPHICS_CORE, 1, NULL) == 0, "post 2");
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    int saw_flip = 0, saw_eop = 0;
    for (int i = 0; i < 2; i++) {
        CHECK(w[i].rc == 0 && w[i].out == 1, "waiter %d rc=%#x out=%d", i, w[i].rc, w[i].out);
        if (w[i].ev.filter == ORBIS_KERNEL_EVFILT_VIDEO_OUT) {
            saw_flip = w[i].ev.ident == 6 && w[i].ev.data == 77 && w[i].ev.udata == (void *)0x99;
        }
        if (w[i].ev.filter == ORBIS_KERNEL_EVFILT_GRAPHICS_CORE) saw_eop = 1;
    }
    CHECK(saw_flip && saw_eop, "flip=%d eop=%d", saw_flip, saw_eop);
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

// One wait returns both FIFO events and host knotes.
static void case_mixed(void) {
    OrbisKernelEqueue eq = make_eq();
    CHECK(sceKernelAddTimerEvent(eq, 9, 1000, NULL) == 0, "add timer");
    usleep(5000);
    CHECK(ps4_equeue_post_event(eq, 6, ORBIS_KERNEL_EVFILT_VIDEO_OUT, 0, NULL) == 0, "post");
    OrbisKernelEvent ev[4];
    int n = poll_eq(eq, ev, 4);
    CHECK(n == 2 && ev[0].filter == ORBIS_KERNEL_EVFILT_VIDEO_OUT && ev[1].filter == ORBIS_KERNEL_EVFILT_TIMER,
          "n=%d filters=%d,%d", n, ev[0].filter, ev[1].filter);
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

static void case_timeout(void) {
    OrbisKernelEqueue eq = make_eq();
    OrbisKernelEvent ev;
    uint32_t t = 20000;
    int out = -1;
    double t0 = now_ms();
    int rc = sceKernelWaitEqueue(eq, &ev, 1, &out, &t);
    double dt = now_ms() - t0;
    CHECK(rc == (int)ORBIS_KERNEL_ERROR_ETIMEDOUT && out == 0, "rc=%#x out=%d", rc, out);
    CHECK(dt >= 19.9 && dt < 30, "timed out after %.2f ms", dt);
    CHECK(sceKernelWaitEqueue(eq, &ev, 0, &out, &t) == (int)ORBIS_KERNEL_ERROR_EINVAL, "numEvents 0");
    CHECK(sceKernelWaitEqueue(eq + 1000, &ev, 1, &out, &t) == (int)ORBIS_KERNEL_ERROR_EBADF, "bad eq");
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
    CHECK(sceKernelDeleteEqueue(eq) == (int)ORBIS_KERNEL_ERROR_EBADF, "second delete");
}

// Deleting a queue returns EBADF to every thread waiting on it.
static void case_delete_wakes(void) {
    OrbisKernelEqueue eq = make_eq();
    Waiter w[3];
    pthread_t th[3];
    for (int i = 0; i < 3; i++) {
        w[i] = (Waiter){.eq = eq, .rc = -1};
        pthread_create(&th[i], NULL, waiter_main, &w[i]);
    }
    usleep(20000);
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
    for (int i = 0; i < 3; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].rc == (int)ORBIS_KERNEL_ERROR_EBADF, "waiter %d rc=%#x", i, w[i].rc);
    }
    // The slot is free again once the waiters are gone.
    for (int i = 0; i < 200; i++) {
        OrbisKernelEqueue e = make_eq();
        CHECK(sceKernelDeleteEqueue(e) == 0, "delete %d", i);
    }
}

// A level user event stays returned until it is deleted; a trigger sets the udata.
static void case_user_level(void) {
    OrbisKernelEqueue eq = make_eq();
    CHECK(sceKernelAddUserEvent(eq, 3) == 0, "add");
    OrbisKernelEvent ev[2];
    CHECK(poll_eq(eq, ev, 2) == 0, "not triggered yet");
    CHECK(sceKernelTriggerUserEvent(eq, 3, (void *)0x41) == 0, "trigger");
    CHECK(sceKernelTriggerUserEvent(eq, 3, (void *)0x42) == 0, "trigger again");
    for (int i = 0; i < 2; i++) {
        int n = poll_eq(eq, ev, 2);
        CHECK(n == 1 && ev[0].filter == ORBIS_KERNEL_EVFILT_USER && ev[0].ident == 3, "n=%d filter=%d", n,
              ev[0].filter);
        CHECK(ev[0].flags == 0 && ev[0].fflags == 0 && ev[0].data == 0 && ev[0].udata == (void *)0x42,
              "flags=%#x fflags=%#x data=%lld udata=%p", ev[0].flags, ev[0].fflags, (long long)ev[0].data,
              ev[0].udata);
    }
    // Gravity Rush 2's reactor re-arms a received user event by deleting and adding it again.
    CHECK(sceKernelDeleteUserEvent(eq, 3) == 0, "delete");
    CHECK(sceKernelAddUserEvent(eq, 3) == 0, "re-add");
    CHECK(poll_eq(eq, ev, 2) == 0, "re-added event is not triggered");
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

// An edge user event (EV_CLEAR) is returned once per trigger and wakes a blocked waiter.
static void case_user_edge(void) {
    OrbisKernelEqueue eq = make_eq();
    CHECK(sceKernelAddUserEventEdge(eq, -2) == 0, "add");
    Waiter w = {.eq = eq, .rc = -1};
    pthread_t th;
    pthread_create(&th, NULL, waiter_main, &w);
    usleep(20000);
    CHECK(sceKernelTriggerUserEvent(eq, -2, (void *)0x77) == 0, "trigger");
    pthread_join(th, NULL);
    CHECK(w.rc == 0 && w.out == 1 && w.ev.filter == ORBIS_KERNEL_EVFILT_USER, "rc=%#x out=%d filter=%d", w.rc,
          w.out, w.ev.filter);
    CHECK(w.ev.ident == (uint64_t)(int64_t)-2 && w.ev.flags == EV_CLEAR_FLAG && w.ev.udata == (void *)0x77,
          "ident=%#llx flags=%#x udata=%p", (unsigned long long)w.ev.ident, w.ev.flags, w.ev.udata);
    OrbisKernelEvent ev;
    CHECK(poll_eq(eq, &ev, 1) == 0, "cleared after one return");
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

static void case_user_errors(void) {
    OrbisKernelEqueue eq = make_eq();
    CHECK(sceKernelTriggerUserEvent(eq, 5, NULL) == (int)ORBIS_KERNEL_ERROR_ENOENT, "trigger unregistered");
    CHECK(sceKernelDeleteUserEvent(eq, 5) == (int)ORBIS_KERNEL_ERROR_ENOENT, "delete unregistered");
    CHECK(sceKernelAddUserEvent(eq + 1000, 5) == (int)ORBIS_KERNEL_ERROR_EBADF, "bad eq");
    CHECK(sceKernelTriggerUserEvent(eq + 1000, 5, NULL) == (int)ORBIS_KERNEL_ERROR_EBADF, "trigger bad eq");
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

// The guest shims: arguments in SysV registers, udata a guest value, events in guest memory.
static void case_shims(void) {
    static uint8_t mem[4096];
    GuestContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mem_base = mem;
    ctx.rsp = 0x800;

    ctx.rdi = 0x100; // eq out
    ctx.rsi = 0;
    shim_sceKernelCreateEqueue(&ctx);
    CHECK(ctx.rax == 0, "create rax=%#llx", (unsigned long long)ctx.rax);
    OrbisKernelEqueue eq = *(int32_t *)(mem + 0x100);

    ctx.rdi = (uint64_t)eq;
    ctx.rsi = 4;
    ctx.rdx = 2000;
    ctx.rcx = 0x8000001234ULL; // guest udata
    shim_sceKernelAddTimerEvent(&ctx);
    CHECK(ctx.rax == 0, "add timer rax=%#llx", (unsigned long long)ctx.rax);

    *(uint32_t *)(mem + 0x110) = 100000; // timeout
    ctx.rdi = (uint64_t)eq;
    ctx.rsi = 0x200; // events
    ctx.rdx = 1;
    ctx.rcx = 0x108; // out count
    ctx.r8 = 0x110;
    shim_sceKernelWaitEqueue(&ctx);
    CHECK(ctx.rax == 0 && *(int *)(mem + 0x108) == 1, "wait rax=%#llx", (unsigned long long)ctx.rax);

    ctx.rdi = 0x200;
    shim_sceKernelGetEventFilter(&ctx);
    CHECK((int)ctx.rax == ORBIS_KERNEL_EVFILT_TIMER, "filter=%d", (int)ctx.rax);
    ctx.rdi = 0x200;
    shim_sceKernelGetEventId(&ctx);
    CHECK(ctx.rax == 4, "id=%llu", (unsigned long long)ctx.rax);
    ctx.rdi = 0x200;
    shim_sceKernelGetEventUserData(&ctx);
    CHECK(ctx.rax == 0x8000001234ULL, "udata=%#llx", (unsigned long long)ctx.rax);

    ctx.rdi = (uint64_t)eq;
    ctx.rsi = 4;
    shim_sceKernelDeleteTimerEvent(&ctx);
    CHECK(ctx.rax == 0, "delete timer");
    shim_sceKernelDeleteTimerEvent(&ctx);
    CHECK((uint32_t)ctx.rax == ORBIS_KERNEL_ERROR_ENOENT, "second delete rax=%#llx", (unsigned long long)ctx.rax);
    CHECK(ctx.rsp == 0x800 + 8 * 8, "every shim pops its return address: rsp=%#llx", (unsigned long long)ctx.rsp);
    CHECK(sceKernelDeleteEqueue(eq) == 0, "delete eq");
}

typedef struct {
    OrbisKernelEqueue a, b;
    int rounds;
} PingPong;

static void *pong_main(void *arg) {
    PingPong *p = arg;
    OrbisKernelEvent ev;
    int out;
    for (int i = 0; i < p->rounds; i++) {
        sceKernelWaitEqueue(p->a, &ev, 1, &out, NULL);
        ps4_equeue_post_event(p->b, 6, ORBIS_KERNEL_EVFILT_VIDEO_OUT, i, NULL);
    }
    return NULL;
}

// Not a check: the round-trip latency of a post that wakes a blocked waiter (the flip/EOP path).
static void case_bench(void) {
    PingPong p = {make_eq(), make_eq(), 20000};
    pthread_t th;
    pthread_create(&th, NULL, pong_main, &p);
    OrbisKernelEvent ev;
    int out;
    double t0 = now_ms();
    for (int i = 0; i < p.rounds; i++) {
        ps4_equeue_post_event(p.a, 6, ORBIS_KERNEL_EVFILT_VIDEO_OUT, i, NULL);
        sceKernelWaitEqueue(p.b, &ev, 1, &out, NULL);
    }
    double dt = now_ms() - t0;
    pthread_join(th, NULL);
    printf("post+wake round trip: %.2f us\n", dt * 1000 / p.rounds);
    uint32_t zero = 0;
    t0 = now_ms();
    for (int i = 0; i < 100000; i++) sceKernelWaitEqueue(p.a, &ev, 1, &out, &zero);
    printf("empty poll: %.3f us\n", (now_ms() - t0) * 1000 / 100000);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const char *c = argv[1];
    if (!strcmp(c, "timer-periodic")) case_timer_periodic();
    else if (!strcmp(c, "timer-delete")) case_timer_delete();
    else if (!strcmp(c, "fd-socket")) case_fd_socket();
    else if (!strcmp(c, "post-wakes")) case_post_wakes();
    else if (!strcmp(c, "mixed")) case_mixed();
    else if (!strcmp(c, "timeout")) case_timeout();
    else if (!strcmp(c, "delete-wakes")) case_delete_wakes();
    else if (!strcmp(c, "user-level")) case_user_level();
    else if (!strcmp(c, "user-edge")) case_user_edge();
    else if (!strcmp(c, "user-errors")) case_user_errors();
    else if (!strcmp(c, "shims")) case_shims();
    else if (!strcmp(c, "bench")) case_bench();
    else {
        printf("FAIL unknown case %s\n", c);
        return 2;
    }
    if (g_fail) return 1;
    printf("PASS %s\n", c);
    return 0;
}
