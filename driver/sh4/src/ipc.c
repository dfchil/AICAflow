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
uint32_t g_reserved_peak_commands, g_reserved_peak_writes;
bool g_dsp_scene;
uint16_t g_dsp_return_left, g_dsp_return_right;
uint32_t g_next_sequence = 1;
bool g_ready, g_lifecycle;
volatile bool g_upload_dma_done;
afx_asset_t g_upload_dma_asset;
uint32_t g_upload_dma_bytes, g_upload_dma_offset;
bool g_upload_dma_stream;
mutex_t g_host_mutex = RECURSIVE_MUTEX_INITIALIZER;
uint32_t const g_spu_base = AFX_SPU_RAM_BASE_SH4;

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
    uint32_t head = g2_read_32_raw(g_spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, head));
    uint32_t tail = g2_read_32_raw(g_spu_base + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail));
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
    }
    return result;
}
bool read_observed(uint32_t index, afx_instance_status_t *out) {
    if (index >= AFX_MAX_FLOW_SLOTS || !out) return false;
    uint32_t base = AFX_OBSERVED_ADDR + index * sizeof(afx_observed_t);
    uint32_t first = read_spu_word(base);
    if (first & 1u) return false;
    afx_instance_status_t copy = {
        .reference = read_spu_word(base + 4), .state = read_spu_word(base + 8),
        .sequence = read_spu_word(base + 12), .result = read_spu_word(base + 16),
        .position = read_spu_word(base + 20), .next_deadline = read_spu_word(base + 24),
        .detail = read_spu_word(base + 28)
    };
    if (first != read_spu_word(base) || (first & 1u)) return false;
    *out = copy;
    return true;
}
int afx_init(const void *firmware, uint32_t size) {
    afx_firmware_info_t info;
    afx_result_t valid = afx_firmware_validate(firmware, size, &info);
    if (valid) return -(int)valid; /* No reset on rejected image. */
    g_ready = g_lifecycle = false;
    spu_disable();
    spu_memset(0, 0, AFX_AICA_RAM_SIZE);
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
            if (!allocator_reset(info.asset_base)) { spu_disable(); return -AFX_NO_HOST_RAM; }
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
    spu_disable();
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
