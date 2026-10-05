#include "palette.h"

namespace pal {

PalSet parsePaletteChunk(const QByteArray &data)
{
    PalSet ps;
    if (data.size() < 768)
        return ps;

    auto read = [&](const uchar *buf, int base) {
        QVector<QRgb> v(256);
        for (int i = 0; i < 256; ++i) {
            const int r = buf[base + i * 3] << 2;
            const int g = buf[base + i * 3 + 1] << 2;
            const int b = buf[base + i * 3 + 2] << 2;
            v[i] = qRgb(r, g, b);
        }
        return v;
    };

    const uchar *buf = reinterpret_cast<const uchar *>(data.constData());
    ps.day = read(buf, 0);
    if (data.size() > 768)
        ps.night = read(buf, 768);
    ps.valid = true;
    return ps;
}

QString PalSet::describe() const
{
    if (!valid)
        return QStringLiteral("invalid palette");
    return QStringLiteral("256 colors%1")
        .arg(night.isEmpty() ? QString() : QStringLiteral(" (+ night palette)"));
}

QVector<QRgb> defaultPalette()
{
    QVector<QRgb> v(256);
    for (int i = 0; i < 256; ++i)
        v[i] = qRgb(i, i, i);
    // Index 0 is transparent in sprites; give it a visible swatch value
    // only for palette display purposes (callers treat idx 0 as transparent
    // when colorizing bitmaps).
    v[0] = qRgb(0, 0, 0);
    return v;
}

} // namespace pal
