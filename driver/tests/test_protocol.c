#include <aicaflow/firmware.h>
#include <aicaflow/codec.h>
/* The firmware reads a flow's bank table from AICA RAM; here from a buffer. */
static uint32_t bank_table[AFX_MAX_FLOW_BANKS];
#define AFX_BANK_TABLE_READ(context, slot) ((void)(context), bank_table[slot])
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

/* A flow with AFX_FLAG_BANKS and three bank slots whose one setup plays from
 * slot `slot`: relocation at 80, bank list at 92, image at 128 with the bank
 * table at image offset 48. */
static void banked(uint8_t file[192], uint32_t slot) {
    memset(file, 0, 192);
    afx_write32(file, AFX_FILE_MAGIC); afx_write32(file + 4, AFX_FILE_VERSION);
    afx_write32(file + 8, 192); afx_write32(file + 12, AFX_FLAG_BANKS);
    afx_write32(file + 16, 128); afx_write32(file + 20, 64);
    afx_write32(file + 24, AFX_SETUP_BYTES); afx_write32(file + 28, 9);
    afx_write32(file + 32, 1); afx_write32(file + 36, 1);
    afx_write32(file + 40, 0x12345678); afx_write32(file + 44, 9);
    afx_write32(file + 48, 80); afx_write32(file + 52, 1); afx_write32(file + 60, 92);
    afx_write32(file + 64, 1); afx_write32(file + 68, 1000); afx_write32(file + 72, 1);
    afx_write32(file + 80, 0); afx_write32(file + 84, (slot << AFX_BANK_SLOT_SHIFT) | 0x20);
    afx_write32(file + 88, 64);
    afx_write32(file + 92, 2);
    afx_write32(file + 96, 0xc0de0001); afx_write32(file + 100, 1);
    afx_write32(file + 104, 0xc0de0002); afx_write32(file + 108, 2);
    afx_write16(file + 128, (uint16_t)(slot << 5)); afx_write16(file + 130, 0x20);
    afx_write16(file + 128 + 6, 31);
    file[128 + 36] = AFX_OP_NOTE_PL;
    file[128 + 44] = AFX_OP_END;
}

static void test_banks(void) {
    uint8_t file[192];
    afx_file_header_t header;
    uint32_t low, high;
    for (uint32_t slot = 0; slot < 3; ++slot) {
        banked(file, slot);
        assert(afx_file_validate(file, sizeof(file), &header) == AFX_OK);
    }
    assert(header.banks_offset == 92 && afx_file_bank_count(file, &header) == 3);
    afx_file_bank_identity(file, &header, 0, &low, &high);
    assert(low == 0x12345678 && high == 9);
    afx_file_bank_identity(file, &header, 2, &low, &high);
    assert(low == 0xc0de0002 && high == 2);
    banked(file, 3); /* A slot past the list. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_RELOCATION);
    banked(file, 1); afx_write32(file + 12, 0); /* A list without the flag. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    banked(file, 1); afx_write32(file + 60, 0);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    for (uint32_t count = 0; count < 5; count += 4) {
        banked(file, 0); afx_write32(file + 92, count);
        assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    }
    banked(file, 0); afx_write32(file + 92, 3); /* A third entry, with no identity. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_SAMPLE);
    banked(file, 0); afx_write32(file + 60, 112); afx_write32(file + 112, 2); /* Into the image. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    banked(file, 0); afx_write32(file + 104, 0); afx_write32(file + 108, 0);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_SAMPLE);
    banked(file, 0); afx_write32(file + 28, 13); /* The table no longer ends the image. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_BOUNDS);
    banked(file, 0); file[128 + 48 + 5] = 1; /* The file's table is zero. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_FORMAT);
    /* Without the flag, the slot bits stay part of a slot 0 address. */
    uint8_t plain[160];
    candidate(plain);
    afx_write32(plain + 84, 1u << AFX_BANK_SLOT_SHIFT); afx_write16(plain + 96, 0x20);
    assert(afx_file_validate(plain, sizeof(plain), &header) == AFX_OK);
    assert(afx_file_bank_count(plain, &header) == 1);
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

/* A stream with a counted repeat: NOTE_PL, WAIT8 1, KEYOFF 0, REPEAT back to
 * target count more times, WAIT8 2, END. */
static void repeating(uint8_t file[160], uint32_t target, uint8_t count, uint32_t flags) {
    candidate(file);
    afx_write32(file + 12, flags);
    afx_write32(file + 28, 21);
    file[140] = AFX_OP_WAIT8; file[141] = 1;
    file[142] = AFX_OP_KEYOFF; file[143] = 0;
    file[144] = AFX_OP_REPEAT; afx_write32(file + 145, target); file[149] = count;
    file[150] = AFX_OP_WAIT8; file[151] = 2;
    file[152] = AFX_OP_END;
}

static void test_repeat(void) {
    uint8_t file[160];
    afx_file_header_t header;
    afx_validation_profile_t profile;
    uint64_t ticks;
    uint32_t num, den;
    repeating(file, 0, 2, AFX_FLAG_REPEAT);
    assert(afx_file_validate_profile(file, sizeof(file), &header, &profile) == AFX_OK);
    /* The jump joins the burst before it (KEYOFF) to the burst at its target (NOTE). */
    assert(profile.peak_commands == 2 && profile.peak_register_writes == 20);
    /* The span (one tick) plays three times: 1 + 2 more, then the WAIT 2. */
    assert(afx_flow_duration(file, sizeof(file), &ticks, &num, &den) == AFX_OK && ticks == 5);
    repeating(file, 8, 255, AFX_FLAG_REPEAT); /* From the WAIT: its span is WAIT 1, KEYOFF. */
    assert(afx_file_validate_profile(file, sizeof(file), &header, &profile) == AFX_OK);
    assert(profile.peak_commands == 1);
    assert(afx_flow_duration(file, sizeof(file), &ticks, &num, &den) == AFX_OK && ticks == 3 + 255);
    /* The flag and the instruction must agree. */
    repeating(file, 0, 2, 0);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    candidate(file); afx_write32(file + 12, AFX_FLAG_REPEAT);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    /* Into an instruction, at or past the REPEAT, or a span that never waits. */
    repeating(file, 3, 2, AFX_FLAG_REPEAT);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    repeating(file, 12, 2, AFX_FLAG_REPEAT);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    repeating(file, 18, 2, AFX_FLAG_REPEAT);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    repeating(file, 10, 2, AFX_FLAG_REPEAT);
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    repeating(file, 0, 0, AFX_FLAG_REPEAT); /* A count of zero is no REPEAT. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    /* Two REPEATs in turn: the second's span starts after the first. Their
     * spans may not overlap, as an instance has one counter. */
    repeating(file, 0, 1, AFX_FLAG_REPEAT);
    afx_write32(file + 28, 25);
    file[142] = AFX_OP_REPEAT; afx_write32(file + 143, 0); file[147] = 1;
    file[148] = AFX_OP_WAIT8; file[149] = 1;
    file[150] = AFX_OP_REPEAT; afx_write32(file + 151, 16); file[155] = 1;
    file[156] = AFX_OP_END;
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_OK);
    assert(afx_flow_duration(file, sizeof(file), &ticks, &num, &den) == AFX_OK && ticks == 4);
    afx_write32(file + 151, 8); /* Back into the first span. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
    /* A LOOP back into a span; its loop tick is counted along the stream. A
     * LOOP that meets the REPEAT before it waits would jump twice in a tick. */
    repeating(file, 0, 1, AFX_FLAG_REPEAT | AFX_FLAG_LOOP);
    afx_write32(file + 28, 25);
    file[152] = AFX_OP_LOOP; afx_write32(file + 153, 8); /* The WAIT 1, at tick 0. */
    assert(afx_file_validate_profile(file, sizeof(file), &header, &profile) == AFX_OK);
    afx_write32(file + 56, 1); afx_write32(file + 153, 10); /* The KEYOFF, then the REPEAT. */
    assert(afx_file_validate(file, sizeof(file), NULL) == AFX_BAD_COMMAND);
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
    event = (afx_event_t){.opcode = AFX_OP_REPEAT, .target = 0x12345, .count = 255};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_OK && written == 6);
    assert(afx_decode_event(bytes, written, &decoded) == AFX_OK);
    assert(decoded.opcode == AFX_OP_REPEAT && decoded.target == 0x12345 && decoded.count == 255 &&
           decoded.bytes == 6);
    assert(afx_decode_event(bytes, 5, &decoded) == AFX_BAD_BOUNDS);
    bytes[5] = 0;
    assert(afx_decode_event(bytes, 6, &decoded) == AFX_BAD_COMMAND);
    event.count = 0;
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_BAD_COMMAND);
    event.count = 256;
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_BAD_COMMAND);
    /* The compact PATCH forms: 2 bytes plus their values, decoded as PATCH with the form's mask. */
    static const struct { uint8_t opcode; uint32_t mask, bytes; } compact[] = {
        {AFX_OP_PATCH_LEVEL, AFX_PATCH_LEVEL_MASK, 4},
        {AFX_OP_PATCH_ENV, AFX_PATCH_ENV_MASK, 4},
        {AFX_OP_PATCH_ENV_LEVEL, AFX_PATCH_ENV_LEVEL_MASK, 6},
    };
    for (uint32_t i = 0; i < sizeof(compact) / sizeof(compact[0]); ++i) {
        assert(afx_compact_opcode(AFX_OP_PATCH, compact[i].mask) == compact[i].opcode);
        event = (afx_event_t){.opcode = compact[i].opcode, .channel = 7, .mask = compact[i].mask};
        assert(afx_encode_event(bytes, sizeof(bytes), &event, values, &written) == AFX_OK);
        assert(written == compact[i].bytes && bytes[0] == compact[i].opcode);
        assert(afx_decode_event(bytes, written, &decoded) == AFX_OK);
        assert(decoded.opcode == AFX_OP_PATCH && decoded.channel == 7 &&
               decoded.mask == compact[i].mask && decoded.bytes == written);
        assert(afx_read16(decoded.values) == values[0]);
        if (written == 6) assert(afx_read16(decoded.values + 2) == values[1]);
        assert(afx_decode_event(bytes, written - 1, &decoded) == AFX_BAD_BOUNDS);
        /* A compact opcode carries only its own mask. */
        event.mask = AFX_NOTE_PL_MASK;
        assert(afx_encode_event(bytes, sizeof(bytes), &event, values, &written) == AFX_BAD_COMMAND);
    }
    assert(afx_compact_opcode(AFX_OP_NOTE, AFX_NOTE_PL_MASK) == AFX_OP_NOTE_PL);
    assert(afx_compact_opcode(AFX_OP_NOTE, AFX_PATCH_ENV_MASK) == AFX_OP_NOTE);
    assert(afx_compact_opcode(AFX_OP_PATCH, AFX_NOTE_PL_MASK) == AFX_OP_PATCH);
    assert(afx_compact_opcode(AFX_OP_KEYOFF, 0) == AFX_OP_KEYOFF);
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

    /* Bank slots: bits 21-22 pick a bank table entry, only with AFX_FLAG_BANKS. */
    context = (afx_runtime_slot_t){.bank_base_units = 0x1000u >> 5, .bank_end = 0x2000};
    bank_table[1] = AFX_BANK_TABLE_ENTRY(0x40000, 0x100);
    bank_table[2] = AFX_BANK_TABLE_ENTRY(0x80000, 0x21); /* Rounds up to 0x40 bytes. */
    bank_table[3] = 0;
    control = 0x0020 | 0x0180; low = 0x10; /* Slot 1, offset 0x10, PCM format bits kept. */
    assert(resolve_sample_address(&context, &control, &low) == AFX_BAD_SAMPLE);
    context.flags = AFX_FLAG_BANKS;
    assert(resolve_sample_address(&context, &control, &low) == AFX_OK);
    assert(control == (0x0180 | 0x04) && low == 0x0010);
    control = 0x0040; low = 0x3f; /* Slot 2: the last byte of its rounded size. */
    assert(resolve_sample_address(&context, &control, &low) == AFX_OK);
    assert(control == 0x08 && low == 0x3f);
    control = 0x0040; low = 0x40;
    assert(resolve_sample_address(&context, &control, &low) == AFX_BAD_SAMPLE);
    control = 0x0060; low = 0; /* Slot 3 is empty. */
    assert(resolve_sample_address(&context, &control, &low) == AFX_BAD_SAMPLE);
    control = 0x0020; low = 0x100; /* Past slot 1's bank, inside slot 0's range. */
    assert(resolve_sample_address(&context, &control, &low) == AFX_BAD_SAMPLE);
    control = 0; low = 0x0fff; /* Slot 0 still comes from the context. */
    assert(resolve_sample_address(&context, &control, &low) == AFX_OK);
    assert(control == 0 && low == 0x1fff);
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
    test_banks();
    test_loop();
    test_repeat();
    test_bytecode();
    test_control_id();
    puts("bank-bound protocol checks passed");
}
