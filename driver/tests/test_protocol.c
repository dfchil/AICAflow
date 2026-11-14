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

/* An AFM of one setup (bank-relative address 0, 64 bytes) and the given
 * sections, order and loop index. Returns its size. */
typedef struct { const uint8_t *stream; uint32_t bytes, body; } test_section_t;
static uint32_t song_file(uint8_t *out, uint32_t capacity, const test_section_t *sections,
                          uint32_t section_count, const uint16_t *order, uint32_t order_count,
                          uint32_t loop, uint32_t channels, uint32_t voices, const uint8_t *lanes) {
    memset(out, 0, capacity);
    uint32_t image_at = 128, setups_end = 32 + AFX_SETUP_BYTES;
    uint32_t table = setups_end + (lanes ? (channels + 3u) & ~3u : 0);
    uint32_t order_at = table + section_count * AFX_SONG_SECTION_BYTES;
    uint32_t streams = (order_at + order_count * 2u + 3u) & ~3u, at = streams;
    uint8_t *image = out + image_at;
    for (uint32_t i = 0; i < section_count; ++i) {
        memcpy(image + at, sections[i].stream, sections[i].bytes);
        afx_write32(image + table + i * 12u, at);
        afx_write32(image + table + i * 12u + 4u, sections[i].bytes);
        afx_write32(image + table + i * 12u + 8u, sections[i].body);
        at += sections[i].bytes;
    }
    for (uint32_t i = 0; i < order_count; ++i) afx_write16(image + order_at + i * 2u, order[i]);
    if (lanes) memcpy(image + setups_end, lanes, channels);
    uint32_t image_size = at;
    assert(image_at + image_size <= capacity);
    afx_write32(image, section_count); afx_write32(image + 4, order_count);
    afx_write32(image + 8, loop); afx_write32(image + 12, table); afx_write32(image + 16, order_at);
    afx_write32(image + 20, streams); afx_write32(image + 24, at - streams);
    afx_write32(image + 28, channels);
    afx_write16(image + 32 + 2 * AFX_FIELD_ENV_DR, 31);
    afx_write32(out, AFX_SONG_MAGIC); afx_write32(out + 4, AFX_SONG_VERSION);
    afx_write32(out + 8, image_at + image_size); afx_write32(out + 12, AFX_FLAG_MUSIC | (lanes ? AFX_FLAG_LANES : 0));
    afx_write32(out + 16, image_at); afx_write32(out + 20, image_size);
    afx_write32(out + 24, 1); afx_write32(out + 28, 96);
    afx_write32(out + 36, 0x5096); afx_write32(out + 40, 1);
    afx_write32(out + 44, channels); afx_write32(out + 48, voices);
    afx_write32(out + 52, 1000); afx_write32(out + 56, 1);
    afx_write32(out + 64, afx_control_id(image, image_size));
    afx_write32(out + 96, 32); afx_write32(out + 100, 0); afx_write32(out + 104, 64);
    return image_at + image_size;
}
#define NOTE(ch) AFX_OP_NOTE_PL, ch, 0, 0, 0, 8, 0x24, 0x30
#define WAIT(t) AFX_OP_WAIT8, t

static void test_song(void) {
    static uint8_t file[1024];
    afx_song_header_t header;
    afx_validation_profile_t work;
    /* A: a note, its body ten ticks, then a tail of two that keys it off. */
    const uint8_t a[] = {NOTE(0), WAIT(10), AFX_OP_NEXT, AFX_JOIN_NEW, AFX_OP_KEYOFF, 0, WAIT(2), AFX_OP_END};
    const test_section_t one[] = {{a, sizeof(a), 10}};
    const uint16_t twice[] = {0, 0};
    /* A, A, looping to the first: two voices at each join, the tail's and the new play's. */
    uint32_t size = song_file(file, sizeof(file), one, 1, twice, 2, 0, 1, 2, NULL);
    assert(afx_song_validate_profile(file, size, &header, &work) == AFX_OK);
    assert(header.voices == 2 && header.channels == 1);
    uint32_t measured_channels = 0, measured_voices = 0;
    afx_write32(file + 48, 7); /* A wrong pool: measured, not compared. */
    assert(afx_song_measure(file, size, NULL, &measured_channels, &measured_voices) == AFX_OK);
    assert(measured_channels == 1 && measured_voices == 2);
    afx_write32(file + 48, 2);
    /* At a join the tail's KEYOFF and the new play's NOTE share a tick. */
    assert(work.peak_commands == 2 && work.peak_register_writes == 20);
    afx_write32(file + 48, 1);
    assert(afx_song_validate(file, size, NULL) == AFX_BAD_COMMAND); /* Not the pool it needs. */
    afx_write32(file + 48, 2);
    afx_write32(file + 44, 2);
    assert(afx_song_validate(file, size, NULL) == AFX_BAD_BOUNDS); /* Image header disagrees. */
    afx_write32(file + 128 + 28, 2);
    assert(afx_song_validate(file, size, NULL) == AFX_BAD_COMMAND); /* Channels not those used. */
    /* FREE gives the tail's voice back before the new play takes one. */
    const uint8_t freed[] = {NOTE(0), NOTE(1), WAIT(10), AFX_OP_NEXT, AFX_JOIN_NEW, AFX_OP_KEYOFF, 0,
                             AFX_OP_KEYOFF, 1, AFX_OP_FREE, 0, AFX_OP_FREE, 1, WAIT(2), AFX_OP_END};
    const test_section_t free_one[] = {{freed, sizeof(freed), 10}};
    size = song_file(file, sizeof(file), free_one, 1, twice, 2, 0, 2, 2, NULL);
    assert(afx_song_validate(file, size, NULL) == AFX_OK);
    const uint8_t kept[] = {NOTE(0), NOTE(1), WAIT(10), AFX_OP_NEXT, AFX_JOIN_NEW, AFX_OP_KEYOFF, 0,
                            AFX_OP_KEYOFF, 1, WAIT(2), AFX_OP_END};
    const test_section_t kept_one[] = {{kept, sizeof(kept), 10}};
    size = song_file(file, sizeof(file), kept_one, 1, twice, 2, 0, 2, 4, NULL);
    assert(afx_song_validate(file, size, NULL) == AFX_OK);
    /* Two executors: a tail must be over before the next join, not in its tick. */
    const uint8_t long_tail[] = {NOTE(0), WAIT(10), AFX_OP_NEXT, AFX_JOIN_NEW, WAIT(10), AFX_OP_END};
    const test_section_t long_one[] = {{long_tail, sizeof(long_tail), 10}};
    size = song_file(file, sizeof(file), long_one, 1, twice, 2, 0, 1, 2, NULL);
    assert(afx_song_validate(file, size, NULL) == AFX_BAD_COMMAND);
    const uint8_t short_tail[] = {NOTE(0), WAIT(10), AFX_OP_NEXT, AFX_JOIN_NEW, WAIT(9), AFX_OP_END};
    const test_section_t short_one[] = {{short_tail, sizeof(short_tail), 10}};
    size = song_file(file, sizeof(file), short_one, 1, twice, 2, 0, 1, 2, NULL);
    assert(afx_song_validate(file, size, NULL) == AFX_OK);
    /* A song that ends: the last NEXT starts nothing, and the tail plays out. */
    size = song_file(file, sizeof(file), long_one, 1, twice, 1, AFX_SONG_NO_LOOP, 1, 1, NULL);
    assert(afx_song_validate(file, size, NULL) == AFX_OK);
    /* Continue-only: intro, then a body that goes on with its voices. No pool. */
    const uint8_t intro[] = {NOTE(0), WAIT(4), AFX_OP_NEXT, AFX_JOIN_CONTINUE, AFX_OP_END};
    const uint8_t body[] = {NOTE(1), WAIT(6), AFX_OP_KEYOFF, 1, WAIT(2), AFX_OP_NEXT, AFX_JOIN_CONTINUE,
                            AFX_OP_END};
    const test_section_t loop_song[] = {{intro, sizeof(intro), 4}, {body, sizeof(body), 8}};
    const uint16_t intro_body[] = {0, 1};
    size = song_file(file, sizeof(file), loop_song, 2, intro_body, 2, 1, 2, 0, NULL);
    assert(afx_song_validate_profile(file, size, &header, &work) == AFX_OK);
    assert(!header.voices && work.peak_commands == 1);
    /* At the continue join the body's KEYOFF and its NOTE share a tick: no. The
     * body keys off at 6 and joins at 8; its NOTE then runs at 8 with nothing else. */
    /* With lanes. */
    const uint8_t lanes[] = {3, 1};
    size = song_file(file, sizeof(file), loop_song, 2, intro_body, 2, 1, 2, 0, lanes);
    assert(afx_song_validate(file, size, NULL) == AFX_OK);
    const uint8_t bad_lanes[] = {3, 64};
    size = song_file(file, sizeof(file), loop_song, 2, intro_body, 2, 1, 2, 0, bad_lanes);
    assert(afx_song_validate(file, size, NULL) == AFX_BAD_FORMAT);
    /* Refused streams. */
    const uint8_t late_next[] = {NOTE(0), WAIT(9), AFX_OP_NEXT, AFX_JOIN_NEW, AFX_OP_END};
    const uint8_t two_nexts[] = {WAIT(10), AFX_OP_NEXT, 0, AFX_OP_NEXT, 0, AFX_OP_END};
    const uint8_t tail_after_continue[] = {NOTE(0), WAIT(10), AFX_OP_NEXT, AFX_JOIN_CONTINUE,
                                           AFX_OP_KEYOFF, 0, AFX_OP_END};
    const uint8_t no_end[] = {NOTE(0), WAIT(10), AFX_OP_NEXT, AFX_JOIN_NEW, WAIT(1)};
    const uint8_t parked[] = {NOTE(0), WAIT(10), AFX_OP_NEXT, AFX_JOIN_NEW, AFX_OP_PARK};
    const uint8_t no_next[] = {NOTE(0), WAIT(10), AFX_OP_END};
    const uint8_t bad_join[] = {NOTE(0), WAIT(10), AFX_OP_NEXT, 2, AFX_OP_END};
    const uint8_t *const refused[] = {late_next, two_nexts, tail_after_continue, no_end, parked,
                                      no_next, bad_join};
    const uint32_t refused_bytes[] = {sizeof(late_next), sizeof(two_nexts), sizeof(tail_after_continue),
                                      sizeof(no_end), sizeof(parked), sizeof(no_next), sizeof(bad_join)};
    for (unsigned i = 0; i < sizeof(refused) / sizeof(*refused); ++i) {
        const test_section_t bad[] = {{refused[i], refused_bytes[i], 10}};
        size = song_file(file, sizeof(file), bad, 1, twice, 2, 0, 1, 1, NULL);
        assert(afx_song_validate(file, size, NULL) != AFX_OK);
    }
    /* Layout: an order entry past the sections, a loop index past the order. */
    const uint16_t past[] = {0, 1};
    size = song_file(file, sizeof(file), one, 1, past, 2, 0, 1, 2, NULL);
    assert(afx_song_validate_layout(file, size, NULL, NULL) == AFX_BAD_BOUNDS);
    size = song_file(file, sizeof(file), one, 1, twice, 2, 2, 1, 2, NULL);
    assert(afx_song_validate_layout(file, size, NULL, NULL) == AFX_BAD_BOUNDS);
    size = song_file(file, sizeof(file), one, 1, twice, 2, 0, 1, 2, NULL);
    afx_write32(file + 72, 1); /* A reserved word. */
    assert(afx_song_validate_layout(file, size, NULL, NULL) == AFX_BAD_FORMAT);
    /* A flow may not use NEXT or FREE, and LOOP (0x16) is gone. */
    uint8_t flow[160];
    candidate(flow);
    flow[140] = AFX_OP_NEXT; flow[141] = 0; flow[142] = AFX_OP_END; afx_write32(flow + 28, 11);
    assert(afx_file_validate(flow, sizeof(flow), NULL) == AFX_BAD_COMMAND);
    candidate(flow);
    flow[140] = 0x16; afx_write32(flow + 141, 0); afx_write32(flow + 28, 13);
    assert(afx_file_validate(flow, sizeof(flow), NULL) == AFX_BAD_COMMAND);
    candidate(flow); afx_write32(flow + 56, 1); /* The old loop_tick word. */
    assert(afx_file_validate(flow, sizeof(flow), NULL) == AFX_BAD_BOUNDS);
    candidate(flow); afx_write32(flow + 12, 32); /* The old LOOP flag. */
    assert(afx_file_validate(flow, sizeof(flow), NULL) == AFX_BAD_BOUNDS);
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
    event = (afx_event_t){.opcode = AFX_OP_NEXT, .join = AFX_JOIN_CONTINUE};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_OK && written == 2);
    assert(afx_decode_event(bytes, written, &decoded) == AFX_OK);
    assert(decoded.opcode == AFX_OP_NEXT && decoded.join == AFX_JOIN_CONTINUE && decoded.bytes == 2);
    assert(afx_decode_event(bytes, 1, &decoded) == AFX_BAD_BOUNDS);
    event.join = 2;
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_BAD_COMMAND);
    event = (afx_event_t){.opcode = AFX_OP_FREE, .channel = 63};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_OK && written == 2);
    assert(afx_decode_event(bytes, written, &decoded) == AFX_OK && decoded.channel == 63);
    bytes[1] = 64;
    assert(afx_decode_event(bytes, 2, &decoded) == AFX_BAD_COMMAND);
    bytes[0] = 0x16; /* LOOP is gone. */
    assert(afx_decode_event(bytes, 5, &decoded) == AFX_BAD_COMMAND);
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
    test_song();
    test_bytecode();
    test_control_id();
    puts("bank-bound protocol checks passed");
}
