/* The KOS calls of the SH4 library, implemented on the software AICA.
 * See docs/design/host-backend.md: one bus lock guards the chip; rendering is
 * the clock while a renderer runs, and the wall clock otherwise; the SH4
 * interrupt goes to a worker thread, or, without threads, to the next point
 * where the library's lock is free. */
#include "../shim/platform_shim.h"
#include "../emu/aica.h"

#include <aicaflow/host_audio.h>

#include <stdbool.h>
#include <string.h>
#include <time.h>

#if AFX_HOST_THREADS
#include <pthread.h>
#include <sched.h>
#endif

#ifndef AFX_HOST_ARM_CYCLES_PER_SAMPLE
#define AFX_HOST_ARM_CYCLES_PER_SAMPLE 128u
#endif
/* ARM7 instructions an access runs when no time has passed, so a polling
 * loop sees the firmware answer. */
#define AFX_HOST_POLL_INSTRUCTIONS 512u
/* Without a renderer the chip catches up with the wall clock, at most this
 * much at once. */
#define AFX_HOST_CATCH_UP_MS 100u
/* The renderer takes the lock for at most this many samples at a time. */
#define AFX_HOST_RENDER_SLICE 32u
/* A renderer is running if it rendered this recently. */
#define AFX_HOST_RENDER_FRESH_MS 50u

#define SH4_REGISTERS 0xa0700000u
#define SH4_RAM 0xa0800000u

static aica_t g_chip;
static bool g_chip_ready;
static uint64_t g_last_render_ms, g_last_advance_us;
static void (*g_start_hook)(void);
static bool g_started;

/* The SH4 interrupt: KOS's threaded-handler contract. */
static afx_host_asic_handler_t g_irq_handler;
static void (*g_irq_ack)(uint16_t), (*g_irq_unmask)(uint16_t);
static bool g_irq_enabled, g_irq_worker_pending, g_irq_acking, g_irq_handling;

#if AFX_HOST_THREADS
static pthread_mutex_t g_bus = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_irq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_irq_wake = PTHREAD_COND_INITIALIZER;
/* Each registration starts a worker with a new generation; a removed
 * handler's worker leaves when it sees a newer one. It is detached, not
 * joined: it may be waiting for the library's mutex, held by the caller. */
static unsigned g_irq_generation;
#define BUS_LOCK() pthread_mutex_lock(&g_bus)
#define BUS_UNLOCK() pthread_mutex_unlock(&g_bus)
#define IRQ_LOCK() pthread_mutex_lock(&g_irq_lock)
#define IRQ_UNLOCK() pthread_mutex_unlock(&g_irq_lock)
#else
#define BUS_LOCK() ((void)0)
#define BUS_UNLOCK() ((void)0)
#define IRQ_LOCK() ((void)0)
#define IRQ_UNLOCK() ((void)0)
#endif

uint64_t afx_host_timer_ms_gettime64(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Callers hold the bus lock. */
static aica_t *chip(void) {
    if (!g_chip_ready) {
        aica_init(&g_chip);
        g_chip.cycles_per_sample = AFX_HOST_ARM_CYCLES_PER_SAMPLE;
        g_chip_ready = true;
    }
    return &g_chip;
}

static uint64_t now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* The AICA runs whether or not anyone listens. With no renderer running, an
 * access first advances it by the time that has passed, and discards the
 * sound. Returns whether the ARM7 ran. Callers hold the bus lock. */
static bool catch_up(aica_t *c) {
    uint64_t now = now_us();
    if (now / 1000u - g_last_render_ms < AFX_HOST_RENDER_FRESH_MS) return false;
    if (!g_last_advance_us || now < g_last_advance_us) g_last_advance_us = now;
    uint64_t elapsed = now - g_last_advance_us;
    if (elapsed > AFX_HOST_CATCH_UP_MS * 1000u) {
        g_last_advance_us = now - AFX_HOST_CATCH_UP_MS * 1000u;
        elapsed = AFX_HOST_CATCH_UP_MS * 1000u;
    }
    uint32_t frames = (uint32_t)(elapsed * AICA_RATE / 1000000u);
    if (!frames) {
        aica_run_cpu(c, AFX_HOST_POLL_INSTRUCTIONS);
        return true;
    }
    static int16_t discard[2 * 256];
    for (uint32_t left = frames; left;) {
        uint32_t n = left < 256u ? left : 256u;
        aica_render(c, discard, n);
        left -= n;
    }
    g_last_advance_us += (uint64_t)frames * 1000000u / AICA_RATE;
    return true;
}

static void irq_deliver_inline(void);

/* With the line raised and the event unmasked, run the acknowledgement (it
 * masks the event) and wake the worker. Called without the bus lock. */
static void irq_check(bool line) {
    IRQ_LOCK();
    if (!line || !g_irq_enabled || !g_irq_handler || g_irq_acking) {
        IRQ_UNLOCK();
        return;
    }
    g_irq_acking = true;
    void (*ack)(uint16_t) = g_irq_ack;
    IRQ_UNLOCK();
    if (ack) ack(0x0101);
    else afx_host_asic_disable(0x0101, 9);
    IRQ_LOCK();
    g_irq_acking = false;
    g_irq_worker_pending = true;
#if AFX_HOST_THREADS
    pthread_cond_signal(&g_irq_wake);
#endif
    IRQ_UNLOCK();
    irq_deliver_inline();
}

static bool line_now(void) {
    BUS_LOCK();
    bool line = aica_main_irq(chip());
    BUS_UNLOCK();
    return line;
}

#if AFX_HOST_THREADS
static void *irq_worker(void *argument) {
    unsigned generation = (unsigned)(uintptr_t)argument;
    IRQ_LOCK();
    for (;;) {
        while (!g_irq_worker_pending && generation == g_irq_generation)
            pthread_cond_wait(&g_irq_wake, &g_irq_lock);
        if (generation != g_irq_generation) break;
        g_irq_worker_pending = false;
        g_irq_handling = true;
        afx_host_asic_handler_t handler = g_irq_handler;
        void (*unmask)(uint16_t) = g_irq_unmask;
        IRQ_UNLOCK();
        if (handler) handler(0x0101, NULL);
        if (unmask) unmask(0x0101);
        else afx_host_asic_enable(0x0101, 9);
        IRQ_LOCK();
        g_irq_handling = false;
    }
    IRQ_UNLOCK();
    return NULL;
}
static void irq_deliver_inline(void) {}
#else
#include <kos/mutex.h>
/* The library's lock (driver/sh4/src/ipc.c). */
extern mutex_t g_host_mutex;
static bool g_irq_in_handler;
/* Without threads the handler runs here, and only between library calls: on
 * KOS its worker would wait for the library's lock. */
static void irq_deliver_inline(void) {
    while (g_irq_worker_pending && g_irq_handler && !g_irq_in_handler && !g_host_mutex.depth) {
        g_irq_worker_pending = false;
        g_irq_in_handler = true;
        g_irq_handler(0x0101, NULL);
        g_irq_in_handler = false;
        if (g_irq_unmask) g_irq_unmask(0x0101);
        else afx_host_asic_enable(0x0101, 9);
    }
}
#endif

int afx_host_asic_request_threaded_handler(uint16_t code, afx_host_asic_handler_t handler, void *data,
                                           void (*ack_and_mask)(uint16_t), void (*unmask)(uint16_t)) {
    (void)code;
    if (!handler || data) return -1;
    IRQ_LOCK();
    if (g_irq_handler) { IRQ_UNLOCK(); return -1; }
    g_irq_handler = handler;
    g_irq_ack = ack_and_mask;
    g_irq_unmask = unmask;
    g_irq_worker_pending = false;
#if AFX_HOST_THREADS
    pthread_t thread;
    unsigned generation = ++g_irq_generation;
    if (pthread_create(&thread, NULL, irq_worker, (void *)(uintptr_t)generation)) {
        g_irq_handler = NULL;
        IRQ_UNLOCK();
        return -1;
    }
    pthread_detach(thread);
#endif
    IRQ_UNLOCK();
    return 0;
}

void afx_host_asic_remove_handler(uint16_t code) {
    (void)code;
    IRQ_LOCK();
    g_irq_handler = NULL;
    g_irq_enabled = false;
    g_irq_worker_pending = false;
#if AFX_HOST_THREADS
    ++g_irq_generation;
    pthread_cond_broadcast(&g_irq_wake);
#endif
    IRQ_UNLOCK();
}

void afx_host_asic_enable(uint16_t code, uint8_t level) {
    (void)code; (void)level;
    IRQ_LOCK();
    g_irq_enabled = true;
    IRQ_UNLOCK();
    irq_check(line_now());
}

void afx_host_asic_disable(uint16_t code, uint8_t level) {
    (void)code; (void)level;
    IRQ_LOCK();
    g_irq_enabled = false;
    IRQ_UNLOCK();
}

/* A library wait: the chip catches up, and its interrupt is delivered. The
 * ARM7 runs even while a renderer is active, so a wait that cannot sleep
 * (a browser) still sees the firmware answer. */
static void wait_step(void) {
    BUS_LOCK();
    aica_t *c = chip();
    if (!catch_up(c)) aica_run_cpu(c, AFX_HOST_POLL_INSTRUCTIONS);
    bool line = aica_main_irq(c);
    BUS_UNLOCK();
    irq_check(line);
    irq_deliver_inline();
}

uint32_t afx_host_g2_read_32(uintptr_t address) {
    BUS_LOCK();
    aica_t *c = chip();
    (void)catch_up(c);
    uint32_t value = 0;
    if (address - SH4_RAM < AICA_RAM_BYTES) {
        const uint8_t *p = c->ram + ((address - SH4_RAM) & ~3u);
        value = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    } else if (address - SH4_REGISTERS < AICA_REG_BYTES) {
        value = aica_reg_read(c, (uint32_t)(address - SH4_REGISTERS));
    }
    bool line = aica_main_irq(c);
    BUS_UNLOCK();
    irq_check(line);
    return value;
}

void afx_host_g2_write_32(uintptr_t address, uint32_t value) {
    BUS_LOCK();
    aica_t *c = chip();
    if (address - SH4_RAM < AICA_RAM_BYTES) {
        uint8_t *p = c->ram + ((address - SH4_RAM) & ~3u);
        p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
        p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
    } else if (address - SH4_REGISTERS < AICA_REG_BYTES) {
        aica_reg_write(c, (uint32_t)(address - SH4_REGISTERS), value);
    }
    bool line = aica_main_irq(c);
    BUS_UNLOCK();
    irq_check(line);
}

void afx_host_spu_memload(uintptr_t address, const void *source, size_t size) {
    BUS_LOCK();
    aica_t *c = chip();
    if (address <= AICA_RAM_BYTES && size <= AICA_RAM_BYTES - address)
        memcpy(c->ram + address, source, size);
    BUS_UNLOCK();
}

void afx_host_spu_memset(uintptr_t address, uint32_t value, size_t size) {
    BUS_LOCK();
    aica_t *c = chip();
    if (address <= AICA_RAM_BYTES && size <= AICA_RAM_BYTES - address) {
        for (size_t i = 0; i + 4 <= size; i += 4) {
            uint8_t *p = c->ram + address + i;
            p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
            p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
        }
    }
    BUS_UNLOCK();
}

int afx_host_spu_dma_transfer(const void *source, uintptr_t address, size_t size, int block,
                              afx_host_dma_callback_t callback, void *data) {
    (void)block;
    afx_host_spu_memload(address, source, size);
    if (callback) callback(data);
    return 0;
}

void afx_host_spu_disable(void) {
    BUS_LOCK();
    aica_t *c = chip();
    aica_key_off_all(c);
    aica_arm_reset(c, true);
    BUS_UNLOCK();
}

void afx_host_spu_enable(void) {
    BUS_LOCK();
    aica_arm_reset(chip(), false);
    bool first = !g_started;
    g_started = true;
    void (*hook)(void) = g_start_hook;
    BUS_UNLOCK();
    if (first && hook) hook();
}

void afx_host_thd_pass(void) {
    wait_step();
#if AFX_HOST_THREADS
    sched_yield();
#endif
}

void afx_host_thd_sleep(uint32_t milliseconds) {
#if AFX_HOST_THREADS
    struct timespec pause = { milliseconds / 1000u, (long)(milliseconds % 1000u) * 1000000L };
    nanosleep(&pause, NULL);
#else
    (void)milliseconds;
#endif
    wait_step();
}

#if !AFX_HOST_THREADS
/* Without threads: called as a library call takes its lock from outside. */
void afx_host_poll(void) {
    static bool polling;
    if (polling) return;
    polling = true;
    wait_step();
    polling = false;
}
#endif

/* Not public API. For tests that render faster than real time: waits until
 * the interrupt worker has handled every raised interrupt, as a KOS sleep
 * would let it. */
void afx_host_settle(void) {
    for (int tries = 0; tries < 100000; ++tries) {
        bool line = line_now();
        irq_check(line);
        irq_deliver_inline();
        IRQ_LOCK();
        bool busy = g_irq_worker_pending || g_irq_acking || g_irq_handling ||
                    (line && g_irq_enabled && g_irq_handler);
        IRQ_UNLOCK();
        if (!busy) return;
#if AFX_HOST_THREADS
        sched_yield();
#endif
    }
}

/* Not public API. For tests and tools: the chip itself, unlocked; use it only
 * while nothing renders. */
aica_t *afx_host_debug_chip(void) { return chip(); }

uint32_t afx_host_audio_rate(void) { return AICA_RATE; }

void afx_host_audio_render(int16_t *stereo, uint32_t frames) {
    while (frames) {
        uint32_t slice = frames < AFX_HOST_RENDER_SLICE ? frames : AFX_HOST_RENDER_SLICE;
        BUS_LOCK();
        aica_t *c = chip();
        aica_render(c, stereo, slice);
        g_last_advance_us = now_us();
        g_last_render_ms = g_last_advance_us / 1000u;
        bool line = aica_main_irq(c);
        BUS_UNLOCK();
        irq_check(line);
        stereo += slice * 2u;
        frames -= slice;
    }
    irq_deliver_inline();
}

void afx_host_audio_set_start_hook(void (*hook)(void)) {
    BUS_LOCK();
    g_start_hook = hook;
    bool started = g_started;
    BUS_UNLOCK();
    if (started && hook) hook();
}
