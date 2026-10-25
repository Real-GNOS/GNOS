/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gzip.h — kernel-side gzip/DEFLATE decompression. (GPLv2)
 *
 * Ported from the Uinxed reference (libs/util/gzip.{c,h}) so the initrd can
 * ship gzip-compressed: Limine hands the kernel the raw module, and the
 * boot path sniffs the 1f 8b magic and decompresses before mounting.
 * Unlike the reference the output buffer comes from the physical frame
 * allocator (the kernel heap is far too small for a 1 GiB image), and the
 * SSE2 fast-copy path is dropped -- GNOS builds -mno-sse.
 */
#ifndef GNUCOS_GZIP_H
#define GNUCOS_GZIP_H

#include <stddef.h>
#include <stdint.h>

/* Decompress a gzip stream into a freshly frame-allocated buffer
 * (released only by leaking into the initrd's permanent home). */
int gzip_decompress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size);

#endif /* GNUCOS_GZIP_H */
