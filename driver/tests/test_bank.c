#include <aicaflow/bank.h>
#include <aicaflow/codec.h>
#include <aicaflow/dsp.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplies the simulated SPU transport used by the real allocator and loader. */
#include "test_transport.c"
#include "../sh4/src/host_internal.h"

static void test_observation_cache(void) {
    uint32_t epoch = 0;
    afx_instance_status_t status;
    unsigned reads = observed_reads;
    assert(!read_observed(63, &epoch, &status));
    assert(observed_reads - reads == 1);
    observed(63, 64, AFX_RUNNING, 1, AFX_OK);
    reads = observed_reads;
    assert(read_observed(63, &epoch, &status) && status.state == AFX_RUNNING);
    assert(observed_reads - reads == 9);
    reads = observed_reads;
    assert(!read_observed(63, &epoch, &status));
    assert(observed_reads - reads == 1);
    uint8_t *record = ram + AFX_OBSERVED_ADDR + 63 * sizeof(afx_observed_t);
    afx_write32(record, 3); /* In-progress publication must not enter the cache. */
    assert(!read_observed(63, &epoch, &status) && epoch == 2);
    observed(63, 64, AFX_DONE, 2, AFX_OK);
    assert(read_observed(63, &epoch, &status) && status.state == AFX_DONE);
    afx_write32(record, UINT32_MAX - 1u);
    assert(read_observed(63, &epoch, &status));
    observed(63, 0, AFX_FREE, 3, AFX_OK); /* Epoch rollover to zero. */
    assert(read_observed(63, &epoch, &status) && epoch == 0);
}

static void test_bounded_bank_upload(void) {
    const uint32_t bytes = 2u * 65536u + 17u;
    uint8_t *input = malloc(bytes + 1u);
    assert(input);
    for (uint32_t i = 0; i < bytes; ++i) input[i + 1u] = (uint8_t)(i * 37u);
    afx_mem_stats_t before, after;
    assert(afx_mem_stats(&before) == AFX_OK);
    afx_asset_t asset = 0;
    unsigned transfers = dma_transfers;
    dma_max_size = 0;
    assert(afx_sample_bank_upload(input + 1, bytes, &asset) == AFX_OK);
    assert(dma_transfers - transfers == 3 && dma_max_size == 65536);
    uint32_t address = afx_asset_addr(asset);
    assert(!memcmp(ram + address, input + 1, bytes));
    for (uint32_t i = bytes; i < align_up(bytes, 32); ++i) assert(!ram[address + i]);
    assert(afx_asset_free(asset) == AFX_OK);
    /* Failure after one completed chunk releases the partial AICA allocation. */
    dma_fail_at = dma_transfers + 2;
    assert(afx_sample_bank_upload(input + 1, bytes, &asset) == -AFX_BUSY && !asset);
    assert(!dma_source && !g_upload_dma_asset);
    dma_fail_at = 0;
    assert(afx_mem_stats(&after) == AFX_OK);
    assert(after.free_bytes == before.free_bytes &&
           after.active_allocations == before.active_allocations);
    assert(afx_sample_bank_upload(input + 1, bytes, &asset) == AFX_OK);
    assert(afx_asset_free(asset) == AFX_OK);
    free(input);
}

static void test_direct_bank_upload(void) {
    const uint32_t bytes = 2u * 65536u + 17u;
    uint8_t *input = upload_image_alloc(bytes);
    assert(input);
    memset(input, 0xa5, align_up(bytes, 32)); /* Padding must not leak into AICA. */
    const uint32_t sizes[] = {17, 32, 65536, bytes};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        expected_dma_source = input;
        direct_dma_transfers = 0;
        afx_asset_t bank;
        assert(afx_sample_bank_upload(input, sizes[i], &bank) == AFX_OK);
        uint32_t address = afx_asset_addr(bank);
        assert(!memcmp(ram + address, input, sizes[i]));
        for (uint32_t j = sizes[i]; j < align_up(sizes[i], 32); ++j) assert(!ram[address + j]);
        assert(direct_dma_transfers == (sizes[i] < 32 ? 0u : sizes[i] > 65536 ? 2u : 1u));
        assert(afx_asset_free(bank) == AFX_OK);
    }
    dma_fail_at = dma_transfers + 2;
    afx_asset_t bank = 0;
    assert(afx_sample_bank_upload(input, bytes, &bank) == -AFX_BUSY && !bank);
    assert(!dma_source && !g_upload_dma_asset);
    dma_fail_at = 0;
    expected_dma_source = NULL;
    free(input);
}

static void test_queue_capacity(void) {
    assert(AFX_CMD_QUEUE_CAPACITY == 32 && sizeof(afx_cmd_queue_t) == 2112);
    uint8_t observed_before[AFX_MAX_FLOW_SLOTS * sizeof(afx_observed_t)];
    memcpy(observed_before, ram + AFX_OBSERVED_ADDR, sizeof(observed_before));
    assert(AFX_ASSET_MAX == 0x1fc800u + 1728u);
    const uint32_t starts[] = {0, UINT32_MAX - 15u};
    for (unsigned pass = 0; pass < 2; ++pass) {
        uint32_t start = starts[pass];
        afx_write32(ram + AFX_QUEUE_ADDR, start);
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), start);
        for (unsigned i = 0; i < 32; ++i)
            assert(enqueue(AFX_CMD_INSTANCE_GAIN, 1, i + 1, 0, NULL, 0) == AFX_OK);
        uint8_t before[sizeof(afx_cmd_queue_t)];
        memcpy(before, ram + AFX_QUEUE_ADDR, sizeof(before));
        assert(enqueue(AFX_CMD_STOP, 1, 99, 0, NULL, 0) == -AFX_IPC_FULL);
        assert(!memcmp(before, ram + AFX_QUEUE_ADDR, sizeof(before)));
        /* One executor batch frees eight entries, including across counter wrap. */
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), start + 8u);
        for (unsigned i = 32; i < 40; ++i)
            assert(enqueue(AFX_CMD_INSTANCE_GAIN, 1, i + 1, 0, NULL, 0) == AFX_OK);
        for (unsigned i = 8; i < 40; ++i) {
            afx_cmd_t command = queued(start + i);
            assert(command.sequence == i + 1 && command.opcode == AFX_CMD_INSTANCE_GAIN);
        }
        assert(enqueue(AFX_CMD_STOP, 1, 99, 0, NULL, 0) == -AFX_IPC_FULL);
        afx_write32(ram + AFX_QUEUE_ADDR, start + 41u);
        assert(enqueue(AFX_CMD_STOP, 1, 99, 0, NULL, 0) == -AFX_BAD_FIRMWARE);
    }
    afx_write32(ram + AFX_QUEUE_ADDR, 0);
    afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), 0);
    assert(!memcmp(observed_before, ram + AFX_OBSERVED_ADDR, sizeof(observed_before)));
}

static void test_channel_maps(afx_asset_t flow) {
    /* A full queue must not leak the channels/map reserved during activation. */
    uint32_t head = queue_head();
    for (unsigned i = 0; i < AFX_CMD_QUEUE_CAPACITY; ++i)
        assert(enqueue(AFX_CMD_INSTANCE_GAIN, 1, i + 1, 0, NULL, 0) == AFX_OK);
    uint64_t available = g_available_channels;
    uint64_t maps[AFX_CHANNEL_MAP_ARENAS];
    memcpy(maps, g_map_used, sizeof(maps));
    uint32_t references = g_assets[AFX_HANDLE_INDEX(flow)].references;
    afx_instance_t rejected = 0;
    assert(afx_instance_activate(flow, &rejected) == -AFX_IPC_FULL && rejected == 0);
    assert(g_available_channels == available && !memcmp(maps, g_map_used, sizeof(maps)));
    assert(g_assets[AFX_HANDLE_INDEX(flow)].references == references);
    afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), head + AFX_CMD_QUEUE_CAPACITY);
    assert(AFX_CHANNEL_MAP_ARENAS * AFX_CHANNEL_MAP_ARENA_SIZE == 320);
    afx_asset_slot_t *asset = &g_assets[AFX_HANDLE_INDEX(flow)];
    const uint8_t counts[] = {1, 3, 5, 7, 9, 17, 31, 33, 63, 64};
    for (unsigned test = 0; test < sizeof(counts); ++test) {
        uint32_t count = counts[test], reserved = (count + 3u) & ~3u;
        asset->required_channels = count;
        afx_instance_t first, second;
        assert(afx_instance_activate(flow, &first) == AFX_OK);
        afx_instance_slot_t *slot = &g_instances[AFX_HANDLE_INDEX(first)];
        uint32_t base = slot->map_addr;
        assert(!(base & 3u));
        for (uint32_t i = 0; i < reserved; ++i)
            assert(ram[base + i] == (i < count ? i : 0xff));
        uint8_t snapshot[320];
        if (count <= 32) {
            assert(afx_instance_activate(flow, &second) == AFX_OK);
            uint32_t next = g_instances[AFX_HANDLE_INDEX(second)].map_addr;
            assert(next == base + reserved);
            for (uint32_t i = 0; i < reserved; ++i)
                assert(ram[next + i] == (i < count ? count + i : 0xff));
        }
        memcpy(snapshot, ram + AFX_CHANNEL_MAP_ARENA_ADDR, sizeof(snapshot));
        memset(snapshot + (base - AFX_CHANNEL_MAP_ARENA_ADDR), 0xff, reserved);
        release_instance_preserving_generation(AFX_HANDLE_INDEX(first));
        assert(!memcmp(snapshot, ram + AFX_CHANNEL_MAP_ARENA_ADDR, sizeof(snapshot)));
        assert(afx_instance_activate(flow, &first) == AFX_OK);
        assert(g_instances[AFX_HANDLE_INDEX(first)].map_addr == base);
        release_instance_preserving_generation(AFX_HANDLE_INDEX(first));
        if (count <= 32) release_instance_preserving_generation(AFX_HANDLE_INDEX(second));
        for (unsigned i = 0; i < sizeof(snapshot); ++i)
            assert(ram[AFX_CHANNEL_MAP_ARENA_ADDR + i] == 0xff);
        assert(g_available_channels == UINT64_MAX);
        /* The simulated ARM consumes these commands between test cases. */
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    }
    asset->required_channels = 1;
    /* Isolate map capacity from the independent execution-budget limit. */
    uint32_t peak_commands = asset->peak_commands, peak_writes = asset->peak_register_writes;
    asset->peak_commands = asset->peak_register_writes = 0;
    afx_instance_t instances[64];
    for (unsigned i = 0; i < 64; ++i) {
        assert(afx_instance_activate(flow, &instances[i]) == AFX_OK);
        assert(ram[g_instances[AFX_HANDLE_INDEX(instances[i])].map_addr] == i);
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    }
    for (unsigned i = 0; i < 64; ++i)
        release_instance_preserving_generation(AFX_HANDLE_INDEX(instances[i]));
    for (unsigned i = 0; i < 320; ++i)
        assert(ram[AFX_CHANNEL_MAP_ARENA_ADDR + i] == 0xff);
    asset->peak_commands = peak_commands;
    asset->peak_register_writes = peak_writes;
}

static void firmware(uint8_t image[64]) {
    uint32_t manifest[] = {AFX_FIRMWARE_MAGIC, AFX_ABI_VERSION, AFX_LAYOUT_ID, 64, 64,
                           AFX_ASSET_MAX, AFX_PRIVATE_BASE + 64 * sizeof(afx_runtime_slot_t),
                           AFX_STACK_BASE};
    memset(image, 0, 64);
    for (unsigned i = 0; i < 8; ++i) afx_write32(image + 32 + 4 * i, manifest[i]);
}

static void bank_file(uint8_t data[96], uint32_t low, uint32_t high) {
    memset(data, 0, 96);
    afx_write32(data, AFX_BANK_MAGIC); afx_write32(data + 4, AFX_BANK_VERSION);
    afx_write32(data + 8, low); afx_write32(data + 12, high);
    afx_write32(data + 16, AFX_BANK_HEADER_BYTES); afx_write32(data + 20, 64);
    afx_write32(data + 24, 96);
}

static void flow_file(uint8_t data[160], uint32_t low, uint32_t high) {
    memset(data, 0, 160);
    afx_write32(data, AFX_FILE_MAGIC); afx_write32(data + 4, AFX_FILE_VERSION);
    afx_write32(data + 8, 160); afx_write32(data + 16, 96); afx_write32(data + 20, 64);
    afx_write32(data + 24, AFX_SETUP_BYTES); afx_write32(data + 28, 9);
    afx_write32(data + 32, 1); afx_write32(data + 36, 1);
    afx_write32(data + 40, low); afx_write32(data + 44, high);
    afx_write32(data + 48, 80); afx_write32(data + 52, 1);
    afx_write32(data + 64, 1); afx_write32(data + 68, 1000); afx_write32(data + 72, 1);
    afx_write32(data + 76, AFX_WORK_PROFILE(1, 19));
    afx_write32(data + 80, 0); afx_write32(data + 84, 0); afx_write32(data + 88, 64);
    afx_write16(data + 96 + 6, 31); /* A valid 32-frame PCM16 loop range. */
    data[96 + AFX_SETUP_BYTES] = AFX_OP_NOTE_PL;
    data[96 + AFX_SETUP_BYTES + 8] = AFX_OP_END;
}

static void test_relative_flow(const afx_bank_t *bank) {
    uint8_t source[161], expected[64], seek[120] = {0};
    uint8_t *file = source + 1; /* The caller's file need not be aligned. */
    flow_file(file, bank->id.low, bank->id.high);
    afx_write32(file + 28, 11);
    file[140] = AFX_OP_WAIT8; file[141] = 10; file[142] = AFX_OP_END;
    memcpy(expected, file + 96, sizeof(expected));
    afx_asset_t flow;
    assert(afx_bank_flow_upload(bank, file, 160, &flow) == AFX_OK);
    afx_asset_slot_t *asset = &g_assets[AFX_HANDLE_INDEX(flow)];
    assert(asset->bank == bank->asset);
    assert(!memcmp(ram + asset->addr, expected, sizeof(expected)));
    assert(!memcmp(file + 96, expected, sizeof(expected)));
    memset(source, 0xcc, sizeof(source)); /* No retained caller pointer either. */

    afx_write32(seek, AFX_SEEK_MAGIC); afx_write32(seek + 4, AFX_SEEK_VERSION);
    afx_write32(seek + 8, 1); afx_write32(seek + 12, bank->id.low);
    afx_write32(seek + 16, bank->id.high); afx_write32(seek + 20, 32);
    afx_write32(seek + 32, AFX_CHECKPOINT_MAGIC);
    afx_write32(seek + 36, AFX_CHECKPOINT_VERSION); afx_write32(seek + 40, 1);
    for (unsigned active = 0; active < 2; ++active) {
        uint32_t bytes = active ? 104 : 64;
        afx_write32(seek + 24, bytes - 32); afx_write32(seek + 28, bytes);
        afx_write32(seek + 52, active ? 46 : 36); /* After WAIT, or before NOTE. */
        afx_write32(seek + 56, active ? 10 : 0); afx_write32(seek + 60, active);
        memcpy(seek + 68, expected, AFX_SETUP_BYTES);
        assert(afx_flow_seek_index_load_memory(flow, seek, bytes) == AFX_OK);
        afx_instance_t instance;
        assert(afx_instance_activate(flow, &instance) == AFX_OK);
        afx_cmd_t command = queued(queue_head() - 1);
        afx_activation_t activation;
        memcpy(&activation, command.payload, sizeof(activation));
        assert(((uint32_t)activation.bank_base_units << 5) == afx_asset_addr(bank->asset));
        assert(activation.bank_bytes == bank->bytes);
        uint32_t index = AFX_HANDLE_INDEX(instance);
        observed(index, instance, AFX_PAUSED, command.sequence, AFX_OK);
        assert(afx_update() == AFX_OK);
        uint16_t patch_values[] = {0, 0};
        uint32_t head = queue_head();
        for (uint32_t mask = 1; mask <= 3; ++mask)
            assert(afx_instance_patch(instance, 0, mask, patch_values) == -AFX_BAD_COMMAND);
        assert(queue_head() == head);
        assert(afx_instance_patch(instance, 0, AFX_NOTE_PL_MASK, patch_values) == AFX_OK);
        unsigned reads_before = asset_reads;
        uint32_t selected = UINT32_MAX;
        assert(afx_instance_seek_checkpoint(instance, 5, &selected) == AFX_OK && selected == 0);
        assert(asset_reads == reads_before);
        command = queued(queue_head() - 1);
        assert(command.opcode == AFX_CMD_REBUILD);
        afx_rebuild_payload_t rebuild;
        memcpy(&rebuild, command.payload, sizeof(rebuild));
        assert(rebuild.state_count == active && rebuild.remaining_wait == (active ? 10 : 0));
        assert(rebuild.stream_position == (active ? 46 : 36));
        if (active) {
            assert(afx_read16(ram + rebuild.states_address + 4) == 0);
            assert(afx_read16(ram + rebuild.states_address + 6) == 0);
        }
        release_instance_preserving_generation(index);
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    }
    /* Select the preceding entry, including exact and between-entry targets. */
    afx_write32(seek + 24, 88); afx_write32(seek + 28, 120);
    afx_write32(seek + 40, 2);
    memset(seek + 48, 0, 72);
    afx_write32(seek + 52, 36);
    afx_write32(seek + 64, 7); afx_write32(seek + 68, 46);
    afx_write32(seek + 72, 3); afx_write32(seek + 76, 1);
    memcpy(seek + 84, expected, AFX_SETUP_BYTES);
    assert(afx_flow_seek_index_load_memory(flow, seek, sizeof(seek)) == AFX_OK);
    const uint32_t targets[] = {6, 7, 9};
    for (unsigned i = 0; i < 3; ++i) {
        afx_instance_t instance;
        assert(afx_instance_activate(flow, &instance) == AFX_OK);
        afx_cmd_t command = queued(queue_head() - 1);
        uint32_t index = AFX_HANDLE_INDEX(instance), selected = UINT32_MAX;
        observed(index, instance, AFX_PAUSED, command.sequence, AFX_OK);
        assert(afx_update() == AFX_OK);
        unsigned reads_before = asset_reads;
        assert(afx_instance_seek_checkpoint(instance, targets[i], &selected) == AFX_OK);
        assert(selected == (i ? 7u : 0u) && asset_reads == reads_before);
        release_instance_preserving_generation(index);
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    }
    assert(afx_asset_free(flow) == AFX_OK);
    flow_file(file, bank->id.low, bank->id.high);
    afx_write32(file + 28, 19);
    afx_write32(file + 76, AFX_WORK_PROFILE(2, 20)); /* Authored NOTE + one-word PATCH. */
    file[140] = AFX_OP_PATCH;
#if AFX_VALIDATE_ASSETS
    for (uint32_t mask = 1; mask <= 3; ++mask) {
        afx_write32(file + 142, mask);
        assert(afx_bank_flow_upload(bank, file, 160, &flow) == -AFX_BAD_COMMAND);
        assert(flow == AFX_ASSET_INVALID);
    }
#endif
    afx_write32(file + 142, 1u << AFX_FIELD_PITCH);
    afx_write16(file + 146, 1);
    file[148] = AFX_OP_WAIT8; file[149] = 10; file[150] = AFX_OP_END;
    assert(afx_bank_flow_upload(bank, file, 160, &flow) == AFX_OK);
    assert(g_assets[AFX_HANDLE_INDEX(flow)].peak_register_writes == 20);
    assert(afx_asset_free(flow) == AFX_OK);
}

static void test_dsp_allocations(void) {
    afx_dsp_program_t dsp;
    afx_mem_stats_t before, during, after;
    assert(afx_mem_stats(&before) == AFX_OK);
    assert(afx_dsp_program_delay(&dsp, 100, 8192, false) == AFX_OK);
    auto_dsp_ack = 1;
    for (unsigned rbl = 0; rbl < 4; ++rbl) {
        unsigned word_writes = asset_word_writes, block_writes = asset_block_writes;
        assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), rbl) == AFX_OK);
        assert(asset_word_writes == word_writes && asset_block_writes > block_writes);
        uint32_t bytes = AFX_DSP_MIN_BYTES << rbl;
        assert(g_dsp_ring && !(g_dsp_ring & (AFX_DSP_RING_ALIGN - 1)));
        assert(afx_mem_stats(&during) == AFX_OK);
        assert(during.asset_limit == before.asset_limit && during.used_bytes == before.used_bytes + bytes);
        afx_cmd_t command = queued(queue_head() - 1);
        assert(command.opcode == AFX_CMD_DSP_ENABLE && command.flags == (1u | AFX_DSP_FLAG_HOST_INIT));
        assert(afx_read32(command.payload) == g_dsp_ring && afx_read32(command.payload + 4) == bytes);
        for (uint32_t offset = 0; offset < bytes; offset += 4)
            assert(afx_read32(ram + g_dsp_ring + offset) == 0x60006000u);
        uint32_t word = 0;
        assert(afx_mem_free(g_dsp_ring) == -AFX_ASSET_REFERENCED);
        assert(afx_mem_upload(g_dsp_ring + 4, &word, sizeof(word)) == -AFX_ASSET_REFERENCED);
        /* Space after the ring remains usable, through the new ceiling. */
        uint32_t upper = g_dsp_ring + bytes;
        uint32_t asset = afx_mem_alloc(AFX_ASSET_MAX - upper, 32);
        assert(asset == upper);
        assert(afx_mem_upload(AFX_ASSET_MAX - 4, &word, sizeof(word)) == AFX_OK);
        assert(afx_mem_free(asset) == AFX_OK);
    }
    assert(afx_dsp_program_delay(&dsp, 100, 8192, true) == AFX_OK);
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == AFX_OK);
    for (uint32_t offset = 0; offset < AFX_DSP_MIN_BYTES; offset += 4)
        assert(afx_read32(ram + g_dsp_ring + offset) == 0);
    /* Legacy firmware still accepts the original flags and float block fill. */
    afx_write32(ram + AFX_STATUS_ADDR + offsetof(afx_status_t, capabilities),
                AFX_CAP_BOOTSTRAP | AFX_CAP_LIFECYCLE);
    assert(afx_dsp_program_delay(&dsp, 100, 8192, false) == AFX_OK);
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == AFX_OK);
    assert(queued(queue_head() - 1).flags == 1);
    assert(afx_read32(ram + g_dsp_ring) == 0x60006000u);
    afx_write32(ram + AFX_STATUS_ADDR + offsetof(afx_status_t, capabilities),
                AFX_CAP_BOOTSTRAP | AFX_CAP_LIFECYCLE | AFX_CAP_DSP_HOST_INIT);
    /* Replacing a ring with a memoryless scene returns the whole allocation. */
    assert(afx_dsp_program_gain(&dsp, 8192) == AFX_OK);
    assert(afx_dsp_scene_program(&dsp, sizeof(dsp)) == AFX_OK && !g_dsp_ring);
    assert(afx_dsp_scene_disable() == AFX_OK);
    assert(afx_mem_stats(&after) == AFX_OK && after.free_bytes == before.free_bytes);
    assert(afx_dsp_program_delay(&dsp, 100, 8192, false) == AFX_OK);
    /* No contiguous allocation available: do not queue an enable or leak. */
    uint32_t all = afx_mem_alloc(before.free_bytes, 32), head = queue_head();
    assert(all);
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == -AFX_NO_AICA_RAM);
    assert(queue_head() == head && !g_dsp_ring && !g_dsp_scene);
    assert(afx_mem_free(all) == AFX_OK);
    /* A full queue rejects before ownership reaches ARM7. */
    for (unsigned i = 0; i < AFX_CMD_QUEUE_CAPACITY; ++i)
        assert(enqueue(AFX_CMD_NOP, 0, 0, 0, NULL, 0) == AFX_OK);
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == -AFX_IPC_FULL);
    assert(!g_dsp_ring && !g_dsp_scene);
    afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    /* An explicit firmware rejection returns the allocation. */
    dsp_reply_result = AFX_BAD_COMMAND;
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == -AFX_BAD_COMMAND);
    assert(!g_dsp_ring && !g_dsp_scene);
    dsp_reply_result = AFX_OK;
    /* Timeout is not proof of rejection: retain until a later acknowledged stop. */
    auto_dsp_ack = 0;
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == -AFX_TIMEOUT);
    uint32_t pinned = g_dsp_ring;
    assert(pinned && g_dsp_scene && afx_mem_free(pinned) == -AFX_ASSET_REFERENCED);
    assert(afx_dsp_scene_disable() == -AFX_TIMEOUT && g_dsp_ring == pinned);
    auto_dsp_ack = 1;
    assert(afx_dsp_scene_disable() == AFX_OK && !g_dsp_ring && !g_dsp_scene);
    auto_dsp_ack = 0;
    assert(afx_mem_stats(&after) == AFX_OK && after.free_bytes == before.free_bytes);
}

static uint8_t *read_fixture(const char *path, uint32_t *bytes) {
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size > 0 && size <= AFX_ASSET_MAX && !fseek(file, 0, SEEK_SET));
    uint8_t *data = malloc((size_t)size);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    assert(!fclose(file));
    *bytes = (uint32_t)size;
    return data;
}

int main(int argc, char **argv) {
    assert(argc == 1 || argc == 4);
    uint8_t fw[64], bank_data[96], flow_data[160], seek_data[48];
    afx_bank_t bank = {0};
    afx_asset_t flow;
    afx_write32(dsp_registers + 0x3408, 0x6000); /* DSP left running by a previous driver. */
    firmware(fw); assert(afx_init(fw, sizeof(fw)) == AFX_OK);
    test_observation_cache();
    test_bounded_bank_upload();
    test_direct_bank_upload();
    test_queue_capacity();
    afx_dsp_program_t dsp;
    afx_mem_stats_t memory;
    assert(afx_mem_stats(&memory) == AFX_OK && memory.asset_limit == AFX_ASSET_MAX);
    assert(afx_dsp_program_gain(&dsp, 8192) == AFX_OK);
    sleep_observe_reference = AFX_DSP_SCENE_REFERENCE;
    assert(afx_dsp_scene_program(&dsp, sizeof(dsp)) == AFX_OK);
    assert(afx_mem_stats(&memory) == AFX_OK && memory.asset_limit == AFX_ASSET_MAX);
    sleep_observe_reference = AFX_DSP_SCENE_REFERENCE;
    assert(afx_dsp_scene_disable() == AFX_OK);
    assert(afx_dsp_program_delay(&dsp, 7938, 8192, false) == AFX_OK);
    sleep_observe_reference = AFX_DSP_SCENE_REFERENCE;
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == AFX_OK);
    assert(afx_mem_stats(&memory) == AFX_OK && memory.asset_limit == AFX_ASSET_MAX);
    assert(memory.used_bytes == AFX_DSP_MIN_BYTES && g_dsp_ring && !(g_dsp_ring & 2047));
    assert(afx_dsp_program_delay(&dsp, 9000, 8192, false) == AFX_OK);
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == -AFX_BAD_BOUNDS);
    sleep_observe_reference = AFX_DSP_SCENE_REFERENCE;
    assert(afx_dsp_scene_disable() == AFX_OK);
    assert(afx_mem_stats(&memory) == AFX_OK && memory.asset_limit == AFX_ASSET_MAX);
    bank_file(bank_data, 0x12345678u, 0x9abcdef0u);
    test_dsp_allocations();
    flow_file(flow_data, 0x12345678u, 0x9abcdef0u);
    assert(afx_file_validate(flow_data, sizeof(flow_data), NULL) == AFX_OK);
    afx_validation_profile_t profile;
    assert(afx_file_validate_profile(flow_data, sizeof(flow_data), NULL, &profile) == AFX_OK);
    assert(profile.peak_commands == 1 && profile.peak_register_writes == 19);
    afx_write32(flow_data + 4, AFX_FILE_VERSION - 1u);
    assert(afx_file_validate(flow_data, sizeof(flow_data), NULL) == AFX_BAD_FORMAT);
    afx_write32(flow_data + 4, AFX_FILE_VERSION);
    assert(afx_bank_load_memory(&bank, bank_data, sizeof(bank_data)) == AFX_OK);
    /* Trusted builds require an authored profile, but do not recount events. */
    afx_write32(flow_data + 76, 0);
#if AFX_VALIDATE_ASSETS
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == AFX_OK);
    assert(afx_asset_free(flow) == AFX_OK);
#else
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == -AFX_BAD_FORMAT);
#endif
    afx_write32(flow_data + 76, AFX_WORK_PROFILE(1, 1));
#if AFX_VALIDATE_ASSETS
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == -AFX_BAD_FORMAT);
#else
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == AFX_OK);
    assert(g_assets[AFX_HANDLE_INDEX(flow)].peak_register_writes == 1);
    assert(afx_asset_free(flow) == AFX_OK);
#endif
    afx_write32(flow_data + 76, AFX_WORK_PROFILE(1, 19));
    flow_data[96 + AFX_SETUP_BYTES] = 0xff;
#if AFX_VALIDATE_ASSETS
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == -AFX_BAD_COMMAND);
#else
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == AFX_OK);
    assert(afx_asset_free(flow) == AFX_OK);
#endif
    flow_data[96 + AFX_SETUP_BYTES] = AFX_OP_NOTE_PL;
    afx_write32(flow_data + 76, AFX_WORK_PROFILE(AFX_EXECUTION_BUDGET_COMMANDS + 1, 19));
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) != AFX_OK);
    afx_write32(flow_data + 76, AFX_WORK_PROFILE(1, 19));
    afx_write32(flow_data + 20, UINT32_MAX);
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == -AFX_BAD_BOUNDS);
    afx_write32(flow_data + 20, 64);
    test_relative_flow(&bank);
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &flow) == AFX_OK);
    afx_asset_t second;
    uint32_t bank_index = AFX_HANDLE_INDEX(bank.asset);
    assert(afx_bank_flow_upload(&bank, flow_data, sizeof(flow_data), &second) == AFX_OK);
    assert(g_assets[bank_index].references == 2);
    assert(g_assets[AFX_HANDLE_INDEX(second)].bank == bank.asset);
    assert(afx_bank_release(&bank) == -AFX_ASSET_REFERENCED);
    assert(afx_asset_free(second) == AFX_OK && g_assets[bank_index].references == 1);
    /* A generic asset reusing the slot must not release the old flow's bank. */
    afx_asset_t raw = afx_asset_upload(flow_data, sizeof(flow_data), AFX_UPLOAD_ALIGN);
    assert(raw && AFX_HANDLE_INDEX(raw) == AFX_HANDLE_INDEX(second));
    assert(!g_assets[AFX_HANDLE_INDEX(raw)].bank && !g_assets[AFX_HANDLE_INDEX(raw)].flow);
    assert(afx_asset_free(raw) == AFX_OK && g_assets[bank_index].references == 1);
    test_channel_maps(flow);
    memset(seek_data, 0, sizeof(seek_data));
    afx_write32(seek_data, AFX_SEEK_MAGIC); afx_write32(seek_data + 4, AFX_SEEK_VERSION);
    afx_write32(seek_data + 8, 1); afx_write32(seek_data + 12, 0x12345678u);
    afx_write32(seek_data + 16, 0x9abcdef0u); afx_write32(seek_data + 20, AFX_SEEK_HEADER_BYTES);
    afx_write32(seek_data + 24, 16); afx_write32(seek_data + 28, sizeof(seek_data));
    afx_write32(seek_data + 32, AFX_CHECKPOINT_MAGIC);
    afx_write32(seek_data + 36, AFX_CHECKPOINT_VERSION);
    assert(afx_flow_seek_index_load_memory(flow, seek_data, sizeof(seek_data)) == AFX_OK);
    afx_write32(seek_data + 8, 2);
    assert(afx_flow_seek_index_load_memory(flow, seek_data, sizeof(seek_data)) == -AFX_BAD_SAMPLE);
    assert(afx_bank_release(&bank) == -AFX_ASSET_REFERENCED);
    assert(afx_asset_free(flow) == AFX_OK);
    assert(afx_bank_release(&bank) == AFX_OK);
    if (argc == 4) {
        uint32_t bank_bytes, flow_bytes, seek_bytes;
        uint8_t *b = read_fixture(argv[1], &bank_bytes);
        uint8_t *f = read_fixture(argv[2], &flow_bytes);
        uint8_t *s = read_fixture(argv[3], &seek_bytes);
        assert(afx_bank_load_memory(&bank, b, bank_bytes) == AFX_OK);
        f[40] ^= 1; /* Reject a valid control stream bound to a different bank. */
        assert(afx_bank_flow_upload(&bank, f, flow_bytes, &flow) != AFX_OK);
        f[40] ^= 1;
        assert(afx_bank_flow_upload(&bank, f, flow_bytes, &flow) == AFX_OK);
        s[8] ^= 1; /* A stale checkpoint must not attach to a new control image. */
        assert(afx_flow_seek_index_load_memory(flow, s, seek_bytes) != AFX_OK);
        s[8] ^= 1;
        assert(afx_flow_seek_index_load_memory(flow, s, seek_bytes) == AFX_OK);
        assert(afx_asset_free(flow) == AFX_OK);
        assert(afx_bank_release(&bank) == AFX_OK);
        free(b); free(f); free(s);
    }
    afx_shutdown();
    return 0;
}
