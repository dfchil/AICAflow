#include <aicaflow/firmware.h>
#include "host_internal.h"

uint32_t g_dynamic_base, g_asset_limit;
afx_block_t *g_free_blocks, *g_allocs;
uint32_t g_free_count, g_free_capacity, g_alloc_count, g_alloc_capacity;
afx_asset_slot_t *g_assets;
uint32_t g_asset_capacity;
afx_instance_slot_t g_instances[AFX_MAX_FLOW_SLOTS];
uint64_t g_available_channels = UINT64_MAX;
uint64_t g_map_used[AFX_CHANNEL_MAP_ARENAS];
bool g_dsp_scene;
uint32_t g_dsp_ring;
uint16_t g_dsp_return_left, g_dsp_return_right;
uint32_t g_next_sequence = 1;
uint32_t g_queue_head; /* Only the SH-4 advances head, so it need not be read back. */
uint32_t g_queue_tail_seen; /* The last tail read back; see enqueue(). */
bool g_ready, g_lifecycle;
volatile bool g_upload_dma_done;
afx_asset_t g_upload_dma_asset;
uint32_t g_upload_dma_bytes, g_upload_dma_offset;
bool g_upload_dma_stream;
mutex_t g_host_mutex = RECURSIVE_MUTEX_INITIALIZER;
uint32_t const g_spu_base = AFX_SPU_RAM_BASE_SH4;

/* The AICA's main-CPU interrupt (see AFX_AICA_MCIPD) runs a KOS threaded
 * handler: in interrupt context only the ASIC event is masked; a KOS worker
 * thread then clears the interrupt, drains the event ring, takes each reported
 * slot's observation, recycles instances that ended and reports them, and
 * unmasks. The program makes no call for any of it. KOS calls the masking
 * part again for every other ASIC interrupt while the line is raised; that
 * only masks again and wakes the worker, which coalesces. KOS hooks
 * ASIC_EVT_SPU_IRQ nowhere itself; a program that does must not use AICAflow. */
#define AICA_REG_SH4(offset) (0xa0700000u + (offset))
static volatile bool g_host_irq_seen;
volatile bool g_events_running;
static uint32_t g_event_tail, g_event_overflows;
static void host_irq_mask(uint16_t code) {
    asic_evt_disable(code, ASIC_IRQ_DEFAULT);
    g_host_irq_seen = true;
}
static void host_irq_unmask(uint16_t code) {
    asic_evt_enable(code, ASIC_IRQ_DEFAULT);
}
/* Slots with news since the last call, from the event ring; every slot after
 * an overflow. */
static uint64_t events_take(void) {
    uint32_t head = read_spu_word(AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, event_head));
    uint32_t overflows = read_spu_word(AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, event_overflows));
    uint64_t changed = 0;
    if (overflows != g_event_overflows || head - g_event_tail > AFX_EVENT_CAPACITY) {
        g_event_overflows = overflows;
        changed = UINT64_MAX;
    } else if (head != g_event_tail) {
        uint32_t words[AFX_EVENT_CAPACITY / 4];
        for (uint32_t i = 0; i < AFX_EVENT_CAPACITY / 4; ++i)
            words[i] = read_spu_word(AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, events) + i * 4u);
        for (uint32_t at = g_event_tail; at != head; ++at) {
            uint32_t slot = at & (AFX_EVENT_CAPACITY - 1u);
            uint32_t index = (words[slot / 4u] >> ((slot & 3u) * 8u)) & 0xffu;
            if (index < AFX_MAX_FLOW_SLOTS) changed |= UINT64_C(1) << index;
        }
    }
    if (head != g_event_tail) {
        g_event_tail = head;
        g2_write_32(g_spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, event_tail), head);
    }
    return changed;
}
static void host_events(uint32_t code, void *data) {
    (void)code; (void)data;
    /* Blocking: an upload may hold the lock for a while, and this thread can
     * wait it out where the program's calls cannot. */
    mutex_lock(&g_host_mutex);
    g_events_running = true;
    /* Clear before reading, and read the register back so the clear has landed:
     * news published before it is then visible to the reads that follow, and
     * news published after it raises the interrupt again. */
    g2_write_32(AICA_REG_SH4(AFX_AICA_MCIRE), AFX_AICA_MCI_SCPU);
    (void)g2_read_32(AICA_REG_SH4(AFX_AICA_MCIPD));
    if (g_ready && g_lifecycle) {
        uint64_t changed = events_take();
        for (uint32_t index = 0; index < AFX_MAX_FLOW_SLOTS; ++index)
            if (g_instances[index].live &&
                ((changed >> index) & 1u || g_instances[index].recycle_wanted)) (void)update_slot(index);
    }
    g_events_running = false;
    mutex_unlock(&g_host_mutex);
    events_report_ended();
}
static void host_irq_stop(void) {
    asic_evt_disable(ASIC_EVT_SPU_IRQ, ASIC_IRQ_DEFAULT);
    asic_evt_remove_handler(ASIC_EVT_SPU_IRQ); /* Also ends the worker thread. */
    g2_write_32(AICA_REG_SH4(AFX_AICA_MCIEB), 0);
    g2_write_32(AICA_REG_SH4(AFX_AICA_MCIRE), AFX_AICA_MCI_SCPU);
    g_host_irq_seen = false;
}
/* The firmware raises the interrupt once after booting: unmask, and wait for
 * it to arrive. */
static bool host_irq_start(void) {
    g_host_irq_seen = false;
    g_event_tail = g_event_overflows = 0;
    if (asic_evt_request_threaded_handler(ASIC_EVT_SPU_IRQ, host_events, NULL,
                                          host_irq_mask, host_irq_unmask)) return false;
    g2_write_32(AICA_REG_SH4(AFX_AICA_MCIEB), AFX_AICA_MCI_SCPU);
    asic_evt_enable(ASIC_EVT_SPU_IRQ, ASIC_IRQ_DEFAULT);
    uint64_t started = timer_ms_gettime64();
    while (!g_host_irq_seen && timer_ms_gettime64() - started < 100) thd_pass();
    if (!g_host_irq_seen) host_irq_stop();
    return g_host_irq_seen;
}
uint32_t new_sequence(void) {
    if (!g_next_sequence) ++g_next_sequence;
    return g_next_sequence++;
}
int enqueue(uint32_t opcode, uint32_t reference, uint32_t sequence,
                   uint32_t flags, const void *payload, uint32_t payload_size) {
    if (!g_ready || !g_lifecycle || payload_size > 48) return -AFX_UNSUPPORTED;
    /* The AICA sees the command only after the G2 FIFO has drained. This is
     * the same payload-then-head discipline as KOS's sound command queue. */
    g2_lock_scoped();
    int result = AFX_OK;
    uint32_t head = g_queue_head;
    /* Only the ARM7 advances tail, so the last value read is a lower bound on
     * it: room seen then is still there. Tail is read back over G2 only when
     * that view has no room, once per AFX_CMD_QUEUE_CAPACITY commands at most. */
    if (head - g_queue_tail_seen >= AFX_CMD_QUEUE_CAPACITY)
        g_queue_tail_seen = g2_read_32_raw(g_spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail));
    uint32_t tail = g_queue_tail_seen;
    if (head - tail > AFX_CMD_QUEUE_CAPACITY) result = -AFX_BAD_FIRMWARE;
    else if (head - tail == AFX_CMD_QUEUE_CAPACITY) result = -AFX_IPC_FULL;
    else {
        afx_cmd_t command = { .opcode = opcode, .sequence = sequence,
                              .reference = reference, .flags = flags };
        if (payload_size) memcpy(command.payload, payload, payload_size);
        uint32_t address = AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, commands) +
                           (head & (AFX_CMD_QUEUE_CAPACITY - 1u)) * sizeof(command);
        const uint32_t *words = (const uint32_t *)&command;
        for (uint32_t i = 0; i < sizeof(command) / 4; ++i) {
            if (!(i & 7u)) g2_fifo_wait();
            g2_write_32_raw(g_spu_base + address + i * 4, words[i]);
        }
        g2_fifo_wait();
        g2_write_32_raw(g_spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, head), head + 1u);
        g_queue_head = head + 1u;
    }
    return result;
}
bool read_observed(uint32_t index, uint32_t *last_epoch, afx_instance_status_t *out) {
    if (index >= AFX_MAX_FLOW_SLOTS || !last_epoch || !out) return false;
    uint32_t base = AFX_OBSERVED_ADDR + index * sizeof(afx_observed_t);
    uint32_t first = read_spu_word(base);
    if ((first & 1u) || first == *last_epoch) return false;
    afx_instance_status_t copy = {
        .reference = read_spu_word(base + 4), .state = read_spu_word(base + 8),
        .sequence = read_spu_word(base + 12), .result = read_spu_word(base + 16),
        .position = read_spu_word(base + 20), .next_deadline = read_spu_word(base + 24),
        .detail = read_spu_word(base + 28)
    };
    if (first != read_spu_word(base) || (first & 1u)) return false;
    *out = copy;
    *last_epoch = first;
    return true;
}
static void stop_driver(void) {
    host_irq_stop();
    spu_disable();
    /* ARM reset does not stop DSP. Remove memory accesses and drain the DSP
     * pipeline before its former ring can become firmware or asset memory. */
    g2_write_32(0xa0702000u, 0);
    g2_write_32(0xa0702004u, 0);
    for (uint32_t step = 0; step < 128; ++step)
        g2_write_32(0xa0703408u + step * 16u, 2);
    thd_sleep(2);
}
int afx_init(const void *firmware, uint32_t size) {
    afx_firmware_info_t info;
    afx_result_t valid = afx_firmware_validate(firmware, size, &info);
    if (valid) return -(int)valid; /* No reset on rejected image. */
    g_ready = g_lifecycle = false;
    stop_driver();
    /* Assets are uploaded before use and a DSP ring is filled when its scene
     * is programmed. Only the control region needs a known state: a stale
     * status block must not look like a booted driver. */
    spu_memset(AFX_CONTROL_BASE, 0, AFX_AICA_RAM_SIZE - AFX_CONTROL_BASE);
    g_queue_head = g_queue_tail_seen = 0;
    upload_words(0, firmware, size);
    spu_enable();
    uint64_t started = timer_ms_gettime64();
    while (timer_ms_gettime64() - started < 1000) {
        uint32_t capabilities = read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, capabilities));
        if (read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, magic)) == AFX_STATUS_MAGIC &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, abi)) == AFX_ABI_VERSION &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, layout_id)) == AFX_LAYOUT_ID &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, asset_base)) == info.asset_base &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, asset_limit)) == info.asset_limit &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, private_end)) == info.private_end &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, stack_base)) == info.stack_base &&
            read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, error)) == AFX_OK &&
            (capabilities & AFX_CAP_BOOTSTRAP)) {
            /* Status reaches the host only through the interrupt. */
            if (!host_irq_start()) { spu_disable(); return -AFX_TIMEOUT; }
            if (!allocator_reset(info.asset_base)) { stop_driver(); return -AFX_NO_HOST_RAM; }
            maps_reset();
            g_ready = true;
            g_lifecycle = (capabilities & AFX_CAP_LIFECYCLE) != 0;
            return AFX_OK;
        }
    }
    spu_disable();
    g_dynamic_base = 0;
    return -AFX_TIMEOUT;
}
void afx_shutdown(void) {
    g_ready = g_lifecycle = false;
    stop_driver();
    (void)allocator_reset(0);
}
uint32_t afx_status_heartbeat(void) {
    return read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, heartbeat));
}
uint32_t afx_status_timer_ticks(void) {
    return read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, timer_ticks));
}
uint32_t afx_status_max_lateness(void) {
    return read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, max_lateness));
}
