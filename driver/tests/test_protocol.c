#include <aicaflow/firmware.h>
#include <aicaflow/codec.h>

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
    afx_write32(file + 4, AFX_ABI_VERSION);
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
    event = (afx_event_t){.opcode = AFX_OP_WAIT32, .wait = 0};
    assert(afx_encode_event(bytes, sizeof(bytes), &event, NULL, &written) == AFX_BAD_COMMAND);
}

static void test_control_id(void) {
    static const uint8_t image[] = {'a'};
    assert(afx_control_id(NULL, 1) == 0);
    assert(afx_control_id(image, sizeof(image)) == 0xe40c292cu);
}

int main(void) {
    test_bank_bound_container();
    test_bytecode();
    test_control_id();
    puts("bank-bound protocol checks passed");
}
