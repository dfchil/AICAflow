#include <aicaflow/firmware.h>
#include <aicaflow/codec.h>
#include "../arm7/sample_address.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void candidate(uint8_t file[160]) {
    memset(file, 0, 160);
    afx_write32(file, AFX_FILE_MAGIC); afx_write32(file + 4, AFX_FILE_VERSION);
    afx_write32(file + 8, 160); afx_write32(file + 16, 96); afx_write32(file + 20, 64);
    afx_write32(file + 24, AFX_SETUP_BYTES); afx_write32(file + 28, 9);
    afx_write32(file + 32, 1); afx_write32(file + 36, 1);
    afx_write32(file + 40, 0x12345678); afx_write32(file + 44, 9);
    afx_write32(file + 48, 80); afx_write32(file + 52, 1);
    afx_write32(file + 64, 1); afx_write32(file + 68, 1000); afx_write32(file + 72, 1);
    afx_write32(file + 80, 0); afx_write32(file + 84, 0); afx_write32(file + 88, 64);
    afx_write16(file + 96 + 6, 31);
    file[132] = AFX_OP_NOTE_PL;
    file[140] = AFX_OP_END;
}

static void test_bank_bound_container(void) {
    uint8_t file[160];
    afx_file_header_t header;
    uint64_t ticks;
    uint32_t num, den;
    candidate(file);
    assert(afx_file_validate(file, sizeof(file), &header) == AFX_OK);
    assert(header.abi == AFX_FILE_VERSION && header.control_id == 1 &&
           header.bank_id_low == 0x12345678u && header.bank_id_high == 9);
    assert(afx_flow_duration(file, sizeof(file), &ticks, &num, &den) == AFX_OK);
    assert(!ticks && num == 1000 && den == 1);
    afx_write32(file + 4, AFX_FILE_VERSION - 1u);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_FORMAT);
    candidate(file); afx_write32(file + 8, 159);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    candidate(file); afx_write32(file + 52, 0);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    candidate(file); afx_write32(file + 84, 1);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_RELOCATION);
    candidate(file); file[140] = AFX_OP_PARK;
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
}

/* A looping stream: NOTE_PL, WAIT8 1, then LOOP back to the NOTE_PL. */
static void looping(uint8_t file[160], uint32_t target, uint32_t loop_tick, uint32_t flags) {
    candidate(file);
    afx_write32(file + 12, flags);
    afx_write32(file + 28, 15);
    afx_write32(file + 56, loop_tick);
    file[140] = AFX_OP_WAIT8; file[141] = 1;
    file[142] = AFX_OP_LOOP; afx_write32(file + 143, target);
}

static void test_loop(void) {
    uint8_t file[160];
    afx_file_header_t header;
    afx_validation_profile_t profile;
    uint64_t ticks;
    uint32_t num, den;
    looping(file, 0, 0, AFX_FLAG_LOOP);
    assert(afx_file_validate_profile(file, sizeof(file), &header, &profile) == AFX_OK);
    assert((header.flags & AFX_FLAG_LOOP) && header.loop_tick == 0);
    /* The LOOP joins the burst before it (nothing) to the burst at its target (the NOTE). */
    assert(profile.peak_commands == 1 && profile.peak_register_writes == 19);
    assert(afx_flow_duration(file, sizeof(file), &ticks, &num, &den) == AFX_OK && ticks == 1);
    /* Looping to the WAIT itself: an instruction boundary one tick in. */
    looping(file, 8, 0, AFX_FLAG_LOOP);
    assert(afx_file_validate_profile(file, sizeof(file), &header, &profile) == AFX_OK);
    assert(profile.peak_commands == 1);
    /* Into the middle of an instruction, at the LOOP itself, or past it. */
    looping(file, 3, 0, AFX_FLAG_LOOP);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    looping(file, 10, 0, AFX_FLAG_LOOP);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    looping(file, 15, 0, AFX_FLAG_LOOP);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    /* The header's loop tick must be the target's authored tick. */
    looping(file, 0, 1, AFX_FLAG_LOOP);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    looping(file, 8, 1, AFX_FLAG_LOOP);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    /* The flag and the terminal instruction must agree; loop_tick needs the flag. */
    looping(file, 0, 0, 0);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    looping(file, 0, 0, AFX_FLAG_LOOP); file[142] = AFX_OP_END; afx_write32(file + 28, 11);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    candidate(file); afx_write32(file + 56, 1);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    /* A loop body that never waits would spin. */
    looping(file, 0, 0, AFX_FLAG_LOOP);
    file[140] = AFX_OP_LOOP; afx_write32(file + 141, 0); afx_write32(file + 28, 13);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    /* The old layout check reads the flag and tick without decoding the stream. */
    looping(file, 3, 7, AFX_FLAG_LOOP);
    assert(afx_file_validate_layout(file, sizeof(file), &header) == AFX_OK && header.loop_tick == 7);
}

static void test_bytecode(void) {
    uint8_t bytes[44];
    uint16_t values[] = {0x1234, 0x5678};
    uint32_t written;
    afx_event_t event = {.opcode = AFX_OP_NOTE_PL, .channel = 63, .setup = 1,
                         .mask = AFX_NOTE_PL_MASK};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, values, &written) == AFX_OK);
    assert(written == 8);
    afx_event_t decoded;
    assert(afx_decode_event(bytes, written, &decoded) == AFX_OK);
    assert(decoded.opcode == AFX_OP_NOTE && decoded.setup == 1 && decoded.channel == 63);
    for (uint32_t mask = 1; mask <= 3; ++mask) {
        event = (afx_event_t){.opcode = AFX_OP_PATCH, .mask = mask};
        assert(afx_encode_event(bytes, sizeof(bytes), &event, values, &written) == AFX_BAD_COMMAND);
        memset(bytes, 0, sizeof(bytes));
        bytes[0] = AFX_OP_PATCH;
        afx_write32(bytes + 2, mask);
        assert(afx_decode_event(bytes, sizeof(bytes), &decoded) == AFX_BAD_COMMAND);
        event.opcode = AFX_OP_NOTE;
        assert(afx_encode_event(bytes, sizeof(bytes), &event, values, &written) == AFX_OK);
    }
    event = (afx_event_t){.opcode = AFX_OP_PATCH, .mask = AFX_NOTE_PL_MASK};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, values, &written) == AFX_OK);
    assert(afx_decode_event(bytes, written, &decoded) == AFX_OK);
    event = (afx_event_t){.opcode = AFX_OP_WAIT32, .wait = 0};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_BAD_COMMAND);
    event = (afx_event_t){.opcode = AFX_OP_LOOP, .target = 0x12345};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_OK && written == 5);
    assert(afx_decode_event(bytes, written, &decoded) == AFX_OK);
    assert(decoded.opcode == AFX_OP_LOOP && decoded.target == 0x12345 && decoded.bytes == 5);
    assert(afx_decode_event(bytes, 4, &decoded) == AFX_BAD_BOUNDS);
}

static void test_control_id(void) {
    static const uint8_t image[] = {'a'};
    assert(afx_control_id(NULL, 1) == 0);
    assert(afx_control_id(image, sizeof(image)) == 0xe40c292cu);
}

static void test_sample_addresses(void) {
    assert(sizeof(afx_runtime_slot_t) == 64);
    afx_runtime_slot_t context = {.bank_base_units = 0xffe0u >> 5, .bank_end = 0x40000};
    const uint32_t offsets[] = {0, 1, 31, 32, 0xffff, 0x10000, 0x3001f};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        uint16_t control = 0xc380u | (offsets[i] >> 16), low = offsets[i];
        uint16_t original_control = control;
        assert(resolve_sample_address(&context, &control, &low) == AFX_OK);
        assert((((uint32_t)(control & 0x7fu) << 16) | low) == 0xffe0u + offsets[i]);
        assert((control & ~0x7fu) == (original_control & ~0x7fu));
    }
    uint16_t control = 3, low = 0x20; /* Exactly one byte beyond the bank. */
    assert(resolve_sample_address(&context, &control, &low) == AFX_BAD_SAMPLE);
    assert(control == 3 && low == 0x20);
    control = 0x7f; low = 0xffff;
    assert(resolve_sample_address(&context, &control, &low) == AFX_BAD_SAMPLE);
    control = 0xc400;
    assert(resolve_sample_address(&context, &control, &low) == AFX_OK);
    assert(control == 0xc400 && low == 0xffff); /* Noise is untouched. */
    context.bank_base_units = 0xffff; context.bank_end = AFX_AICA_RAM_SIZE;
    control = 0; low = 31;
    assert(resolve_sample_address(&context, &control, &low) == AFX_OK);
    assert(control == 0x1f && low == 0xffff);
    context.bank_base_units = 0; context.bank_end = 0;
    control = low = 0;
    assert(resolve_sample_address(&context, &control, &low) == AFX_BAD_SAMPLE);
}

/* The firmware image a host would upload must describe the layout these
 * headers were built with. */
static void test_firmware_image(const char *path) {
    static uint8_t image[AFX_AICA_RAM_SIZE];
    afx_firmware_info_t info;
    FILE *file = fopen(path, "rb");
    assert(file);
    size_t size = fread(image, 1, sizeof(image), file);
    assert(size && feof(file) && !fclose(file));
    assert(afx_firmware_validate(image, (uint32_t)size, &info) == AFX_OK);
    assert(info.abi == AFX_ABI_VERSION && info.layout_id == AFX_LAYOUT_ID);
    assert(info.load_bytes == size && info.asset_base >= size && !(info.asset_base & 31u));
    assert(info.asset_limit == AFX_ASSET_MAX && info.stack_base == AFX_STACK_BASE);
    assert(info.private_end <= info.stack_base);
}

int main(int argc, char **argv) {
    if (argc == 2) test_firmware_image(argv[1]);
    test_sample_addresses();
    test_bank_bound_container();
    test_loop();
    test_bytecode();
    test_control_id();
    puts("bank-bound protocol checks passed");
}
