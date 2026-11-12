#include <aicaflow/bank.h>
#include <aicaflow/scene.h>
#include <aicaflow/codec.h>
#include <aicaflow/dsp.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Status arrives by interrupt and AICAflow's event thread takes it: the
 * program makes no call. The thread reads only the slots the event ring names,
 * recycles an instance that ended and reports it, and loses no news published
 * while it works. */
static afx_instance_t ev_instance, ev_other;
static unsigned ended_calls;
static afx_instance_t ended_instance;
static afx_instance_status_t ended_status;
static void on_end(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    assert(user == &ended_calls);
    /* Unlocked: the program may call in, and the handle is already stale. */
    afx_instance_status_t now;
    assert(afx_instance_status(instance, &now) == -AFX_STALE_GENERATION);
    ++ended_calls;
    ended_instance = instance;
    ended_status = *status;
}
static unsigned loop_calls;
static afx_instance_status_t loop_status;
static void on_loop(afx_instance_t instance, const afx_instance_status_t *status, void *user) {
    assert(user == &loop_calls && instance == ev_instance);
    ++loop_calls;
    loop_status = *status;
}
static void publish_done(void) { observed(5, ev_instance, AFX_DONE, 3, AFX_OK); }
static void publish_other(void) { observed(6, ev_other, AFX_PAUSED, 2, AFX_OK); }
static void publish_running_and_dispatch(void) {
    observed(5, ev_instance, AFX_RUNNING, 4, AFX_OK);
    irq_other_asic_event(); /* Another ASIC interrupt while the thread reads. */
}
static void live_slot(uint32_t index, afx_instance_t *out) {
    afx_instance_slot_t *slot = &g_instances[index];
    memset(slot, 0, sizeof(*slot));
    slot->live = true;
    slot->generation = 1;
    *out = AFX_MAKE_HANDLE(index, 1);
}
static void drain_queue(void) {
    afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
}
static void test_events(void) {
    afx_instance_status_t status;
    irq_run_worker(); /* The boot interrupt: nothing to read. */
    afx_set_end_callback(on_end, &ended_calls);
    live_slot(5, &ev_instance);
    live_slot(6, &ev_other);
    unsigned reads = observed_reads, runs = worker_runs;
    irq_run_worker();
    assert(observed_reads == reads && worker_runs == runs); /* No news, no work. */
    /* One record read for one event: epoch, seven fields, epoch. */
    observed(5, ev_instance, AFX_RUNNING, 1, AFX_OK);
    irq_run_worker();
    assert(observed_reads - reads == 9);
    assert(afx_instance_status(ev_instance, &status) == AFX_OK && status.state == AFX_RUNNING);
    /* A position-only republish is not news; refresh reads it on request. */
    reads = observed_reads;
    observed_position(5, 0x40);
    irq_run_worker();
    assert(observed_reads == reads && worker_runs == runs + 1);
    assert(afx_instance_status(ev_instance, &status) == AFX_OK && status.position == 0);
    assert(afx_instance_refresh(ev_instance) == AFX_OK);
    assert(afx_instance_status(ev_instance, &status) == AFX_OK && status.position == 0x40);
    /* A LOOP: the state word carries the loop count, which is news, and the
     * loop callback reports it with the record's position. */
    afx_set_loop_callback(on_loop, &loop_calls);
    observed(5, ev_instance, AFX_RUNNING | (1u << AFX_STATE_LOOPS_SHIFT), 1, AFX_OK);
    irq_run_worker();
    assert(loop_calls == 1 && loop_status.loops == 1 && loop_status.state == AFX_RUNNING);
    assert(afx_instance_status(ev_instance, &status) == AFX_OK && status.state == AFX_RUNNING &&
           status.loops == 1);
    /* Other news with the same loop count is not a loop. */
    observed(5, ev_instance, AFX_RUNNING | (1u << AFX_STATE_LOOPS_SHIFT), 2, AFX_OK);
    irq_run_worker();
    assert(loop_calls == 1);
    assert(afx_instance_status(ev_instance, &status) == AFX_OK && status.sequence == 2);
    /* Published right after the clear: taken in the same pass. */
    observed(5, ev_instance, AFX_RUNNING | (2u << AFX_STATE_LOOPS_SHIFT), 2, AFX_OK);
    irq_hook_on_clear = publish_other;
    irq_run_worker();
    assert(loop_calls == 2 && loop_status.loops == 2);
    assert(afx_instance_status(ev_other, &status) == AFX_OK && status.state == AFX_PAUSED);
    /* Published before the unmask: the interrupt arrives on unmask. */
    irq_hook_on_unmask = publish_done;
    observed(6, ev_other, AFX_RUNNING, 3, AFX_OK);
    irq_run_worker();
    /* It ended: the thread asked the firmware to recycle it. */
    afx_cmd_t command = queued(queue_head() - 1);
    assert(command.opcode == AFX_CMD_RECYCLE && command.reference == ev_instance);
    assert(g_instances[5].recycling && !ended_calls);
    drain_queue();
    /* KOS calls the masking part again for other ASIC interrupts meanwhile. */
    observed(5, 0, AFX_FREE, command.sequence, AFX_OK);
    irq_other_asic_event();
    irq_other_asic_event();
    irq_run_worker();
    assert(ended_calls == 1 && ended_instance == ev_instance);
    assert(ended_status.state == AFX_DONE && ended_status.result == AFX_OK);
    assert(ended_status.loops == 2); /* A DONE record carries no count; the last one stays. */
    assert(afx_instance_status(ev_instance, &status) == -AFX_STALE_GENERATION && !g_instances[5].live);
    /* While the thread reads, after the clear, another ASIC interrupt. */
    live_slot(5, &ev_instance);
    irq_hook_on_clear = publish_running_and_dispatch;
    observed(6, ev_other, AFX_RUNNING, 4, AFX_OK);
    irq_run_worker();
    assert(afx_instance_status(ev_instance, &status) == AFX_OK && status.sequence == 4);
    /* More news than the ring holds: every live record is read again. */
    for (uint32_t i = 0; i < AFX_EVENT_CAPACITY + 2u; ++i)
        observed(i & 1u ? 5 : 6, i & 1u ? ev_instance : ev_other, AFX_RUNNING, 10 + i, AFX_OK);
    assert(afx_read32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, event_overflows)) == 2);
    irq_run_worker();
    assert(afx_instance_status(ev_instance, &status) == AFX_OK && status.sequence == 10 + AFX_EVENT_CAPACITY + 1u);
    assert(afx_instance_status(ev_other, &status) == AFX_OK && status.sequence == 10 + AFX_EVENT_CAPACITY);
    /* A refused recycle is not asked again. */
    observed(6, ev_other, AFX_DONE, 40, AFX_OK);
    irq_run_worker();
    command = queued(queue_head() - 1);
    assert(command.opcode == AFX_CMD_RECYCLE && command.reference == ev_other);
    drain_queue();
    uint32_t head = queue_head();
    observed(6, ev_other, AFX_ERROR, command.sequence, AFX_BUSY);
    irq_run_worker();
    assert(queue_head() == head && g_instances[6].recycle_refused && !g_instances[6].pending);
    afx_set_end_callback(NULL, NULL);
    memset(&g_instances[5], 0, sizeof(g_instances[5]));
    memset(&g_instances[6], 0, sizeof(g_instances[6]));
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
    /* A stream chunk that is not a multiple of 32 bytes would leave the next
     * one unaligned, so only the bank's last chunk may be. */
    static _Alignas(32) uint8_t chunk[96];
    bool complete = false;
    assert(afx_sample_bank_stream_begin(81, &asset) == AFX_OK);
    assert(afx_sample_bank_stream_dma_begin(asset, 0, chunk, 40) == -AFX_BAD_BOUNDS);
    assert(afx_sample_bank_stream_dma_begin(asset, 0, chunk, 64) == AFX_OK);
    assert(afx_sample_bank_stream_dma_poll(asset, &complete) == AFX_OK && !complete);
    thd_pass(); /* the simulated DMA finishes */
    assert(afx_sample_bank_stream_dma_poll(asset, &complete) == AFX_OK && complete);
    assert(afx_sample_bank_stream_dma_begin(asset, 64, chunk, 17) == AFX_OK);
    thd_pass();
    assert(afx_sample_bank_stream_dma_poll(asset, &complete) == AFX_OK && complete);
    assert(afx_sample_bank_stream_finish(asset) == AFX_OK);
    assert(afx_asset_free(asset) == AFX_OK);
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
        g_queue_head = start;
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
        /* The SH-4 owns head; a tail beyond it can only be firmware corruption. */
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), start + 41u);
        assert(enqueue(AFX_CMD_STOP, 1, 99, 0, NULL, 0) == -AFX_BAD_FIRMWARE);
    }
    afx_write32(ram + AFX_QUEUE_ADDR, 0);
    g_queue_head = 0;
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
        /* Release frees the reservation without a G2 write to the old map. */
        release_instance_preserving_generation(AFX_HANDLE_INDEX(first));
        assert(!memcmp(snapshot, ram + AFX_CHANNEL_MAP_ARENA_ADDR, sizeof(snapshot)));
        assert(afx_instance_activate(flow, &first) == AFX_OK);
        assert(g_instances[AFX_HANDLE_INDEX(first)].map_addr == base);
        release_instance_preserving_generation(AFX_HANDLE_INDEX(first));
        if (count <= 32) release_instance_preserving_generation(AFX_HANDLE_INDEX(second));
        for (unsigned arena = 0; arena < AFX_CHANNEL_MAP_ARENAS; ++arena) assert(!g_map_used[arena]);
        assert(g_available_channels == UINT64_MAX);
        /* The simulated ARM consumes these commands between test cases. */
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    }
    asset->required_channels = 1;
    afx_instance_t instances[64];
    for (unsigned i = 0; i < 64; ++i) {
        assert(afx_instance_activate(flow, &instances[i]) == AFX_OK);
        assert(ram[g_instances[AFX_HANDLE_INDEX(instances[i])].map_addr] == i);
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    }
    for (unsigned i = 0; i < 64; ++i)
        release_instance_preserving_generation(AFX_HANDLE_INDEX(instances[i]));
    for (unsigned arena = 0; arena < AFX_CHANNEL_MAP_ARENAS; ++arena) assert(!g_map_used[arena]);
}

static void test_coinciding_work(afx_asset_t flow) {
    /* The runtime refuses no work: authoring limits one flow's burst, and the
     * executor reports lateness when bursts from several instances coincide. */
    afx_asset_slot_t *asset = &g_assets[AFX_HANDLE_INDEX(flow)];
    uint32_t peak_writes = asset->peak_register_writes;
    asset->peak_register_writes = AFX_EXECUTION_BUDGET_WRITES;
    afx_instance_t first, second;
    assert(afx_instance_activate(flow, &first) == AFX_OK);
    assert(afx_instance_activate(flow, &second) == AFX_OK);
    release_instance_preserving_generation(AFX_HANDLE_INDEX(first));
    release_instance_preserving_generation(AFX_HANDLE_INDEX(second));
    asset->peak_register_writes = peak_writes;
    assert(g_available_channels == UINT64_MAX);
    afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
}

static void recycle(uint32_t index) {
    afx_cmd_t command = queued(queue_head() - 1);
    assert(command.opcode == AFX_CMD_RECYCLE);
    drain_queue();
    observed(index, 0, AFX_FREE, command.sequence, AFX_OK);
    irq_run_worker();
    assert(!g_instances[index].live);
}
static void maps_empty(void) {
    for (unsigned arena = 0; arena < AFX_CHANNEL_MAP_ARENAS; ++arena) assert(!g_map_used[arena]);
}

static void test_stop_behind_activate(afx_asset_t flow) {
    /* STOP waits for no acknowledgement: it is queued behind the ACTIVATE,
     * and asking again queues nothing. It gives the channel back at once,
     * while the map stays reserved for the ARM7 until RECYCLE. */
    afx_instance_t instance;
    assert(afx_instance_activate(flow, &instance) == AFX_OK);
    uint32_t index = AFX_HANDLE_INDEX(instance);
    uint32_t activation = g_instances[index].status.sequence, head = queue_head();
    uint64_t maps[AFX_CHANNEL_MAP_ARENAS];
    memcpy(maps, g_map_used, sizeof(maps));
    assert(g_available_channels != UINT64_MAX);
    assert(afx_instance_stop(instance) == AFX_OK && queue_head() == head + 1u);
    afx_cmd_t stop = queued(head);
    assert(stop.opcode == AFX_CMD_STOP && stop.reference == instance && stop.sequence > activation);
    assert(g_available_channels == UINT64_MAX && !memcmp(maps, g_map_used, sizeof(maps)));
    assert(afx_instance_stop(instance) == AFX_OK && queue_head() == head + 1u);
    assert(afx_instance_pause(instance) == -AFX_BUSY && queue_head() == head + 1u);
    /* The ACTIVATE's report is older than the STOP: the STOP's is awaited. */
    observed(index, instance, AFX_RUNNING, activation, AFX_OK);
    irq_run_worker();
    assert(g_instances[index].pending && g_instances[index].status.state == AFX_FREE);
    drain_queue();
    observed(index, instance, AFX_DONE, stop.sequence, AFX_OK);
    irq_run_worker();
    assert(g_instances[index].recycling);
    head = queue_head();
    assert(afx_instance_stop(instance) == AFX_OK && queue_head() == head); /* It has ended. */
    recycle(index);
    assert(afx_instance_stop(instance) == -AFX_STALE_GENERATION);
    maps_empty();
    assert(g_available_channels == UINT64_MAX);
    /* A PAUSE behind the ACTIVATE, then a STOP behind the PAUSE. */
    assert(afx_instance_activate(flow, &instance) == AFX_OK);
    index = AFX_HANDLE_INDEX(instance);
    head = queue_head();
    assert(afx_instance_pause(instance) == AFX_OK && afx_instance_pause(instance) == AFX_OK);
    assert(g_available_channels != UINT64_MAX); /* A paused instance keeps its channel. */
    assert(afx_instance_stop(instance) == AFX_OK && queue_head() == head + 2u);
    assert(queued(head).opcode == AFX_CMD_PAUSE && queued(head + 1).opcode == AFX_CMD_STOP);
    assert(g_available_channels == UINT64_MAX);
    release_instance_preserving_generation(index);
    drain_queue();
    maps_empty();
}

static void test_stop_held_back(afx_asset_t flow) {
    /* A full ring holds a STOP back, with its channel; it goes before the next
     * command, or with the next report. */
    afx_instance_t instance, other;
    assert(afx_instance_activate(flow, &instance) == AFX_OK);
    assert(afx_instance_activate(flow, &other) == AFX_OK);
    uint32_t index = AFX_HANDLE_INDEX(instance), other_index = AFX_HANDLE_INDEX(other);
    observed(index, instance, AFX_RUNNING, g_instances[index].status.sequence, AFX_OK);
    observed(other_index, other, AFX_RUNNING, g_instances[other_index].status.sequence, AFX_OK);
    irq_run_worker();
    drain_queue();
    uint32_t head = queue_head();
    while (afx_instance_gain(other, 1) == AFX_OK) {}
    assert(queue_head() == head + AFX_CMD_QUEUE_CAPACITY);
    uint64_t available = g_available_channels, mask = g_instances[index].channel_mask;
    assert(afx_instance_stop(instance) == AFX_OK && g_endings_held == 1);
    assert(afx_instance_stop(instance) == AFX_OK && g_endings_held == 1);
    assert(g_available_channels == available && queue_head() == head + AFX_CMD_QUEUE_CAPACITY);
    /* The ARM7 takes eight: the STOP goes ahead of the next gain. */
    afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), head + 8u);
    assert(afx_instance_gain(other, 2) == AFX_OK && !g_endings_held);
    afx_cmd_t stop = queued(head + AFX_CMD_QUEUE_CAPACITY);
    assert(stop.opcode == AFX_CMD_STOP && stop.reference == instance);
    assert(queued(head + AFX_CMD_QUEUE_CAPACITY + 1).opcode == AFX_CMD_INSTANCE_GAIN);
    assert(g_available_channels == (available | mask));
    /* Full again: this time a report from the ARM7 sends it. */
    drain_queue();
    observed(index, instance, AFX_DONE, stop.sequence, AFX_OK);
    irq_run_worker();
    recycle(index);
    head = queue_head();
    while (afx_instance_gain(other, 1) == AFX_OK) {}
    assert(afx_instance_stop(other) == AFX_OK && g_endings_held == 1);
    drain_queue();
    /* The report of a refused PATCH, say. */
    observed(other_index, other, AFX_RUNNING, g_instances[other_index].status.sequence,
             AFX_BAD_COMMAND);
    irq_run_worker();
    assert(!g_endings_held && queued(queue_head() - 1).opcode == AFX_CMD_STOP);
    assert(g_available_channels == UINT64_MAX);
    release_instance_preserving_generation(other_index);
    drain_queue();
    maps_empty();
}

static void test_replace(afx_asset_t flow) {
    /* A flow that needs every channel replaces one that holds them all. */
    afx_asset_slot_t *asset = &g_assets[AFX_HANDLE_INDEX(flow)];
    asset->required_channels = 64;
    afx_instance_t old, next, refused = 0;
    assert(afx_instance_activate(flow, &old) == AFX_OK);
    uint32_t index = AFX_HANDLE_INDEX(old);
    observed(index, old, AFX_RUNNING, g_instances[index].status.sequence, AFX_OK);
    irq_run_worker();
    drain_queue();
    assert(afx_instance_activate(flow, &refused) == -AFX_NO_CHANNELS && !refused);
    /* Without room for both commands, nothing happens: old keeps playing. */
    uint32_t head = queue_head();
    for (unsigned i = 0; i < AFX_CMD_QUEUE_CAPACITY - 1u; ++i)
        assert(afx_instance_gain(old, 1) == AFX_OK);
    assert(afx_instance_replace(old, flow, &refused) == -AFX_IPC_FULL && !refused);
    assert(queue_head() == head + AFX_CMD_QUEUE_CAPACITY - 1u && !g_available_channels);
    assert(g_instances[index].channel_mask == UINT64_MAX && !g_instances[index].ending);
    drain_queue();
    head = queue_head();
    assert(afx_instance_replace(old, flow, &next) == AFX_OK);
    assert(queued(head).opcode == AFX_CMD_STOP && queued(head).reference == old);
    assert(queued(head + 1).opcode == AFX_CMD_ACTIVATE && queued(head + 1).reference == next);
    assert(g_instances[AFX_HANDLE_INDEX(next)].channel_mask == UINT64_MAX);
    assert(!g_instances[index].channel_mask && !g_available_channels);
    /* old is stopping: replacing it again only activates, and next has the channels. */
    assert(afx_instance_replace(old, flow, &refused) == -AFX_NO_CHANNELS && !refused);
    assert(afx_instance_replace(0, flow, &refused) == -AFX_NO_CHANNELS && !refused);
    release_instance_preserving_generation(index);
    release_instance_preserving_generation(AFX_HANDLE_INDEX(next));
    drain_queue();
    asset->required_channels = 1;
    assert(afx_instance_replace(0, flow, &next) == AFX_OK); /* Nothing to stop. */
    assert(queued(queue_head() - 1).opcode == AFX_CMD_ACTIVATE);
    release_instance_preserving_generation(AFX_HANDLE_INDEX(next));
    drain_queue();
    maps_empty();
    assert(g_available_channels == UINT64_MAX);
}

static void test_release_when_idle(const uint8_t bank_data[96], const uint8_t flow_data[160]) {
    /* A released flow is freed once its last instance is recycled, and a
     * released bank once the last flow that retains it is freed. */
    afx_bank_t bank = {0};
    afx_asset_t flow, refused_flow = 0;
    afx_instance_t instance, refused = 0;
    assert(afx_bank_load_memory(&bank, bank_data, 96) == AFX_OK);
    assert(afx_bank_flow_upload(&bank, flow_data, 160, &flow) == AFX_OK);
    uint32_t flow_index = AFX_HANDLE_INDEX(flow), bank_index = AFX_HANDLE_INDEX(bank.asset);
    assert(afx_instance_activate(flow, &instance) == AFX_OK);
    uint32_t index = AFX_HANDLE_INDEX(instance);
    assert(afx_asset_free(flow) == -AFX_ASSET_REFERENCED);
    assert(afx_asset_release(flow) == AFX_OK && g_assets[flow_index].live);
    assert(afx_asset_release(bank.asset) == AFX_OK && g_assets[bank_index].live);
    assert(afx_instance_activate(flow, &refused) == -AFX_INVALID_HANDLE && !refused);
    assert(afx_bank_flow_upload(&bank, flow_data, 160, &refused_flow) == -AFX_INVALID_HANDLE);
    drain_queue();
    observed(index, instance, AFX_DONE, g_instances[index].status.sequence, AFX_OK);
    irq_run_worker();
    assert(g_assets[flow_index].live);
    recycle(index);
    assert(!g_assets[flow_index].live && !g_assets[bank_index].live);
    assert(afx_asset_addr(flow) == 0 && afx_asset_addr(bank.asset) == 0);
    /* With nothing using it, release frees at once. */
    bank = (afx_bank_t){0};
    assert(afx_bank_load_memory(&bank, bank_data, 96) == AFX_OK);
    assert(afx_asset_release(bank.asset) == AFX_OK && afx_asset_addr(bank.asset) == 0);
    assert(g_available_channels == UINT64_MAX);
    maps_empty();
}

static void test_enqueue_tail_cache(afx_asset_t flow) {
    /* With the queue drained, a full queue's worth of commands reads the
     * ARM7's tail back at most once; a full queue is still reported, and room
     * the ARM7 makes is seen on the next command. */
    afx_instance_t instance;
    assert(afx_instance_activate(flow, &instance) == AFX_OK);
    uint32_t index = AFX_HANDLE_INDEX(instance);
    observed(index, instance, AFX_RUNNING, g_instances[index].status.sequence, AFX_OK);
    irq_run_worker();
    assert(!g_instances[index].pending);
    uint8_t *tail = ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail);
    afx_write32(tail, queue_head());
    unsigned before = tail_reads;
    unsigned sent = 0;
    while (afx_instance_gain(instance, (uint8_t)sent) == AFX_OK) ++sent;
    assert(sent == AFX_CMD_QUEUE_CAPACITY);
    assert(afx_instance_gain(instance, 1) == -AFX_IPC_FULL);
    /* One read when the cached view first looked full, one per refused command. */
    assert(tail_reads - before <= 3u);
    afx_write32(tail, queue_head() - 1u); /* the ARM7 consumed all but one */
    before = tail_reads;
    sent = 0;
    while (afx_instance_gain(instance, 2) == AFX_OK) ++sent;
    assert(sent == AFX_CMD_QUEUE_CAPACITY - 1u && tail_reads - before <= 2u);
    afx_write32(tail, queue_head());
    release_instance_preserving_generation(index);
    assert(g_available_channels == UINT64_MAX);
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

/* Another thread holding the driver lock, as an AICAflow call from an audio
 * thread does for a moment while the game thread loads a bank file. */
static pthread_mutex_t holder_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t holder_condition = PTHREAD_COND_INITIALIZER;
static pthread_t holder;
static int holder_holds, holder_release;
static unsigned holder_passes;
static void *hold_driver_lock(void *ignored) {
    (void)ignored;
    pthread_mutex_lock(&g_host_mutex);
    pthread_mutex_lock(&holder_mutex);
    holder_holds = 1;
    pthread_cond_broadcast(&holder_condition);
    while (!holder_release) pthread_cond_wait(&holder_condition, &holder_mutex);
    pthread_mutex_unlock(&holder_mutex);
    pthread_mutex_unlock(&g_host_mutex);
    return NULL;
}
/* The first pass is the DMA poll's wait: the chunk completes and the lock is
 * taken, so the polls that follow find it busy until the third pass. */
static void hold_lock_hook(void) {
    if (++holder_passes == 1) {
        holder_holds = holder_release = 0;
        assert(!pthread_create(&holder, NULL, hold_driver_lock, NULL));
        pthread_mutex_lock(&holder_mutex);
        while (!holder_holds) pthread_cond_wait(&holder_condition, &holder_mutex);
        pthread_mutex_unlock(&holder_mutex);
    } else if (holder_passes == 3) {
        pthread_mutex_lock(&holder_mutex);
        holder_release = 1;
        pthread_cond_broadcast(&holder_condition);
        pthread_mutex_unlock(&holder_mutex);
        assert(!pthread_join(holder, NULL));
    }
}
static void test_bank_load_busy_lock(const uint8_t bank_data[96]) {
    char path[] = "/tmp/afx_test_bank_XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    assert(file && fwrite(bank_data, 1, 96, file) == 96 && !fclose(file));
    afx_mem_stats_t before, after;
    assert(afx_mem_stats(&before) == AFX_OK);
    afx_bank_t bank = {0};
    holder_passes = 0;
    thd_pass_hook = hold_lock_hook;
    /* A busy lock during the load is waited out; giving up there left the
     * upload DMA claimed and the next load spinning on AFX_BUSY. */
    assert(afx_bank_load_file(&bank, path) == AFX_OK);
    thd_pass_hook = NULL;
    assert(holder_passes == 3 && !g_upload_dma_asset && bank.asset && bank.bytes == 64);
    assert(!memcmp(ram + afx_asset_addr(bank.asset), bank_data + AFX_BANK_HEADER_BYTES, 64));
    assert(afx_bank_release(&bank) == AFX_OK);
    assert(afx_bank_load_file(&bank, path) == AFX_OK && afx_bank_release(&bank) == AFX_OK);
    assert(afx_mem_stats(&after) == AFX_OK && after.free_bytes == before.free_bytes);
    assert(!unlink(path));
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
        irq_run_worker();
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
        irq_run_worker();
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

/* A flow with AFX_FLAG_BANKS over banks a and b (slots 0 and 1) whose one
 * setup plays from `slot` at `offset`: relocation at 80, bank list at 92,
 * image at 128 with the bank table at image offset 48. */
static void banked_flow(uint8_t data[192], const afx_bank_t *a, const afx_bank_t *b,
                        uint32_t slot, uint32_t offset) {
    memset(data, 0, 192);
    afx_write32(data, AFX_FILE_MAGIC); afx_write32(data + 4, AFX_FILE_VERSION);
    afx_write32(data + 8, 192); afx_write32(data + 12, AFX_FLAG_BANKS);
    afx_write32(data + 16, 128); afx_write32(data + 20, 64);
    afx_write32(data + 24, AFX_SETUP_BYTES); afx_write32(data + 28, 9);
    afx_write32(data + 32, 1); afx_write32(data + 36, 1);
    afx_write32(data + 40, a->id.low); afx_write32(data + 44, a->id.high);
    afx_write32(data + 48, 80); afx_write32(data + 52, 1); afx_write32(data + 60, 92);
    afx_write32(data + 64, 1); afx_write32(data + 68, 1000); afx_write32(data + 72, 1);
    afx_write32(data + 76, AFX_WORK_PROFILE(1, 19));
    afx_write32(data + 84, (slot << AFX_BANK_SLOT_SHIFT) | offset); afx_write32(data + 88, 32);
    afx_write32(data + 92, 1); afx_write32(data + 96, b->id.low); afx_write32(data + 100, b->id.high);
    afx_write16(data + 128, (uint16_t)((slot << 5) | (offset >> 16)));
    afx_write16(data + 130, (uint16_t)offset);
    afx_write16(data + 128 + 6, 31);
    data[128 + AFX_SETUP_BYTES] = AFX_OP_NOTE_PL;
    data[128 + AFX_SETUP_BYTES + 8] = AFX_OP_END;
}

/* One flow plays from two banks: each slot is checked against its own bank,
 * the ARM7's bank table is filled at upload, and the flow keeps both banks. */
static void test_flow_banks(void) {
    uint8_t core_data[96], scene_data[96], file[192], seek[104] = {0};
    afx_bank_t core = {0}, scene = {0};
    bank_file(core_data, 0xc0de, 1);
    bank_file(scene_data, 0x5ce7e, 2);
    assert(afx_bank_load_memory(&core, core_data, sizeof(core_data)) == AFX_OK);
    assert(afx_bank_load_memory(&scene, scene_data, sizeof(scene_data)) == AFX_OK);
    const afx_bank_t *banks[] = {&core, &scene}, *swapped[] = {&scene, &core};
    afx_asset_t flow;
    banked_flow(file, &core, &scene, 1, 0x20);
    assert(afx_flow_upload(banks, 1, file, sizeof(file), &flow) == -AFX_BAD_SAMPLE);
    assert(afx_bank_flow_upload(&core, file, sizeof(file), &flow) == -AFX_BAD_SAMPLE);
    assert(afx_flow_upload(swapped, 2, file, sizeof(file), &flow) == -AFX_BAD_SAMPLE);
    banked_flow(file, &core, &scene, 1, 0x30); /* 32 bytes from 0x30 overrun the scene bank. */
    assert(afx_flow_upload(banks, 2, file, sizeof(file), &flow) == -AFX_BAD_FORMAT);
    assert(flow == AFX_ASSET_INVALID && !g_flow_bank_count);
    banked_flow(file, &core, &scene, 1, 0x20);
    assert(afx_flow_upload(banks, 2, file, sizeof(file), &flow) == AFX_OK);
    uint32_t core_index = AFX_HANDLE_INDEX(core.asset), scene_index = AFX_HANDLE_INDEX(scene.asset);
    assert(g_assets[core_index].references == 1 && g_assets[scene_index].references == 1);
    assert(flow_bank(AFX_HANDLE_INDEX(flow), 1) == scene.asset);
    assert(flow_bank(AFX_HANDLE_INDEX(flow), 2) == AFX_ASSET_INVALID);
    const uint8_t *table = ram + afx_asset_addr(flow) + 48;
    assert(afx_read32(table) == 0 && afx_read32(table + 8) == 0 && afx_read32(table + 12) == 0);
    assert(afx_read32(table + 4) == AFX_BANK_TABLE_ENTRY(afx_asset_addr(scene.asset), 64));

    /* The activation carries the flag; slot 0 is the core bank, as before.
     * A checkpoint state is checked against the bank of its own slot: slot 1
     * inside the scene bank, slot 3 (empty), and slot 1 past its end. */
    afx_write32(seek, AFX_SEEK_MAGIC); afx_write32(seek + 4, AFX_SEEK_VERSION);
    afx_write32(seek + 8, 1); afx_write32(seek + 12, core.id.low);
    afx_write32(seek + 16, core.id.high); afx_write32(seek + 20, 32);
    afx_write32(seek + 24, 72); afx_write32(seek + 28, 104);
    afx_write32(seek + 32, AFX_CHECKPOINT_MAGIC);
    afx_write32(seek + 36, AFX_CHECKPOINT_VERSION); afx_write32(seek + 40, 1);
    afx_write32(seek + 52, AFX_SETUP_BYTES); afx_write32(seek + 60, 1);
    const uint16_t controls[] = {0x0020, 0x0060, 0x0020};
    const uint16_t lows[] = {0x0020, 0x0000, 0x0040};
    for (unsigned i = 0; i < 3; ++i) {
        afx_write16(seek + 68, controls[i]); afx_write16(seek + 70, lows[i]);
        assert(afx_flow_seek_index_load_memory(flow, seek, sizeof(seek)) == AFX_OK);
        afx_instance_t instance;
        assert(afx_instance_activate(flow, &instance) == AFX_OK);
        afx_cmd_t command = queued(queue_head() - 1);
        afx_activation_t activation;
        memcpy(&activation, command.payload, sizeof(activation));
        assert(activation.flags == AFX_FLAG_BANKS);
        assert(((uint32_t)activation.bank_base_units << 5) == afx_asset_addr(core.asset));
        uint32_t index = AFX_HANDLE_INDEX(instance);
        observed(index, instance, AFX_PAUSED, command.sequence, AFX_OK);
        irq_run_worker();
        assert(afx_instance_seek(instance, 0) == (i ? -AFX_BAD_FORMAT : AFX_OK));
        release_instance_preserving_generation(index);
        afx_write32(ram + AFX_QUEUE_ADDR + offsetof(afx_cmd_queue_t, tail), queue_head());
    }

    /* Released banks live until the flow that plays from them is freed. */
    assert(afx_asset_release(core.asset) == AFX_OK && afx_asset_release(scene.asset) == AFX_OK);
    assert(afx_asset_addr(core.asset) && afx_asset_addr(scene.asset));
    assert(afx_asset_free(flow) == AFX_OK && !g_flow_bank_count);
    assert(!afx_asset_addr(core.asset) && !afx_asset_addr(scene.asset));
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

/* One entry of a test scene; files are copied in 32-byte aligned. */
typedef struct {
    uint32_t kind;
    const char *name;
    const uint8_t *file;
    uint32_t file_size, offset, bytes, ticks;
} scene_part_t;
/* An AFS file of parts, in order: header, entries, names, files. Returns its size. */
static uint32_t scene_file(uint8_t *out, uint32_t capacity, uint32_t id, uint32_t block_bytes,
                           const scene_part_t *parts, uint32_t count) {
    uint32_t at = AFX_SCENE_HEADER_BYTES + count * AFX_SCENE_ENTRY_BYTES;
    memset(out, 0, capacity);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *entry = out + AFX_SCENE_HEADER_BYTES + i * AFX_SCENE_ENTRY_BYTES;
        afx_write32(entry, parts[i].kind);
        if (parts[i].name) {
            afx_write32(entry + 4, at);
            memcpy(out + at, parts[i].name, strlen(parts[i].name) + 1);
            at += (uint32_t)strlen(parts[i].name) + 1;
        }
        afx_write32(entry + 16, parts[i].offset);
        afx_write32(entry + 20, parts[i].bytes);
        afx_write32(entry + 24, parts[i].ticks);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!parts[i].file) continue;
        uint8_t *entry = out + AFX_SCENE_HEADER_BYTES + i * AFX_SCENE_ENTRY_BYTES;
        at = (at + 31u) & ~31u;
        assert(at + parts[i].file_size <= capacity);
        memcpy(out + at, parts[i].file, parts[i].file_size);
        afx_write32(entry + 8, at);
        afx_write32(entry + 12, parts[i].file_size);
        at += parts[i].file_size;
    }
    afx_write32(out, AFX_SCENE_MAGIC); afx_write32(out + 4, AFX_SCENE_VERSION);
    afx_write32(out + 8, id); afx_write32(out + 12, 0);
    afx_write32(out + 16, at); afx_write32(out + 20, block_bytes);
    afx_write32(out + 24, count);
    return at;
}
static void end_instance(afx_instance_t instance) {
    uint32_t index = AFX_HANDLE_INDEX(instance);
    drain_queue();
    observed(index, instance, AFX_DONE, g_instances[index].status.sequence, AFX_OK);
    irq_run_worker();
    recycle(index);
}

/* A permanent scene (core bank, a flow, a DSP ring) and a level scene whose
 * flow also plays from the core bank and whose slot takes a song later. */
static void test_scenes(void) {
    _Alignas(32) static uint8_t permanent[1024], level[1024], broken[1024];
    uint8_t core_data[96], level_data[96], song_data[96], big_data[256];
    uint8_t jingle[160], theme[192], song[160];
    const afx_bank_t core = {0, {0xc0de, 3}, 64}, level_bank = {0, {0x1e7e1, 4}, 64};
    bank_file(core_data, core.id.low, core.id.high);
    bank_file(level_data, level_bank.id.low, level_bank.id.high);
    bank_file(song_data, 0x5096, 5);
    for (uint32_t i = 0; i < 64; ++i) core_data[32 + i] = (uint8_t)i;
    memset(big_data, 0, sizeof(big_data));
    memcpy(big_data, song_data, 32);
    afx_write32(big_data + 20, 224); afx_write32(big_data + 24, 256);
    flow_file(jingle, core.id.low, core.id.high);
    banked_flow(theme, &core, &level_bank, 1, 0x20);
    flow_file(song, 0x5096, 5);
    const scene_part_t permanent_parts[] = {
        {AFX_SCENE_BANK, "core", core_data, 96, 0, 64, 0},
        {AFX_SCENE_RING, "ring", NULL, 0, 2048, AFX_SCENE_RING_MIN_BYTES, 0},
        {AFX_SCENE_FLOW, "jingle", jingle, 160, 64, 64, 9},
    };
    const scene_part_t level_parts[] = {
        {AFX_SCENE_BANK, NULL, level_data, 96, 0, 64, 0},
        {AFX_SCENE_FLOW, "theme", theme, 192, 64, 64, 9},
        {AFX_SCENE_SLOT, "song", NULL, 0, 128, 256, 0},
    };
    uint32_t permanent_bytes = scene_file(permanent, sizeof(permanent), 1, 2048 + AFX_SCENE_RING_MIN_BYTES,
                                          permanent_parts, 3);
    uint32_t level_bytes = scene_file(level, sizeof(level), 2, 384, level_parts, 3);

    /* The validator: layout, kinds, names and block ranges. */
    afx_scene_header_t header;
    assert(afx_scene_validate(permanent, permanent_bytes, &header) == AFX_OK);
    assert(header.block_bytes == 2048 + AFX_SCENE_RING_MIN_BYTES && header.entry_count == 3);
    assert(!strcmp(afx_scene_entry_name(permanent, &(afx_scene_entry_t){0}), ""));
    afx_scene_entry_t entry = afx_scene_entry_at(permanent, 2);
    assert(entry.kind == AFX_SCENE_FLOW && entry.ticks == 9 && entry.file_size == 160);
    assert(!strcmp(afx_scene_entry_name(permanent, &entry), "jingle"));
    assert(afx_scene_validate(level, level_bytes, NULL) == AFX_OK);
    const struct { uint32_t at, value, result; } cases[] = {
        {0, 0, AFX_BAD_FORMAT}, {4, 2, AFX_BAD_FORMAT}, {8, 0, AFX_BAD_FORMAT},
        {16, 0, AFX_BAD_BOUNDS}, {20, 0, AFX_BAD_BOUNDS}, {20, 400, AFX_BAD_BOUNDS},
        {24, 0, AFX_BAD_BOUNDS}, {24, 1000, AFX_BAD_BOUNDS}, {28, 1, AFX_BAD_FORMAT},
        {32, 0, AFX_BAD_FORMAT}, {32, 5, AFX_BAD_FORMAT}, {60, 1, AFX_BAD_FORMAT},
        {56, 1, AFX_BAD_FORMAT}, /* ticks on a bank */
        {48, 32, AFX_BAD_BOUNDS}, /* the bank overlaps the flow */
        {48, 16, AFX_BAD_BOUNDS}, /* not on a 32-byte boundary */
        {52, 0, AFX_BAD_BOUNDS}, {52, 48, AFX_BAD_BOUNDS},
        {44, 31, AFX_BAD_BOUNDS}, /* an AFB smaller than its header */
        {40, 8, AFX_BAD_BOUNDS}, /* a file inside the entries */
        {36, 4096, AFX_BAD_BOUNDS}, /* a name past the end */
        {36, 8, AFX_BAD_BOUNDS}, /* a name inside the entries */
        {96, AFX_SCENE_RING, AFX_BAD_BOUNDS}, /* the slot as a ring, not on a 2 KiB boundary */
        {112, 128 + 1024, AFX_BAD_BOUNDS}, /* the slot outside the block */
        {108, 64, AFX_BAD_BOUNDS}, /* a slot with a file */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t saved = afx_read32(level + cases[i].at);
        afx_write32(level + cases[i].at, cases[i].value);
        assert(afx_scene_validate(level, level_bytes, NULL) == cases[i].result);
        afx_write32(level + cases[i].at, saved);
    }
    assert(afx_scene_validate(level, level_bytes, NULL) == AFX_OK);
    uint8_t *names = level + AFX_SCENE_HEADER_BYTES + 3 * AFX_SCENE_ENTRY_BYTES;
    memcpy(names, "song", 5); /* Two entries named "song". */
    assert(afx_scene_validate(level, level_bytes, NULL) == AFX_BAD_FORMAT);
    names[0] = 0; /* An empty name. */
    assert(afx_scene_validate(level, level_bytes, NULL) == AFX_BAD_FORMAT);
    memcpy(names, "theme", 5);
    assert(afx_scene_validate(level, level_bytes, NULL) == AFX_OK);
    afx_write32(permanent + 32 + 32 + 20, 0x2000); /* An 8 KiB ring: no RBL has that size. */
    assert(afx_scene_validate(permanent, permanent_bytes, NULL) == AFX_BAD_BOUNDS);
    afx_write32(permanent + 32 + 32 + 20, AFX_SCENE_RING_MIN_BYTES);
    afx_write32(permanent + 32 + 2 * 32, AFX_SCENE_RING); /* Two rings. */
    afx_write32(permanent + 32 + 2 * 32 + 8, 0); afx_write32(permanent + 32 + 2 * 32 + 12, 0);
    afx_write32(permanent + 32 + 2 * 32 + 24, 0);
    assert(afx_scene_validate(permanent, permanent_bytes, NULL) == AFX_BAD_FORMAT);
    permanent_bytes = scene_file(permanent, sizeof(permanent), 1, 2048 + AFX_SCENE_RING_MIN_BYTES,
                                 permanent_parts, 3);

    /* Loading: one block, each entry at its planned place. */
    afx_mem_stats_t before, during, after;
    assert(afx_mem_stats(&before) == AFX_OK);
    afx_scene_t p = 0, l = 0, refused = 0;
    afx_asset_t flow = 0;
    assert(afx_scene_load(level, level_bytes, NULL, 0, &refused) == -AFX_BAD_SAMPLE && !refused);
    assert(afx_mem_stats(&after) == AFX_OK && after.free_bytes == before.free_bytes);
    assert(after.active_allocations == before.active_allocations);
    assert(afx_scene_load(permanent, permanent_bytes, NULL, 0, &p) == AFX_OK && p);
    afx_scene_info_t info;
    assert(afx_scene_entry(p, 0, &info) == AFX_OK && info.kind == AFX_SCENE_BANK && !strcmp(info.name, "core"));
    uint32_t block = info.addr;
    assert(!(block & (AFX_SCENE_RING_ALIGN - 1u)) && afx_asset_addr(info.asset) == block);
    assert(!memcmp(ram + block, core_data + 32, 64));
    afx_asset_t core_asset = info.asset;
    assert(afx_scene_entry(p, 1, &info) == AFX_OK && info.kind == AFX_SCENE_RING && !info.asset);
    assert(info.addr == block + 2048 && info.bytes == AFX_SCENE_RING_MIN_BYTES);
    assert(afx_scene_entry(p, 2, &info) == AFX_OK && info.ticks == 9 && !info.loops);
    assert(afx_scene_entry(p, 3, &info) == -AFX_BAD_BOUNDS);
    assert(afx_scene_find(p, "jingle", &flow) == AFX_OK && flow == info.asset);
    assert(afx_asset_addr(flow) == block + 64 && !memcmp(ram + block + 64, jingle + 96, 64));
    assert(g_assets[AFX_HANDLE_INDEX(flow)].bank == core_asset);
    assert(afx_scene_find(p, "core", &flow) == -AFX_INVALID_HANDLE && !flow);
    assert(afx_scene_find(p, "ring", &flow) == -AFX_INVALID_HANDLE);
    assert(afx_mem_stats(&during) == AFX_OK && during.active_allocations == before.active_allocations + 1);
    assert(during.used_bytes == before.used_bytes + 2048 + AFX_SCENE_RING_MIN_BYTES);
    /* The scene's assets go with it alone. */
    assert(afx_asset_free(core_asset) == -AFX_ASSET_REFERENCED);
    assert(afx_asset_release(core_asset) == -AFX_ASSET_REFERENCED);
    assert(afx_mem_free(block) == -AFX_ASSET_REFERENCED);
    afx_bank_t loose = {0};
    assert(afx_bank_load_memory(&loose, core_data, sizeof(core_data)) == AFX_OK);
    assert(afx_bank_release(&loose) == AFX_OK);

    /* The DSP takes the scene's ring and gives it back to the scene. */
    afx_dsp_program_t dsp;
    assert(afx_dsp_program_delay(&dsp, 100, 8192, false) == AFX_OK);
    auto_dsp_ack = 1;
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == AFX_OK);
    assert(g_dsp_ring == block + 2048 && g_dsp_ring_placed);
    assert(afx_mem_stats(&after) == AFX_OK && after.used_bytes == during.used_bytes);
    uint32_t word = 0;
    assert(afx_mem_upload(block + 2048 + 4, &word, sizeof(word)) == -AFX_ASSET_REFERENCED);
    assert(afx_mem_upload(block + 128, &word, sizeof(word)) == AFX_OK);
    assert(afx_scene_release(p) == -AFX_ASSET_REFERENCED);
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 1) == AFX_OK); /* Too big: an allocation. */
    assert(!g_dsp_ring_placed && g_dsp_ring && g_dsp_ring != block + 2048);
    assert(afx_dsp_scene_program_ring(&dsp, sizeof(dsp), 0) == AFX_OK && g_dsp_ring == block + 2048);
    assert(afx_dsp_scene_disable() == AFX_OK && !g_dsp_ring && !g_dsp_ring_placed);
    auto_dsp_ack = 0;
    assert(afx_mem_stats(&after) == AFX_OK && after.used_bytes == during.used_bytes);

    /* The level's flow plays slot 1 from its own bank and slot 0 from the core bank. */
    uint32_t core_index = AFX_HANDLE_INDEX(core_asset);
    assert(afx_scene_load(level, level_bytes, &p, 1, &l) == AFX_OK && l);
    assert(g_assets[core_index].references == 2);
    afx_asset_t theme_flow, song_flow, stale;
    assert(afx_scene_find(l, "theme", &theme_flow) == AFX_OK);
    assert(g_assets[AFX_HANDLE_INDEX(theme_flow)].bank == core_asset);
    assert(afx_scene_entry(l, 0, &info) == AFX_OK && !strcmp(info.name, "") &&
           flow_bank(AFX_HANDLE_INDEX(theme_flow), 1) == info.asset);
    uint32_t level_block = info.addr;
    assert(afx_read32(ram + afx_asset_addr(theme_flow) + 48 + 4) == AFX_BANK_TABLE_ENTRY(level_block, 64));

    /* A slot: a bank and a flow, the flow after the bank, in place of what it held. */
    assert(afx_scene_find(l, "song", &song_flow) == -AFX_INVALID_HANDLE);
    assert(afx_scene_slot_load(l, "theme", song_data, 96, song, 160, &song_flow) == -AFX_INVALID_HANDLE);
    assert(afx_scene_slot_load(l, "song", NULL, 0, song, 160, &song_flow) == -AFX_BAD_SAMPLE);
    assert(afx_scene_slot_load(l, "song", song_data, 96, song, 160, &song_flow) == AFX_OK);
    assert(afx_asset_addr(song_flow) == level_block + 128 + 64);
    assert(afx_asset_addr(g_assets[AFX_HANDLE_INDEX(song_flow)].bank) == level_block + 128);
    assert(afx_scene_find(l, "song", &flow) == AFX_OK && flow == song_flow);
    afx_instance_t instance;
    assert(afx_instance_activate(song_flow, &instance) == AFX_OK);
    assert(afx_scene_slot_load(l, "song", song_data, 96, song, 160, &flow) == -AFX_ASSET_REFERENCED);
    assert(afx_scene_find(l, "song", &flow) == AFX_OK && flow == song_flow);
    end_instance(instance);
    stale = song_flow;
    assert(afx_scene_slot_load(l, "song", song_data, 96, song, 160, &song_flow) == AFX_OK);
    assert(song_flow != stale && !afx_asset_addr(stale) && afx_asset_addr(song_flow) == level_block + 192);
    /* Too big for the slot: refused, and the slot is empty. */
    assert(afx_scene_slot_load(l, "song", big_data, 256, song, 160, &flow) == -AFX_NO_AICA_RAM);
    assert(afx_scene_find(l, "song", &flow) == -AFX_INVALID_HANDLE && !afx_asset_addr(song_flow));
    assert(afx_scene_slot_load(l, "song", song_data, 96, song, 160, &song_flow) == AFX_OK);
    assert(afx_mem_stats(&after) == AFX_OK && after.active_allocations == before.active_allocations + 2);

    /* Release: the permanent scene keeps its block while the level binds its core
     * bank; the level keeps its own while its theme plays. */
    assert(afx_instance_activate(theme_flow, &instance) == AFX_OK);
    assert(afx_scene_release(p) == AFX_OK);
    assert(afx_scene_find(p, "jingle", &flow) == -AFX_INVALID_HANDLE && afx_scene_release(p) == -AFX_INVALID_HANDLE);
    assert(afx_asset_addr(core_asset) == block && afx_scene_load(level, level_bytes, &p, 1, &refused) == -AFX_INVALID_HANDLE);
    assert(afx_scene_release(l) == AFX_OK);
    assert(afx_mem_stats(&after) == AFX_OK && after.active_allocations == before.active_allocations + 2);
    assert(!afx_asset_addr(song_flow) && afx_asset_addr(theme_flow));
    assert(afx_instance_activate(theme_flow, &refused) == -AFX_INVALID_HANDLE);
    end_instance(instance);
    assert(!afx_asset_addr(theme_flow) && !afx_asset_addr(core_asset));
    assert(afx_mem_stats(&after) == AFX_OK && after.free_bytes == before.free_bytes);
    assert(after.active_allocations == before.active_allocations);
    assert(after.largest_free_block == before.largest_free_block);

    /* A failure part-way loads nothing. */
    memcpy(broken, level, level_bytes);
    afx_write32(broken + afx_read32(broken + 32 + 32 + 8) + 4, 6); /* theme's AFX version */
    assert(afx_scene_load(permanent, permanent_bytes, NULL, 0, &p) == AFX_OK);
    assert(afx_scene_load(broken, level_bytes, &p, 1, &refused) == -AFX_BAD_FORMAT && !refused);
    /* The core bank is bound by the jingle alone again. */
    assert(afx_scene_entry(p, 0, &info) == AFX_OK && g_assets[AFX_HANDLE_INDEX(info.asset)].references == 1);
    assert(afx_mem_stats(&after) == AFX_OK && after.active_allocations == before.active_allocations + 1);
    assert(afx_scene_release(p) == AFX_OK);
    assert(afx_mem_stats(&after) == AFX_OK && after.free_bytes == before.free_bytes);
    /* Left loaded: afx_shutdown forgets it with the allocator. */
    assert(afx_scene_load(permanent, permanent_bytes, NULL, 0, &p) == AFX_OK);
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
    test_events();
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
    test_bank_load_busy_lock(bank_data);
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
    test_coinciding_work(flow);
    test_stop_behind_activate(flow);
    test_stop_held_back(flow);
    test_replace(flow);
    test_enqueue_tail_cache(flow);
    test_release_when_idle(bank_data, flow_data);
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
    test_flow_banks();
    test_scenes();
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
    assert(!irq_handler);
    /* A firmware whose boot report never arrives does not start. */
    irq_boot_raise = 0;
    assert(afx_init(fw, sizeof(fw)) == -AFX_TIMEOUT && !running);
    assert(!irq_handler && !(afx_read32(dsp_registers + AFX_AICA_MCIEB) & AFX_AICA_MCI_SCPU));
    return 0;
}
