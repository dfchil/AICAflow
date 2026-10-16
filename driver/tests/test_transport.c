#include <aicaflow/host.h>
#include <aicaflow/bank.h>
#include <aicaflow/codec.h>
#include <dc/spu.h>
#include <dc/asic.h>
#include <assert.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static uint8_t ram[AFX_AICA_RAM_SIZE];
static uint8_t dsp_registers[0x4000];
static int running, boot_ok = 1;
static unsigned resets;
static uint32_t asset_base;
static unsigned asset_reads, observed_reads, tail_reads;
static uint64_t clock_ms;
static const void *dma_source;
static uintptr_t dma_address;
static size_t dma_size;
static unsigned dma_transfers;
static const uint8_t *expected_dma_source;
static unsigned direct_dma_transfers;
static unsigned asset_word_writes, asset_block_writes;
static unsigned dma_fail_at;
static size_t dma_max_size;
static spu_dma_callback_t dma_callback;
static void *dma_callback_data;
static pthread_mutex_t stall_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stall_condition = PTHREAD_COND_INITIALIZER;
static int stall_write, write_entered, release_write;
static uint32_t sleep_observe_reference;
static int auto_dsp_ack;
static uint32_t dsp_reply_result;
static void observed(uint32_t index, uint32_t reference, uint32_t state,
                     uint32_t sequence, uint32_t result);
/* The AICA's main-CPU interrupt: the line is MCIPD & MCIEB, masked by the ASIC
 * event, and is delivered synchronously the moment all three allow it. */
#define AICA_REG(offset) (0xa0700000u + (offset))
static int irq_boot_raise = 1;
static uint32_t mcipd;
static asic_evt_handler irq_handler;
static int irq_unmasked;
static unsigned irq_deliveries;
static void (*irq_hook_on_clear)(void), (*irq_hook_on_unmask)(void);
static int irq_line(void) {
    return (mcipd & afx_read32(dsp_registers + AFX_AICA_MCIEB) & AFX_AICA_MCI_SCPU) != 0;
}
static void irq_check(void) {
    if (irq_handler && irq_unmasked && irq_line()) {
        ++irq_deliveries;
        irq_handler(ASIC_EVT_SPU_IRQ, NULL);
        /* Raised and unmasked after its handler would interrupt again at once. */
        assert(!irq_unmasked);
    }
}
/* KOS calls every handler whose status bit is set when any ASIC interrupt
 * arrives, masked or not; the SPU line stays set until MCIRE clears it. */
static __attribute__((unused)) void irq_other_asic_event(void) {
    if (irq_handler && irq_line()) {
        irq_handler(ASIC_EVT_SPU_IRQ, NULL);
        assert(!irq_unmasked);
    }
}
static void irq_raise(void) { mcipd |= AFX_AICA_MCI_SCPU; irq_check(); }
/* KOS's threaded handler: the interrupt part masks and wakes a worker thread,
 * which runs the handler, then unmasks. Wakeups before it runs coalesce. The
 * worker runs when a test calls irq_run_worker(), between the library calls a
 * real thread could interleave with. */
static asic_evt_handler worker_handler;
static void (*worker_ack)(uint16_t), (*worker_unmask)(uint16_t);
static int worker_pending;
static unsigned worker_runs;
static void threaded_dispatch(uint32_t code, void *data) {
    (void)data;
    if (worker_ack) worker_ack((uint16_t)code);
    worker_pending = 1;
}
asic_evt_handler_entry_t asic_evt_set_handler(uint16_t code, asic_evt_handler handler, void *data) {
    assert(code == ASIC_EVT_SPU_IRQ && !data);
    asic_evt_handler_entry_t old = { irq_handler, NULL };
    irq_handler = handler;
    return old;
}
int asic_evt_request_threaded_handler(uint16_t code, asic_evt_handler handler, void *data,
                                      void (*ack_and_mask)(uint16_t), void (*unmask)(uint16_t)) {
    assert(code == ASIC_EVT_SPU_IRQ && handler && !data && !irq_handler);
    worker_handler = handler;
    worker_ack = ack_and_mask;
    worker_unmask = unmask;
    worker_pending = 0;
    irq_handler = threaded_dispatch;
    return 0;
}
void asic_evt_remove_handler(uint16_t code) {
    assert(code == ASIC_EVT_SPU_IRQ);
    irq_handler = NULL;
    worker_handler = NULL;
    worker_pending = 0;
}
static __attribute__((unused)) void irq_run_worker(void) {
    while (worker_pending && worker_handler) {
        worker_pending = 0;
        ++worker_runs;
        worker_handler(ASIC_EVT_SPU_IRQ, NULL);
        if (worker_unmask) worker_unmask(ASIC_EVT_SPU_IRQ);
    }
}
void asic_evt_disable(uint16_t code, uint8_t level) {
    assert(code == ASIC_EVT_SPU_IRQ && level == ASIC_IRQ_DEFAULT);
    irq_unmasked = 0;
}
void asic_evt_enable(uint16_t code, uint8_t level) {
    assert(code == ASIC_EVT_SPU_IRQ && level == ASIC_IRQ_DEFAULT);
    if (irq_hook_on_unmask) { void (*hook)(void) = irq_hook_on_unmask; irq_hook_on_unmask = NULL; hook(); }
    irq_unmasked = 1;
    irq_check();
}
void spu_disable(void) { running = 0; ++resets; }
void spu_memload(uintptr_t address, const void *source, size_t size) {
    assert(!(address & 3) && !((uintptr_t)source & 3) && !(size & 3));
    assert(address <= sizeof(ram) && size <= sizeof(ram) - address);
    if (running) assert((address >= asset_base && address <= AFX_ASSET_MAX &&
                         size <= AFX_ASSET_MAX - address) ||
                        (address >= AFX_CHANNEL_MAP_ARENA_ADDR && address <= AFX_PRIVATE_BASE &&
                         size <= AFX_PRIVATE_BASE - address));
    if (running && address >= asset_base && address < AFX_ASSET_MAX) ++asset_block_writes;
    memcpy(ram + address, source, size);
}
void spu_memset(uintptr_t address, uint32_t value, size_t size) {
    assert(!running && value == 0 && address == AFX_CONTROL_BASE &&
           size == sizeof(ram) - AFX_CONTROL_BASE);
    for (uint32_t step = 0; step < 128; ++step)
        assert(!(afx_read32(dsp_registers + 0x3408 + step * 16) & 0x6000));
    memset(ram + address, 0, size);
}
int spu_dma_transfer(const void *source, uintptr_t address, size_t size, int block,
                     spu_dma_callback_t callback, void *data) {
    assert(!dma_source && source && !(address & 31u) && !((uintptr_t)source & 31u) &&
           !(size & 31u));
    assert(!block && callback);
    ++dma_transfers;
    if (source == expected_dma_source) { ++direct_dma_transfers; expected_dma_source += size; }
    if (dma_transfers == dma_fail_at) return -1;
    if (size > dma_max_size) dma_max_size = size;
    dma_source = source; dma_address = address; dma_size = size;
    dma_callback = callback; dma_callback_data = data;
    return 0;
}
static void complete_dma(void) {
    assert(dma_source && dma_callback);
    memcpy(ram + dma_address, dma_source, dma_size);
    dma_source = NULL;
    dma_callback(dma_callback_data);
    dma_callback = NULL;
}
void thd_pass(void) {
    if (dma_source) complete_dma();
}
uint32_t g2_read_32(uintptr_t address) {
    if (address == AICA_REG(AFX_AICA_MCIPD)) return mcipd;
    if (address >= 0xa0700000u && address - 0xa0700000u <= sizeof(dsp_registers) - 4) {
        assert(!(address & 3));
        return afx_read32(dsp_registers + (address - 0xa0700000u));
    }
    assert(address >= 0xa0800000 && address - 0xa0800000 <= sizeof(ram) - 4);
    assert(!(address & 3));
    if (address - 0xa0800000 >= AFX_OBSERVED_ADDR &&
        address - 0xa0800000 < AFX_CHANNEL_MAP_ARENA_ADDR) ++observed_reads;
    if (address - 0xa0800000 >= asset_base && address - 0xa0800000 < AFX_ASSET_MAX)
        ++asset_reads;
    if (address - 0xa0800000 == AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail)) ++tail_reads;
    return afx_read32(ram + (address - 0xa0800000));
}
void g2_write_32(uintptr_t address, uint32_t value) {
    pthread_mutex_lock(&stall_mutex);
    if (stall_write) {
        stall_write = 0;
        write_entered = 1;
        pthread_cond_signal(&stall_condition);
        while (!release_write) pthread_cond_wait(&stall_condition, &stall_mutex);
    }
    pthread_mutex_unlock(&stall_mutex);
    if (address == AICA_REG(AFX_AICA_MCIRE)) {
        mcipd &= ~value;
        if (irq_hook_on_clear) { void (*hook)(void) = irq_hook_on_clear; irq_hook_on_clear = NULL; hook(); }
        return;
    }
    if (address == AICA_REG(AFX_AICA_MCIEB)) {
        afx_write32(dsp_registers + AFX_AICA_MCIEB, value);
        irq_check();
        return;
    }
    if (address >= 0xa0700000u && address - 0xa0700000u <= sizeof(dsp_registers) - 4) {
        assert(!(address & 3));
        afx_write32(dsp_registers + (address - 0xa0700000u), value);
        return;
    }
    assert(address >= 0xa0800000 && address - 0xa0800000 <= sizeof(ram) - 4);
    assert(!(address & 3));
    if (running && address - 0xa0800000 >= asset_base && address - 0xa0800000 < AFX_ASSET_MAX)
        ++asset_word_writes;
    afx_write32(ram + (address - 0xa0800000), value);
}
uint64_t timer_ms_gettime64(void) { clock_ms += 100; return clock_ms; }
void spu_enable(void) {
    running = 1;
    if (!boot_ok) return;
    const uint8_t *m = ram + AFX_FIRMWARE_INFO_OFFSET;
    asset_base = afx_read32(m + 16);
    uint8_t *s = ram + AFX_STATUS_ADDR;
    afx_write32(s + offsetof(afx_status_t, magic), AFX_STATUS_MAGIC);
    afx_write32(s + offsetof(afx_status_t, abi), AFX_ABI_VERSION);
    afx_write32(s + offsetof(afx_status_t, layout_id), AFX_LAYOUT_ID);
    afx_write32(s + offsetof(afx_status_t, capabilities), AFX_CAP_BOOTSTRAP | AFX_CAP_LIFECYCLE | AFX_CAP_DSP_HOST_INIT);
    afx_write32(s + offsetof(afx_status_t, asset_base), asset_base);
    afx_write32(s + offsetof(afx_status_t, asset_limit), AFX_ASSET_MAX);
    afx_write32(s + offsetof(afx_status_t, private_end), afx_read32(m + 24));
    afx_write32(s + offsetof(afx_status_t, stack_base), AFX_STACK_BASE);
    if (irq_boot_raise) irq_raise();
}
static void observed(uint32_t index, uint32_t reference, uint32_t state,
                     uint32_t sequence, uint32_t result) {
    uint8_t *record = ram + AFX_OBSERVED_ADDR + index * sizeof(afx_observed_t);
    uint32_t epoch = afx_read32(record) + 1;
    /* As the firmware does: news is a change beyond position and deadline. */
    int news = afx_read32(record + 4) != reference || afx_read32(record + 8) != state ||
               afx_read32(record + 12) != sequence || afx_read32(record + 16) != result;
    uint8_t *queue = ram + AFX_QUEUE_ADDR;
    afx_write32(record, epoch | 1u);
    afx_write32(record + 4, reference);
    afx_write32(record + 8, state);
    afx_write32(record + 12, sequence);
    afx_write32(record + 16, result);
    afx_write32(record + 20, 0);
    afx_write32(record + 24, 0);
    afx_write32(record + 28, 0);
    afx_write32(record, (epoch | 1u) + 1u);
    if (news) {
        uint32_t head = afx_read32(queue + offsetof(afx_cmd_queue_t, event_head));
        if (head - afx_read32(queue + offsetof(afx_cmd_queue_t, event_tail)) < AFX_EVENT_CAPACITY) {
            queue[offsetof(afx_cmd_queue_t, events) + (head & (AFX_EVENT_CAPACITY - 1u))] = (uint8_t)index;
            afx_write32(queue + offsetof(afx_cmd_queue_t, event_head), head + 1u);
        } else {
            uint32_t overflows = afx_read32(queue + offsetof(afx_cmd_queue_t, event_overflows));
            afx_write32(queue + offsetof(afx_cmd_queue_t, event_overflows), overflows + 1u);
        }
        irq_raise();
    }
}
/* A RUNNING record republished with a new position only: not news. */
static __attribute__((unused)) void observed_position(uint32_t index, uint32_t position) {
    uint8_t *record = ram + AFX_OBSERVED_ADDR + index * sizeof(afx_observed_t);
    uint32_t epoch = afx_read32(record) + 1;
    afx_write32(record, epoch | 1u);
    afx_write32(record + 20, position);
    afx_write32(record, (epoch | 1u) + 1u);
}
void thd_sleep(uint32_t milliseconds) {
    (void)milliseconds;
    if (!sleep_observe_reference && !auto_dsp_ack) return;
    uint32_t head = afx_read32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, head));
    afx_cmd_t command;
    memcpy(&command, ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, commands) +
           ((head - 1u) & (AFX_CMD_QUEUE_CAPACITY - 1u)) * sizeof(command), sizeof(command));
    uint32_t reference = sleep_observe_reference ? sleep_observe_reference : command.reference;
    sleep_observe_reference = 0;
    if (reference == AFX_DSP_SCENE_REFERENCE) {
        afx_write32(ram + AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_result), dsp_reply_result);
        afx_write32(ram + AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_sequence), command.sequence);
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), head);
    } else observed(0, reference, AFX_RUNNING, command.sequence, AFX_OK);
}
static __attribute__((unused)) afx_cmd_t queued(uint32_t index) {
    afx_cmd_t command;
    memcpy(&command, ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, commands) +
           (index & (AFX_CMD_QUEUE_CAPACITY - 1u)) * sizeof(command), sizeof(command));
    return command;
}
static __attribute__((unused)) uint32_t queue_head(void) {
    return afx_read32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, head));
}
