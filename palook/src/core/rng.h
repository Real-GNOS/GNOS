// RNG.MKF animations: nested per-frame offset table, YJ1/YJ2 compressed
// delta frames accumulated onto a 320x200 indexed screen.
// Ported from sdlpal's rngplay.c (PAL_RNGReadFrame / PAL_RNGBlitToSurface).

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace pal {

struct RngAnimation {
    bool valid = false;
    int frameCount = 0;
    // Each frame is a full 320x200 indexed snapshot (819200? no: 64000 bytes),
    // colorize with the active palette.
    QVector<QByteArray> frames;
    QString error;
};

// Decode one RNG chunk (all frames, in order).
RngAnimation decodeRngChunk(const QByteArray &chunk);

// Number of frames without decoding (nested table only).
int rngFrameCount(const QByteArray &chunk);

} // namespace pal
