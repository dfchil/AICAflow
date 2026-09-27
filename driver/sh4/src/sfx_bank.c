#include <aicaflow/sfx_bank.h>
#include <kos/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__sh__)
#include <malloc.h>
#endif

#define AFX_BANK_DMA_CHUNK 65536u
#define AFX_BANK_BUFFER_BYTES (AFX_BANK_DMA_CHUNK + AFX_UPLOAD_ALIGN)

static int range(uint32_t size, uint32_t offset, uint32_t count,
                 uint32_t stride) {
  return offset <= size && count <= (size - offset) / stride;
}

static int read_at(FILE* file, uint32_t offset, void* data, uint32_t bytes) {
  return fseek(file, (long)offset, SEEK_SET) ||
         fread(data, 1, bytes, file) != bytes;
}

static void* bank_buffer_alloc(void) {
#if defined(__sh__)
  return memalign(AFX_UPLOAD_ALIGN, AFX_BANK_BUFFER_BYTES);
#else
  return aligned_alloc(AFX_UPLOAD_ALIGN, AFX_BANK_BUFFER_BYTES);
#endif
}

static int sample_offset_compare(const void* left, const void* right) {
  const afx_sfx_bank_sample_t *a = left, *b = right;
  return a->offset > b->offset ? 1 : a->offset < b->offset ? -1 : 0;
}

static int upload_sample_region(FILE* file, const afx_sfx_bank_header_t* header,
                                const afx_sfx_bank_sample_t* records,
                                afx_asset_t* out) {
  uint8_t* buffer = bank_buffer_alloc();
  afx_sfx_bank_sample_t* ordered = NULL;
  afx_asset_t bank = AFX_ASSET_INVALID;
  uint32_t cursor = 0;
  uint32_t bytes = header->size - header->data_offset;
  int result = -AFX_NO_HOST_RAM;
  if (!buffer) goto done;
  ordered = malloc(header->sample_count * sizeof(*ordered));
  if (!ordered) goto done;
  memcpy(ordered, records, header->sample_count * sizeof(*ordered));
  qsort(ordered, header->sample_count, sizeof(*ordered), sample_offset_compare);
  for (uint32_t i = 0, previous_end = header->data_offset;
       i < header->sample_count; ++i) {
    uint32_t end;
    if (!ordered[i].bytes || !ordered[i].frames ||
        ordered[i].format > AFX_ADPCM ||
        ordered[i].offset < header->data_offset ||
        !range(header->size, ordered[i].offset, ordered[i].bytes, 1) ||
        ordered[i].offset < previous_end) {
      result = -AFX_BAD_FORMAT;
      goto done;
    }
    end = ordered[i].offset + ordered[i].bytes;
    previous_end = end;
  }
  do {
    result = afx_sample_bank_stream_begin(bytes, &bank);
    if (result == -AFX_BUSY) thd_pass();
  } while (result == -AFX_BUSY);
  if (result) goto done;
  for (uint32_t i = 0; i < header->sample_count; ++i) {
    const afx_sfx_bank_sample_t* sample = &ordered[i];
    uint32_t relative = sample->offset - header->data_offset;
    uint32_t next = i + 1u < header->sample_count
                        ? ordered[i + 1u].offset - header->data_offset
                        : bytes;
    uint32_t remaining = sample->bytes;
    uint32_t gap = next - (relative + sample->bytes);
    if (relative != cursor || gap >= AFX_UPLOAD_ALIGN ||
        fseek(file, (long)sample->offset, SEEK_SET)) {
      result = -AFX_BAD_FORMAT;
      goto done;
    }
    while (remaining) {
      uint32_t payload = remaining > AFX_BANK_DMA_CHUNK ? AFX_BANK_DMA_CHUNK : remaining;
      uint32_t count = payload + (payload == remaining ? gap : 0u);
      memset(buffer, 0, AFX_BANK_BUFFER_BYTES);
      if (fread(buffer, 1, payload, file) != payload) {
        result = -AFX_BAD_FORMAT;
        goto done;
      }
      do {
        result = afx_sample_bank_stream_dma_begin(bank, cursor, buffer, count);
        if (result == -AFX_BUSY) thd_pass();
      } while (result == -AFX_BUSY);
      if (result) goto done;
      for (;;) {
        bool complete = false;
        result = afx_sample_bank_stream_dma_poll(bank, &complete);
        if (complete || (result && result != -AFX_BUSY)) break;
        thd_pass();
      }
      if (result) goto done;
      cursor += count;
      remaining -= payload;
    }
  }
  do {
    result = afx_sample_bank_stream_finish(bank);
    if (result == -AFX_BUSY) thd_pass();
  } while (result == -AFX_BUSY);
  if (!result) *out = bank;
done:
  if (result && bank) (void)afx_asset_free(bank);
  free(ordered);
  free(buffer);
  return result;
}

static void dispose(afx_sfx_bank_t* bank) {
  free(bank->samples);
  free(bank->flows);
  free(bank->header);
  free(bank->sounds);
  free(bank->setups);
  free(bank->path);
  *bank = (afx_sfx_bank_t){0};
}

int afx_sfx_bank_release(afx_sfx_bank_t* bank) {
  uint32_t i;
  int result;
  if (!bank) return -AFX_BAD_BOUNDS;
  for (i = 0; bank->flows && bank->header && i < bank->header->sound_count;
       ++i) {
    if (bank->flows[i] && (result = afx_asset_free(bank->flows[i])))
      return result;
    bank->flows[i] = AFX_ASSET_INVALID;
  }
  for (i = 0; bank->samples && bank->header && i < bank->header->sample_count;
       ++i) {
    if (bank->samples[i] && (result = afx_asset_free(bank->samples[i])))
      return result;
    bank->samples[i] = AFX_ASSET_INVALID;
  }
  if (bank->sample_data && (result = afx_asset_free(bank->sample_data)))
    return result;
  bank->sample_data = AFX_ASSET_INVALID;
  dispose(bank);
  return AFX_OK;
}

int afx_sfx_bank_load_samples_file(afx_sfx_bank_t* bank, const char* path) {
  afx_sfx_bank_header_t header;
  afx_sfx_bank_sample_t* records = NULL;
  FILE* file = NULL;
  long file_size;
  uint32_t setup_count, i;
  int result = -AFX_BAD_FORMAT;

  if (!bank || !path || bank->header || !(file = fopen(path, "rb")) ||
      read_at(file, 0, &header, sizeof(header)) ||
      memcmp(header.magic, "AFB1", 4) ||
      header.version != AFX_SFX_BANK_VERSION || !header.sample_count ||
      fseek(file, 0, SEEK_END) || (file_size = ftell(file)) < 0 ||
      (uint32_t)file_size != header.size || header.size < sizeof(header) ||
      header.sample_offset != sizeof(header) ||
      !range(header.size, header.sample_offset, header.sample_count,
             sizeof(*records)) ||
      header.sound_offset !=
          header.sample_offset + header.sample_count * sizeof(*records) ||
      header.setup_offset < header.sound_offset ||
      header.stream_offset < header.setup_offset ||
      header.data_offset < header.stream_offset ||
      (header.stream_offset - header.setup_offset) % sizeof(*bank->setups) ||
      !range(header.size, header.sound_offset, header.sound_count,
             sizeof(*bank->sounds)))
    goto done;

  setup_count =
      (header.stream_offset - header.setup_offset) / sizeof(*bank->setups);
  bank->header = malloc(sizeof(*bank->header));
  bank->sounds = header.sound_count
                     ? malloc(header.sound_count * sizeof(*bank->sounds))
                     : NULL;
  bank->setups =
      setup_count ? malloc(setup_count * sizeof(*bank->setups)) : NULL;
  bank->samples = calloc(header.sample_count, sizeof(*bank->samples));
  bank->flows = header.sound_count
                    ? calloc(header.sound_count, sizeof(*bank->flows))
                    : NULL;
  records = malloc(header.sample_count * sizeof(*records));
  bank->path = malloc(strlen(path) + 1u);
  if (!bank->header ||
      (header.sound_count && (!bank->sounds || !bank->flows)) ||
      (setup_count && !bank->setups) || !bank->samples || !records ||
      !bank->path) {
    result = -AFX_NO_HOST_RAM;
    goto done;
  }
  strcpy(bank->path, path);
  *bank->header = header;
  if (read_at(file, header.sample_offset, records,
              header.sample_count * sizeof(*records)) ||
      (header.sound_count &&
       read_at(file, header.sound_offset, bank->sounds,
               header.sound_count * sizeof(*bank->sounds))) ||
      (setup_count && read_at(file, header.setup_offset, bank->setups,
                              setup_count * sizeof(*bank->setups))))
    goto done;

  result =
      upload_sample_region(file, &header, records, &bank->sample_data);
  if (result) goto done;
  for (i = 0; i < header.sample_count; ++i) {
    if (!records[i].bytes || !records[i].frames ||
        records[i].format > AFX_ADPCM ||
        records[i].offset < header.data_offset ||
        !range(header.size, records[i].offset, records[i].bytes, 1))
      goto done;
    result = afx_sample_view_create(bank->sample_data,
                                    records[i].offset - header.data_offset,
                                    records[i].bytes, records[i].frames,
                                    records[i].format, &bank->samples[i]);
    if (result) goto done;
  }

  bank->bytes = header.size;
  result = AFX_OK;
done:
  free(records);
  if (file) fclose(file);
  if (result) (void)afx_sfx_bank_release(bank);
  return result;
}

int afx_sfx_bank_control_upload(const afx_sfx_bank_t* bank, const char* path,
                                afx_asset_t* out_flow) {
  afx_sfx_control_header_t header;
  afx_sfx_bank_setup_t* packed = NULL;
  afx_sfx_setup_t* setups = NULL;
  uint8_t* data = NULL;
  FILE* file = NULL;
  long file_size;
  uint32_t lane_bytes, i;
  int result = -AFX_BAD_FORMAT;

  if (!out_flow) return -AFX_BAD_BOUNDS;
  *out_flow = AFX_ASSET_INVALID;
  if (!bank || !bank->header || !bank->samples || !path ||
      !(file = fopen(path, "rb")) ||
      read_at(file, 0, &header, sizeof(header)) ||
      memcmp(header.magic, "AFC1", 4) ||
      header.version != AFX_SFX_BANK_VERSION || !header.channels ||
      header.channels > AFX_MAX_FLOW_CHANNELS || !header.setup_count ||
      !header.stream_size ||
      header.flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC |
                       AFX_FLAG_MUSIC_CHORUS | AFX_FLAG_LANES) ||
      fseek(file, 0, SEEK_END) || (file_size = ftell(file)) < 0 ||
      (uint32_t)file_size != header.size)
    goto done;
  lane_bytes = header.flags & AFX_FLAG_LANES ? header.channels : 0;
  if (header.size != sizeof(header) + header.setup_count * sizeof(*packed) +
                         lane_bytes + header.stream_size)
    goto done;
  packed = malloc(header.setup_count * sizeof(*packed));
  setups = calloc(header.setup_count, sizeof(*setups));
  data = malloc(lane_bytes + header.stream_size);
  if (!packed || !setups || !data) {
    result = -AFX_NO_HOST_RAM;
    goto done;
  }
  if (read_at(file, sizeof(header), packed,
              header.setup_count * sizeof(*packed)) ||
      read_at(file, sizeof(header) + header.setup_count * sizeof(*packed), data,
              lane_bytes + header.stream_size))
    goto done;
  for (i = 0; i < header.setup_count; ++i) {
    if (packed[i].sample >= bank->header->sample_count) goto done;
    setups[i].sample = bank->samples[packed[i].sample];
    setups[i].byte_offset = packed[i].byte_offset;
    memcpy(setups[i].fields, packed[i].fields, sizeof(setups[i].fields));
  }
  {
    afx_external_flow_t recipe = {.setups = setups,
                                  .setup_count = header.setup_count,
                                  .stream = data + lane_bytes,
                                  .stream_size = header.stream_size,
                                  .required_channels = header.channels,
                                  .tick_rate_num = 1000,
                                  .tick_rate_den = 1,
                                  .flags = header.flags,
                                  .lane_map = lane_bytes ? data : NULL};
    result = afx_external_flow_upload(&recipe, out_flow);
    if (!result) result = afx_flow_release_host_image(*out_flow);
  }
done:
  if (file) fclose(file);
  free(data);
  free(setups);
  free(packed);
  return result;
}

int afx_sfx_bank_flow_upload(const afx_sfx_bank_t* bank, uint32_t index,
                             afx_asset_t* out_flow) {
  const afx_sfx_bank_sound_t* sound;
  afx_sfx_setup_t* setups = NULL;
  uint8_t* data = NULL;
  uint32_t setup_count, lane_bytes, j;
  FILE* file = NULL;
  int result = -AFX_BAD_FORMAT;

  if (!out_flow) return -AFX_BAD_BOUNDS;
  *out_flow = AFX_ASSET_INVALID;
  if (!bank || !bank->header || !bank->path || !bank->samples ||
      !bank->sounds || !bank->setups || index >= bank->header->sound_count)
    return -AFX_BAD_BOUNDS;
  sound = &bank->sounds[index];
  setup_count = (bank->header->stream_offset - bank->header->setup_offset) /
                sizeof(*bank->setups);
  lane_bytes = sound->flags & AFX_FLAG_LANES ? sound->channels : 0;
  if (!sound->channels || sound->channels > AFX_MAX_FLOW_CHANNELS ||
      sound->flags & ~(AFX_FLAG_CONTROLLED | AFX_FLAG_MUSIC |
                       AFX_FLAG_MUSIC_CHORUS | AFX_FLAG_LANES) ||
      sound->setup_first > setup_count ||
      sound->setup_count > setup_count - sound->setup_first ||
      !sound->setup_count || !sound->stream_size ||
      sound->stream_offset < bank->header->stream_offset + lane_bytes ||
      !range(bank->header->data_offset, sound->stream_offset,
             sound->stream_size, 1) ||
      !(file = fopen(bank->path, "rb")))
    goto done;
  setups = calloc(sound->setup_count, sizeof(*setups));
  data = malloc(sound->stream_size + lane_bytes);
  if (!setups || !data) {
    result = -AFX_NO_HOST_RAM;
    goto done;
  }
  if (read_at(file, sound->stream_offset - lane_bytes, data,
              sound->stream_size + lane_bytes))
    goto done;
  for (j = 0; j < sound->setup_count; ++j) {
    const afx_sfx_bank_setup_t* packed = &bank->setups[sound->setup_first + j];
    if (packed->sample >= bank->header->sample_count) goto done;
    setups[j].sample = bank->samples[packed->sample];
    setups[j].byte_offset = packed->byte_offset;
    memcpy(setups[j].fields, packed->fields, sizeof(setups[j].fields));
  }
  {
    afx_external_flow_t recipe = {.setups = setups,
                                  .setup_count = sound->setup_count,
                                  .stream = data + lane_bytes,
                                  .stream_size = sound->stream_size,
                                  .required_channels = sound->channels,
                                  .tick_rate_num = 1000,
                                  .tick_rate_den = 1,
                                  .flags = sound->flags,
                                  .lane_map = lane_bytes ? data : NULL};
    result = afx_external_flow_upload(&recipe, out_flow);
    if (!result) result = afx_flow_release_host_image(*out_flow);
  }
done:
  if (file) fclose(file);
  free(data);
  free(setups);
  return result;
}

int afx_sfx_bank_load_file(afx_sfx_bank_t* bank, const char* path) {
  int result = afx_sfx_bank_load_samples_file(bank, path);
  if (result) return result;
  for (uint32_t i = 0; i < bank->header->sound_count; ++i) {
    result = afx_sfx_bank_flow_upload(bank, i, &bank->flows[i]);
    if (result) {
      (void)afx_sfx_bank_release(bank);
      return result;
    }
  }
  return AFX_OK;
}
