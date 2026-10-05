// MAP.MKF tile maps + map rendering (sdlpal map.c).

#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>

namespace pal {

struct MapData {
    static constexpr int kRows = 128; // y
    static constexpr int kCols = 64;  // x
    static constexpr int kHalf = 2;   // h (two half rows per map row)

    quint32 tiles[kRows][kCols][kHalf] = {};
    bool valid = false;
};

// Parse a decompressed map chunk (128 * 64 * 2 * 4 = 65536 bytes).
MapData parseMapChunk(const QByteArray &data);

// Render the whole map.
//   layer: 0 = bottom, 1 = top, 2 = both
//   gop: raw GOP.MKF chunk for this map index (sprite of tiles)
//   palette: colors used to colorize the tiles
// The result is cropped to the non-transparent bounding box.
QImage renderMap(const MapData &map, const QByteArray &gop,
                 const QVector<QRgb> &palette, int layer);

// Human readable summary of one tile dword (for the info panel).
QString describeTile(quint32 d);

} // namespace pal
