#include "host_internal.h"

static uint16_t dsp_word(const uint8_t *program, uint32_t offset) {
    return program[offset] | (uint16_t)program[offset + 1u] << 8;
}
static int scene_command(uint32_t opcode, uint32_t flags) {
    uint32_t sequence = new_sequence();
    int result = enqueue(opcode, AFX_DSP_SCENE_REFERENCE, sequence, flags, NULL, 0);
    if (result) return result;
    for (unsigned waited = 0; waited < 2000; ++waited) {
        if (read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_sequence)) == sequence) {
            uint32_t status = read_spu_word(AFX_STATUS_ADDR + offsetof(afx_status_t, dsp_result));
            return status ? -(int)status : AFX_OK;
        }
        thd_sleep(1);
    }
    return -AFX_TIMEOUT;
}
int afx_dsp_scene_enable(void) {
    HOST_GUARD(-AFX_BUSY);
    if (g_dsp_scene) return -AFX_BUSY;
    int result = scene_command(AFX_CMD_DSP_ENABLE, 0);
    if (!result) g_dsp_scene = true;
    return result;
}
int afx_dsp_scene_prepare(void) {
    HOST_GUARD(-AFX_BUSY);
    if (g_dsp_scene) return -AFX_BUSY;
    int result = scene_command(AFX_CMD_DSP_ENABLE, 1);
    if (!result) g_dsp_scene = true;
    return result;
}
int afx_dsp_scene_program(const void *data, uint32_t bytes) {
    HOST_GUARD(-AFX_BUSY);
    const uint8_t *program = data;
    if (!program || bytes != AFX_DSP_PROGRAM_BYTES || g_dsp_scene) return -AFX_BAD_COMMAND;
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
    int result = scene_command(AFX_CMD_DSP_ENABLE, 1);
    if (result) return result;
    g_dsp_scene = true;
    g2_write_32(0xa0702000u, 0);
    g2_write_32(0xa0702004u, 0);
    /* Remove all memory/output writes before replacing their operands. */
    for (uint32_t i = 0; i < 128; ++i) g2_write_32(0xa0703408u + i * 16u, 2);
    thd_sleep(2);
    if (memory_format == 0)
        for (uint32_t i = 0; i < AFX_DSP_BYTES; i += 4)
            g2_write_32(g_spu_base + AFX_DSP_BASE + i, 0x60006000u);
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
int afx_dsp_scene_returns(bool enabled) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_dsp_scene) return -AFX_BUSY;
    g2_write_32(0xa0702000u, enabled ? g_dsp_return_left : 0);
    g2_write_32(0xa0702004u, enabled ? g_dsp_return_right : 0);
    return AFX_OK;
}
int afx_dsp_scene_disable(void) {
    HOST_GUARD(-AFX_BUSY);
    if (!g_dsp_scene) return -AFX_BUSY;
    int result = scene_command(AFX_CMD_DSP_DISABLE, 0);
    if (!result) {
        g_dsp_scene = false;
        g_dsp_return_left = g_dsp_return_right = 0;
    }
    return result;
}
