/* The firmware's executor, driver/arm7/driver.c, compiled natively: the same
 * source the console's aicaflow.drv is built from, with AICA RAM and the
 * registers mapped onto the software AICA (see driver/arm7/aica_access.h).
 * crt0.s's work is done here: clearing the executor's state at boot, and the
 * timer FIQ. */
#include "firmware.h"

#include <string.h>

static aica_t *g_chip;
static uint32_t g_asset_base, g_private_end;

#define AICA_RAM(address) ((uintptr_t)(g_chip->ram + ((uint32_t)(address) & (AICA_RAM_BYTES - 1u))))
#define AICA_REG_WRITE(offset, value) aica_reg_write(g_chip, (offset), (value))
#define AICA_CHANNEL(name, physical) uint32_t name = (physical) * AFX_AICA_CHANNEL_REG_STRIDE
#define AICA_CHANNEL_SET(name, field, value) aica_reg_write(g_chip, (name) + (field) * 4u, (value))
#define AICA_ASSET_BASE g_asset_base
#define AICA_PRIVATE_END g_private_end
#define AICA_MAIN_LOOP() ((void)0)
#define arm_main afx_host_arm_main
#define arm_fiq_enable afx_host_arm_fiq_enable

#include "../../arm7/driver.c"

void afx_host_arm_fiq_enable(void) {}

static uint32_t read_word(const aica_t *chip, uint32_t address) {
    const uint8_t *p = chip->ram + address;
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void afx_host_firmware_boot(aica_t *chip) {
    g_chip = chip;
    /* What crt0.s clears and copies before arm_main. */
    memset((void *)contexts, 0, sizeof(contexts));
    memset(repeat_state, 0, sizeof(repeat_state));
    initialized_data_probe = 0x41584632;
    memset(voice_lane_gain, 0, sizeof(voice_lane_gain));
    memset(voice_lane_dsp_send, 0, sizeof(voice_lane_dsp_send));
    memset(voice_lane_id, 0, sizeof(voice_lane_id));
    memset(voice_lane_pan, 0, sizeof(voice_lane_pan));
    memset(voice_lane_pitch, 0, sizeof(voice_lane_pitch));
    memset(voice_flags, 0, sizeof(voice_flags));
    memset(voice_held, 0, sizeof(voice_held));
    memset(voice_control, 0, sizeof(voice_control));
    memset(voice_base_mix, 0, sizeof(voice_base_mix));
    memset(voice_base_pitch, 0, sizeof(voice_base_pitch));
    memset(voice_base_direct, 0, sizeof(voice_base_direct));
    memset(voice_base_dsp_send, 0, sizeof(voice_base_dsp_send));
    dsp_owner = dsp_delay_base = dsp_delay_bytes = 0;
    /* The layout the uploaded image's linker placed, from its info block. */
    g_asset_base = read_word(chip, AFX_FIRMWARE_INFO_OFFSET + offsetof(afx_firmware_info_t, asset_base));
    g_private_end = read_word(chip, AFX_FIRMWARE_INFO_OFFSET + offsetof(afx_firmware_info_t, private_end));
    afx_host_arm_main();
}

void afx_host_firmware_pass(aica_t *chip) {
    g_chip = chip;
    MAIN_LOOP_PASS();
}

void afx_host_firmware_timer(aica_t *chip) {
    g_chip = chip;
    ++*CLOCK;
    aica_reg_write(chip, 0x2890, AFX_TIMER_RELOAD);
    aica_reg_write(chip, 0x28a4, 0x40);
}
