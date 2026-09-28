#ifndef AICAFLOW_EXAMPLE_ASSET_READ_H
#define AICAFLOW_EXAMPLE_ASSET_READ_H

#include <stdio.h>
#include <stdint.h>

/* KOS ISO9660 streams aligned reads. Its asynchronous sub-32-byte tail
 * can stall in Flycast; finish the aligned body, then seek to stop that
 * stream before reading the tail through the sector cache. Files using
 * this helper must be seekable and opened with _IONBF. */
static size_t asset_read(void *buffer, size_t bytes, FILE *file) {
    size_t body = bytes & ~(size_t)31;
    size_t got = body ? fread(buffer, 1, body, file) : 0;
    if (got != body || bytes == body) return got;
    long position = ftell(file);
    if (position < 0) return got;
    if (position && (fseek(file, position - 1, SEEK_SET) ||
                     fseek(file, position, SEEK_SET))) return got;
    return got + fread((uint8_t *)buffer + got, 1, bytes - got, file);
}

#endif
