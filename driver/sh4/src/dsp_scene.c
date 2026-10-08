#include "host_internal.h"

static uint16_t dsp_word(const uint8_t *program, uint32_t offset) {
    return program[offset] | (uint16_t)program[offset + 1u] << 8;
}
static uint32_t ring_bytes(uint8_t rbl) {
    return rbl == AFX_DSP_RING_NONE ? 0 : AFX_DSP_MIN_BYTES << rbl;
}
static int scene_command(uint32_t opcode, uint32_t bytes, bool host_init) {
    afx_dsp_payload_t payload = {0};
    if (opcode == AFX_CMD_DSP_ENABLE) {
        payload.ring_address = g_dsp_ring;
        payload.ring_bytes = bytes;
    }
    uint32_t sequence = new_sequence();
    int result = enqueue(opcode, AFX_DSP_SCENE_REFERENCE, sequence, 1u | (host_init ? AFX_DSP_FLAG_HOST_INIT : 0u), &payload, sizeof(payload));
    if (result) return result;
    /* A timed-out enable may still execute: pin its allocation until DISABLE
     * is acknowledged, rather than returning live DSP memory to the allocator. */
    if (opcode == AFX_CMD_DSP_ENABLE) g_dsp_scene = true;
    for (unsigned waited = 0; waited < 2000; ++waited) {
        if (read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_sequence)) == sequence) {
            uint32_t status = read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_result));
            if (opcode == AFX_CMD_DSP_ENABLE && status) g_dsp_scene = false;
            return status ? -(int)status : AFX_OK;
        }
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}
static int dsp_scene_program(const void *data, uint32_t bytes, uint8_t rbl) {
    HOST_GUARD(-AFX_BUSY);
    const uint8_t *program = data;
    if (!program || bytes != AFX_DSP_PROGRAM_BYTES || rbl > 3) return -AFX_BAD_COMMAND;
    int memory_format = -1;
    for (uint32_t step = 0; step < 128; ++step) {
        uint16_t w2 = dsp_word(program, step * 8u + 4u);
        if ((w2 & 0x8000u) || (!(step & 1u) && (w2 & 0x6000u)) ||
            ((w2 & 0x1000u) && ((w2 >> 8) & 15u) > 1u)) return -AFX_BAD_COMMAND;
        if (w2 & 0x6000u) {
            int nofl = (dsp_word(program, step * 8u + 6u) >> 15) & 1u;
            if (memory_format >= 0 && memory_format != nofl) return -AFX_BAD_COMMAND;
            memory_format = nofl;
        }
    }
    if ((dsp_word(program, 1408) | dsp_word(program, 1410)) & ~0x0f1fu)
        return -AFX_BAD_COMMAND;
    if (memory_format < 0) rbl = AFX_DSP_RING_NONE;
    uint32_t bytes_reserved = ring_bytes(rbl);
    if (memory_format >= 0) {
        uint32_t words = bytes_reserved / 2u;
        for (uint32_t address = 0; address < AFX_DSP_ADDRESSES; ++address)
            if (dsp_word(program, 1024u + AFX_DSP_COEFFICIENTS * 2u + address * 2u) >= words)
                return -AFX_BAD_BOUNDS;
    }
    if (g_dsp_scene || g_dsp_ring) {
        int result = afx_dsp_scene_disable();
        if (result) return result;
    }
    if (bytes_reserved) {
        g_dsp_ring = afx_mem_alloc(bytes_reserved, AFX_DSP_RING_ALIGN);
        if (!g_dsp_ring) return -AFX_NO_AICA_RAM;
    }
    bool host_init = bytes_reserved &&
        (read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, capabilities)) & AFX_CAP_DSP_HOST_INIT);
    int result = scene_command(AFX_CMD_DSP_ENABLE, bytes_reserved, host_init);
    if (result) {
        if (!g_dsp_scene && g_dsp_ring && free_allocation(g_dsp_ring)) g_dsp_ring = 0;
        return result;
    }
    g2_write_32(0xa0702000u, 0);
    g2_write_32(0xa0702004u, 0);
    /* Remove all memory/output writes before replacing their operands. */
    for (uint32_t i = 0; i < 128; ++i) g2_write_32(0xa0703408u + i * 16u, 2);
    thd_sleep(2);
    if (bytes_reserved && (host_init || memory_format == 0)) {
        /* DSP is NOP and the pipeline has drained. Fill RAM in bounded blocks;
         * the legacy firmware has already supplied linear zero contents. */
        _Alignas(32) uint32_t fill[1024];
        uint32_t value = memory_format == 0 ? 0x60006000u : 0;
        for (uint32_t i = 0; i < 1024; ++i) fill[i] = value;
        for (uint32_t offset = 0; offset < bytes_reserved; offset += sizeof(fill))
            upload_words(g_dsp_ring + offset, fill, sizeof(fill));
        g2_fifo_wait();
    }
    for (uint32_t i = 0; i < 128; ++i)
        g2_write_32(0xa0703000u + i * 4u, dsp_word(program, 1024u + i * 2u));
    for (uint32_t i = 0; i < 64; ++i)
        g2_write_32(0xa0703200u + i * 4u, dsp_word(program, 1280u + i * 2u));
    for (uint32_t i = 0; i < 512; ++i)
        g2_write_32(0xa0703400u + i * 4u, dsp_word(program, i * 2u));
    for (uint32_t i = 0; i < 512; ++i)
        if ((g2_read_32(0xa0703400u + i * 4u) & 0xffffu) != dsp_word(program, i * 2u))
            return -AFX_BAD_COMMAND;
    thd_sleep(2);
    g_dsp_return_left = dsp_word(program, 1408);
    g_dsp_return_right = dsp_word(program, 1410);
    g2_write_32(0xa0702000u, g_dsp_return_left);
    g2_write_32(0xa0702004u, g_dsp_return_right);
    return AFX_OK;
}
int afx_dsp_scene_program(const void *data, uint32_t bytes) {
    return dsp_scene_program(data, bytes, 3);
}
int afx_dsp_scene_program_ring(const void *data, uint32_t bytes, uint8_t rbl) {
    return dsp_scene_program(data, bytes, rbl);
}
int afx_dsp_scene_returns(bool enabled) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_dsp_scene) return -AFX_BUSY;
    g2_write_32(0xa0702000u, enabled ? g_dsp_return_left : 0);
    g2_write_32(0xa0702004u, enabled ? g_dsp_return_right : 0);
    return AFX_OK;
}
int afx_dsp_scene_disable(void) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_dsp_scene && !g_dsp_ring) return -AFX_BUSY;
    if (g_dsp_scene) {
        int result = scene_command(AFX_CMD_DSP_DISABLE, 0, false);
        if (result) return result;
    }
    g_dsp_scene = false;
    g_dsp_return_left = g_dsp_return_right = 0;
    if (g_dsp_ring && !free_allocation(g_dsp_ring)) return -AFX_NO_HOST_RAM;
    g_dsp_ring = 0;
    return AFX_OK;
}
