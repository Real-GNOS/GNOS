// pat.mkf palette parsing (sdlpal's palette.c PAL_GetPalette).

#pragma once

#include <QByteArray>
#include <QRgb>
#include <QString>
#include <QVector>

namespace pal {

struct PalSet {
    QVector<QRgb> day;     // 256 entries, 0xffRRGGBB
    QVector<QRgb> night;   // empty when the chunk has no night colors
    bool valid = false;

    const QVector<QRgb> &colors(bool useNight) const
    {
        return (useNight && !night.isEmpty()) ? night : day;
    }
    QString describe() const;
};

// One pat.mkf chunk: 768 bytes (256 * RGB, 6-bit) or 1536 with a night
// palette following the day palette.
PalSet parsePaletteChunk(const QByteArray &data);

// A grayscale fallback palette so previews still work without pat.mkf.
QVector<QRgb> defaultPalette();

} // namespace pal
