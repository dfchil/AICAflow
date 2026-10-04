#include <aicaflow/codec.h>

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) return fprintf(stderr, "usage: %s candidate.afx\n", argv[0]), 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input || fseek(input, 0, SEEK_END)) return fprintf(stderr, "cannot read %s\n", argv[1]), 2;
    long size = ftell(input);
    /* Host tables/metadata are not resident assets; validate image bounds below. */
    if (size <= 0 || (uint64_t)size > UINT32_MAX || fseek(input, 0, SEEK_SET)) return fclose(input), 2;
    uint8_t *bytes = malloc((size_t)size);
    if (!bytes || fread(bytes, 1, (size_t)size, input) != (size_t)size) return fclose(input), free(bytes), 2;
    fclose(input);
    afx_file_header_t header;
    afx_result_t result = afx_file_validate(bytes, (uint32_t)size, &header);
    free(bytes);
    if (result) return fprintf(stderr, "invalid AFX: %u\n", result), 1;
    printf("valid AFX: %ld bytes, %u channels, %u setups, %u stream bytes\n", size,
           header.required_channels, header.setup_count, header.stream_size);
    return 0;
}
