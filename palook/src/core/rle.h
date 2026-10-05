// PAL RLE sprite decoding: frame table parsing + per-frame RLE bitmaps.
// Ported from sdlpal's palcommon.c (PAL_SpriteGetFrame / PAL_RLEBlitToSurface).

#pragma once

#include <QByteArray>
#include <QImage>
#include <QVector>

namespace pal {

struct Bitmap8 {
    int w = 0;
    int h = 0;
    QByteArray px;   // row-major, one byte per pixel, 0 = transparent
    bool valid() const { return w > 0 && h > 0 && px.size() == w * h; }
};

// Parse the frame offset table at the head of a sprite.
// Returns false when the data does not look like a sprite.
bool spriteFrameTable(const QByteArray &data, QVector<int> *offsets);

// Decode one RLE frame (pointer must reference a frame from the table).
// Returns an invalid bitmap on malformed data.
Bitmap8 decodeRleFrame(const uchar *frame, int avail);

// Convenience: decode frame index i of a whole sprite blob.
Bitmap8 spriteFrame(const QByteArray &data, int index);

// Quick validation: does this blob parse as a sprite with at least one
// decodable frame?
bool looksLikeSprite(const QByteArray &data);

// Colorize an indexed bitmap with a palette (index 0 = transparent).
QImage toImage(const Bitmap8 &bmp, const QVector<QRgb> &palette);

} // namespace pal
