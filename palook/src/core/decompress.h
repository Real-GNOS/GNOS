// YJ1 / YJ2 decompression, ported from sdlpal's yj1.c (GPLv3).
//
// YJ1 ("YJ_1", DOS version) uses a Huffman tree + LZSS blocks.
// YJ2 (Windows version) is an adaptive-Huffman bitstream whose first DWORD
// is the uncompressed length.
//
// The ports are hardened for a viewer: every read/write is bounds checked,
// so corrupt or misdetected lumps fail with an error instead of crashing.

#pragma once

#include <QByteArray>
#include <QString>

namespace pal {

// True when data starts with the YJ_1 signature.
bool hasYj1Signature(const QByteArray &data);

// Decompress; returns bytes written (>= 0) or -1 on error.
// The output is bounded by maxOut.
int yj1Decompress(const QByteArray &src, QByteArray &dst, int maxOut);
int yj2Decompress(const QByteArray &src, QByteArray &dst, int maxOut);

// Try both decompressors (signature/heuristic based) and return the
// decompressed data on success.  On failure returns an empty array and
// sets *method (if given) to a short description of what was attempted.
QByteArray autoDecompress(const QByteArray &src, int maxOut,
                          QString *method = nullptr);

} // namespace pal
