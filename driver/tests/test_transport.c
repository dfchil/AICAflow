#include <aicaflow/host.h>
#include <aicaflow/bank.h>
#include <aicaflow/codec.h>
#include <dc/spu.h>
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
static unsigned asset_reads, observed_reads;
static uint64_t clock_ms;
static const void *dma_source;
static uintptr_t dma_address;
static size_t dma_size;
static unsigned dma_transfers;
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
void spu_disable(void) { running = 0; ++resets; }
void spu_memload(uintptr_t address, const void *source, size_t size) {
    assert(!(address & 3) && !((uintptr_t)source & 3) && !(size & 3));
    assert(address <= sizeof(ram) && size <= sizeof(ram) - address);
    if (running) assert((address >= asset_base && address <= AFX_ASSET_MAX &&
                         size <= AFX_ASSET_MAX - address) ||
                        (address >= AFX_CHANNEL_MAP_ARENA_ADDR && address <= AFX_PRIVATE_BASE &&
                         size <= AFX_PRIVATE_BASE - address));
    memcpy(ram + address, source, size);
}
void spu_memset(uintptr_t address, uint32_t value, size_t size) {
    assert(!running && value == 0 && address == 0 && size == sizeof(ram));
    for (uint32_t step = 0; step < 128; ++step)
        assert(!(afx_read32(dsp_registers + 0x3408 + step * 16) & 0x6000));
    memset(ram, 0, size);
}
int spu_dma_transfer(const void *source, uintptr_t address, size_t size, int block,
                     spu_dma_callback_t callback, void *data) {
    assert(!dma_source && source && !(address & 31u) && !((uintptr_t)source & 31u) &&
           !(size & 31u));
    assert(!block && callback);
    ++dma_transfers;
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
    if (address >= 0xa0700000u && address - 0xa0700000u <= sizeof(dsp_registers) - 4) {
        assert(!(address & 3));
        afx_write32(dsp_registers + (address - 0xa0700000u), value);
        return;
    }
    assert(address >= 0xa0800000 && address - 0xa0800000 <= sizeof(ram) - 4);
    assert(!(address & 3));
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
    afx_write32(s + offsetof(afx_status_t, capabilities), AFX_CAP_BOOTSTRAP | AFX_CAP_LIFECYCLE);
    afx_write32(s + offsetof(afx_status_t, asset_base), asset_base);
    afx_write32(s + offsetof(afx_status_t, asset_limit), AFX_ASSET_MAX);
    afx_write32(s + offsetof(afx_status_t, private_end), afx_read32(m + 24));
    afx_write32(s + offsetof(afx_status_t, stack_base), AFX_STACK_BASE);
}
static void observed(uint32_t index, uint32_t reference, uint32_t state,
                     uint32_t sequence, uint32_t result) {
    uint8_t *record = ram + AFX_OBSERVED_ADDR + index * sizeof(afx_observed_t);
    uint32_t epoch = afx_read32(record) + 1;
    afx_write32(record, epoch | 1u);
    afx_write32(record + 4, reference);
    afx_write32(record + 8, state);
    afx_write32(record + 12, sequence);
    afx_write32(record + 16, result);
    afx_write32(record + 20, 0);
    afx_write32(record + 24, 0);
    afx_write32(record + 28, 0);
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
