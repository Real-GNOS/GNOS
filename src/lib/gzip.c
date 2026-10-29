/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gzip.c — gzip/DEFLATE decompression. (GPLv2)
 *
 * Ported from the Uinxed reference (libs/util/gzip.c): RFC 1951 inflate
 * (fixed and dynamic Huffman blocks, stored blocks) inside the RFC 1952
 * gzip container, with the trailer's CRC-32 and ISIZE verified before the
 * result is accepted.
 *
 * Two deviations from the reference, both forced by GNOS's build:
 *   - the SSE2 16-byte literal fast path is gone (the kernel is compiled
 *     -mno-sse), so literals flush through plain memcpy;
 *   - the output buffer is a run of physical frames via
 *     pmm_alloc_contiguous() -- the kernel heap is 16 MiB and a compressed
 *     initrd decompresses far past that.  The buffer intentionally stays
 *     resident: it becomes the initrd the VFS mounts.
 */
#include <stddef.h>
#include <stdint.h>

#include "gzip.h"
#include "heap.h"
#include "kstring.h"
#include "pmm.h"
#include "vfs.h" /* errno names */

#include "drm_vsnprintf.h" /* plogk */

#define DEFLATE_MAX_BITS    15
#define DEFLATE_MAX_SYMBOLS 288

typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         offset;
    uint32_t       bits;
    unsigned int   bit_count;
} deflate_stream_t;

typedef struct {
    uint16_t counts[DEFLATE_MAX_BITS + 1];
    uint16_t symbols[DEFLATE_MAX_SYMBOLS];
} deflate_huffman_t;

static const uint16_t length_base[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23,  27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};

static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

static const uint16_t distance_base[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};

static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

/* Read count bits from the deflate stream, LSB-first per byte. */
static int read_bits(deflate_stream_t *stream, unsigned int count, uint32_t *value)
{
    while (stream->bit_count < count) {
        if (stream->offset >= stream->size)
            return -E_INVAL;
        stream->bits |= (uint32_t)stream->data[stream->offset++] << stream->bit_count;
        stream->bit_count += 8;
    }

    *value = stream->bits & ((1U << count) - 1U);
    stream->bits >>= count;
    stream->bit_count -= count;
    return 0;
}

/* Discard the partial bits so the next read starts on a byte boundary. */
static void align_to_byte(deflate_stream_t *stream)
{
    unsigned int discard = stream->bit_count & 7U;
    stream->bits >>= discard;
    stream->bit_count -= discard;
}

/* Build a canonical Huffman table from code lengths. */
static int build_huffman(deflate_huffman_t *tree, const uint8_t *lengths, size_t symbol_count)
{
    uint16_t offsets[DEFLATE_MAX_BITS + 1];
    int      remaining = 1;

    for (size_t i = 0; i <= DEFLATE_MAX_BITS; i++)
        tree->counts[i] = 0;
    for (size_t i = 0; i < symbol_count; i++) {
        if (lengths[i] > DEFLATE_MAX_BITS)
            return -E_INVAL;
        tree->counts[lengths[i]]++;
    }
    if (tree->counts[0] == symbol_count)
        return -E_INVAL;

    for (size_t bits = 1; bits <= DEFLATE_MAX_BITS; bits++) {
        remaining = (remaining << 1) - tree->counts[bits];
        if (remaining < 0)
            return -E_INVAL;
    }

    offsets[1] = 0;
    for (size_t bits = 1; bits < DEFLATE_MAX_BITS; bits++)
        offsets[bits + 1] = offsets[bits] + tree->counts[bits];
    for (size_t symbol = 0; symbol < symbol_count; symbol++) {
        uint8_t length = lengths[symbol];
        if (length)
            tree->symbols[offsets[length]++] = (uint16_t)symbol;
    }
    return 0;
}

/* Decode one symbol by walking the canonical code tree bit by bit. */
static int decode_symbol(deflate_stream_t *stream, const deflate_huffman_t *tree, uint16_t *symbol)
{
    unsigned int code  = 0;
    unsigned int first = 0;
    unsigned int index = 0;

    for (size_t length = 1; length <= DEFLATE_MAX_BITS; length++) {
        uint32_t bit;
        if (read_bits(stream, 1, &bit) != 0)
            return -E_INVAL;
        code |= bit;

        unsigned int count = tree->counts[length];
        if (code < first + count) {
            *symbol = tree->symbols[index + code - first];
            return 0;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -E_INVAL;
}

/* Build the fixed literal and distance trees defined by RFC 1951. */
static int build_fixed_trees(deflate_huffman_t *literal_tree, deflate_huffman_t *distance_tree)
{
    uint8_t literal_lengths[288];
    uint8_t distance_lengths[32];

    for (size_t i = 0; i <= 143; i++)
        literal_lengths[i] = 8;
    for (size_t i = 144; i <= 255; i++)
        literal_lengths[i] = 9;
    for (size_t i = 256; i <= 279; i++)
        literal_lengths[i] = 7;
    for (size_t i = 280; i <= 287; i++)
        literal_lengths[i] = 8;
    for (size_t i = 0; i < 32; i++)
        distance_lengths[i] = 5;

    if (build_huffman(literal_tree, literal_lengths, 288) != 0)
        return -E_INVAL;
    return build_huffman(distance_tree, distance_lengths, 32);
}

/* Read the code-length table and build dynamic literal/distance trees. */
static int build_dynamic_trees(deflate_stream_t *stream, deflate_huffman_t *literal_tree,
                               deflate_huffman_t *distance_tree)
{
    static const uint8_t code_order[19]   = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                             11, 4,  12, 3, 13, 2, 14, 1, 15};
    uint8_t              code_lengths[19] = {0};
    uint8_t              lengths[286 + 32];
    deflate_huffman_t    code_tree;
    uint32_t             value;

    if (read_bits(stream, 5, &value) != 0)
        return -E_INVAL;
    size_t literal_count = value + 257;
    if (read_bits(stream, 5, &value) != 0)
        return -E_INVAL;
    size_t distance_count = value + 1;
    if (read_bits(stream, 4, &value) != 0)
        return -E_INVAL;
    size_t code_count = value + 4;
    if (literal_count > 286 || distance_count > 32)
        return -E_INVAL;

    for (size_t i = 0; i < code_count; i++) {
        if (read_bits(stream, 3, &value) != 0)
            return -E_INVAL;
        code_lengths[code_order[i]] = (uint8_t)value;
    }
    if (build_huffman(&code_tree, code_lengths, 19) != 0)
        return -E_INVAL;

    size_t total = literal_count + distance_count;
    size_t index = 0;
    while (index < total) {
        uint16_t symbol;
        if (decode_symbol(stream, &code_tree, &symbol) != 0)
            return -E_INVAL;

        if (symbol <= 15) {
            lengths[index++] = (uint8_t)symbol;
            continue;
        }

        size_t  repeat;
        uint8_t repeated_length;
        if (symbol == 16) {
            if (!index || read_bits(stream, 2, &value) != 0)
                return -E_INVAL;
            repeat          = value + 3;
            repeated_length = lengths[index - 1];
        } else if (symbol == 17) {
            if (read_bits(stream, 3, &value) != 0)
                return -E_INVAL;
            repeat          = value + 3;
            repeated_length = 0;
        } else if (symbol == 18) {
            if (read_bits(stream, 7, &value) != 0)
                return -E_INVAL;
            repeat          = value + 11;
            repeated_length = 0;
        } else
            return -E_INVAL;

        if (repeat > total - index)
            return -E_INVAL;
        while (repeat--)
            lengths[index++] = repeated_length;
    }

    if (!lengths[256])
        return -E_INVAL;
    if (build_huffman(literal_tree, lengths, literal_count) != 0)
        return -E_INVAL;
    return build_huffman(distance_tree, lengths + literal_count, distance_count);
}

/* Decompress one Huffman-coded block, emitting literals and LZ77 matches. */
static int inflate_compressed_block(deflate_stream_t *stream, uint8_t *output,
                                    size_t output_capacity, size_t *output_offset,
                                    const deflate_huffman_t *literal_tree,
                                    const deflate_huffman_t *distance_tree)
{
    uint8_t literals[16];
    size_t  literal_count = 0;

    for (;;) {
        uint16_t symbol;
        uint32_t extra;
        if (decode_symbol(stream, literal_tree, &symbol) != 0)
            return -E_INVAL;

        if (symbol < 256) {
            /* Buffer literals and flush 16 at once -- the reference used this
             * batching for its SSE2 store; the plain memcpy is equivalent. */
            if (*output_offset + literal_count + 1 > output_capacity)
                return -E_INVAL;
            literals[literal_count++] = (uint8_t)symbol;
            if (literal_count == 16) {
                memcpy(output + *output_offset, literals, 16);
                *output_offset += 16;
                literal_count = 0;
            }
            continue;
        }
        if (symbol == 256) {
            for (size_t k = 0; k < literal_count; k++)
                output[(*output_offset)++] = literals[k];
            return 0;
        }
        if (symbol < 257 || symbol > 285)
            return -E_INVAL;

        size_t length_index = symbol - 257;
        size_t length       = length_base[length_index];
        if (length_extra[length_index]) {
            if (read_bits(stream, length_extra[length_index], &extra) != 0)
                return -E_INVAL;
            length += extra;
        }

        if (decode_symbol(stream, distance_tree, &symbol) != 0 || symbol >= 30)
            return -E_INVAL;
        size_t distance = distance_base[symbol];
        if (distance_extra[symbol]) {
            if (read_bits(stream, distance_extra[symbol], &extra) != 0)
                return -E_INVAL;
            distance += extra;
        }

        if (!distance)
            return -E_INVAL;

        /* Flush buffered literals first so output offsets are current for
         * both the distance and the capacity checks below. */
        for (size_t k = 0; k < literal_count; k++)
            output[(*output_offset)++] = literals[k];
        literal_count = 0;

        if (distance > *output_offset || length > output_capacity - *output_offset)
            return -E_INVAL;

        size_t src = *output_offset - distance;
        if (distance >= length) {
            /* Non-overlapping match: bulk copy. */
            memcpy(output + *output_offset, output + src, length);
            *output_offset += length;
        } else {
            /* Overlapping match: byte-at-a-time, LZ77 semantics. */
            for (size_t i = 0; i < length; i++)
                output[(*output_offset)++] = output[src + i];
        }
    }
}

/* Copy an uncompressed (stored) block, verifying its length and inverse. */
static int inflate_stored_block(deflate_stream_t *stream, uint8_t *output, size_t output_capacity,
                                size_t *output_offset)
{
    uint32_t length;
    uint32_t inverted_length;
    align_to_byte(stream);
    if (read_bits(stream, 16, &length) != 0 || read_bits(stream, 16, &inverted_length) != 0)
        return -E_INVAL;
    if ((length ^ 0xffffU) != inverted_length || length > output_capacity - *output_offset)
        return -E_INVAL;

    for (size_t i = 0; i < length; i++) {
        uint32_t byte;
        if (read_bits(stream, 8, &byte) != 0)
            return -E_INVAL;
        output[(*output_offset)++] = (uint8_t)byte;
    }
    return 0;
}

/* Inflate a raw deflate stream block by block until the final marker. */
static int inflate_data(const uint8_t *input, size_t input_size, uint8_t *output,
                        size_t output_capacity, size_t *output_size)
{
    deflate_stream_t stream  = {.data = input, .size = input_size};
    size_t           written = 0;
    uint32_t         final;

    do {
        uint32_t type;
        if (read_bits(&stream, 1, &final) != 0 || read_bits(&stream, 2, &type) != 0)
            return -E_INVAL;

        if (type == 0) {
            if (inflate_stored_block(&stream, output, output_capacity, &written) != 0)
                return -E_INVAL;
        } else if (type == 1 || type == 2) {
            deflate_huffman_t literal_tree;
            deflate_huffman_t distance_tree;
            int               status = type == 1 ? build_fixed_trees(&literal_tree, &distance_tree)
                                                 : build_dynamic_trees(&stream, &literal_tree, &distance_tree);
            if (status != 0)
                return status;
            status = inflate_compressed_block(&stream, output, output_capacity, &written,
                                              &literal_tree, &distance_tree);
            if (status != 0)
                return status;
        } else
            return -E_INVAL;
    } while (!final);

    *output_size = written;
    return 0;
}

/* Compute the standard CRC-32 over data (gzip trailer checksum). */
static uint32_t gzip_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (size_t bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return crc ^ UINT32_MAX;
}

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Advance past a null-terminated header field within the trailer limit. */
static int skip_zero_terminated_field(const uint8_t *input, size_t limit, size_t *offset)
{
    while (*offset < limit && input[*offset])
        (*offset)++;
    if (*offset >= limit)
        return -E_INVAL;
    (*offset)++;
    return 0;
}

/* Gzip decompress. */
int gzip_decompress(const uint8_t *input, size_t input_size, uint8_t **output, size_t *output_size)
{
    if (!input || !output || !output_size || input_size < 18)
        return -E_INVAL;

    *output      = 0;
    *output_size = 0;
    if (input[0] != 0x1f || input[1] != 0x8b || input[2] != 8 || (input[3] & 0xe0)) {
        plogk("gzip: Bad magic/header (0x%02x 0x%02x 0x%02x, flags 0x%02x)\n", input[0], input[1],
              input[2], input[3]);
        return -E_INVAL;
    }

    size_t  offset  = 10;
    size_t  trailer = input_size - 8;
    uint8_t flags   = input[3];
    if (flags & 0x04) {
        if (offset + 2 > trailer) {
            plogk("gzip: Truncated extra field in header (offset %u, trailer %u)\n",
                  (uint32_t)offset, (uint32_t)trailer);
            return -E_INVAL;
        }
        size_t extra_size = (size_t)input[offset] | ((size_t)input[offset + 1] << 8);
        offset += 2;
        if (extra_size > trailer - offset) {
            plogk("gzip: Extra field size %u exceeds header trailer.\n", (uint32_t)extra_size);
            return -E_INVAL;
        }
        offset += extra_size;
    }
    if ((flags & 0x08) && skip_zero_terminated_field(input, trailer, &offset) != 0) {
        plogk("gzip: Unterminated file name field in header (offset %u, trailer %u)\n",
              (uint32_t)offset, (uint32_t)trailer);
        return -E_INVAL;
    }
    if ((flags & 0x10) && skip_zero_terminated_field(input, trailer, &offset) != 0) {
        plogk("gzip: Unterminated comment field in header (offset %u, trailer %u)\n",
              (uint32_t)offset, (uint32_t)trailer);
        return -E_INVAL;
    }
    if (flags & 0x02) {
        if (offset + 2 > trailer) {
            plogk("gzip: Truncated fixed header (offset %u, trailer %u)\n", (uint32_t)offset,
                  (uint32_t)trailer);
            return -E_INVAL;
        }
        offset += 2;
    }
    if (offset >= trailer) {
        plogk("gzip: No deflate data in stream (offset %u, trailer %u)\n", (uint32_t)offset,
              (uint32_t)trailer);
        return -E_INVAL;
    }
    size_t expected_size = load_le32(input + input_size - 4);

    /* The output is a run of physical frames, not heap memory: a compressed
     * initrd decompresses far past the 16 MiB kernel heap.  The run stays
     * resident -- it becomes the initrd the VFS mounts. */
    uint64_t out_phys = pmm_alloc_contiguous((expected_size + 4095) / 4096);
    if (!out_phys) {
        plogk("gzip: No contiguous %u frames for decompressed image.\n",
              (uint32_t)((expected_size + 4095) / 4096));
        return -E_NOMEM;
    }
    uint8_t *result = pmm_virt(out_phys);
    memset(result, 0, expected_size);

    size_t actual_size = 0;
    int    status =
        inflate_data(input + offset, trailer - offset, result, expected_size, &actual_size);
    if (status != 0 || actual_size != expected_size ||
        gzip_crc32(result, actual_size) != load_le32(input + trailer)) {
        plogk("gzip: Corrupt deflate stream (status %d, expected %u, actual %u)\n", status,
              (uint32_t)expected_size, (uint32_t)actual_size);
        return status != 0 ? status : -E_INVAL;
    }

    *output      = result;
    *output_size = actual_size;
    return 0;
}
