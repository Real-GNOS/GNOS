#include "rle.h"

namespace pal {

namespace {

inline quint16 le16(const uchar *p)
{
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

constexpr int kMaxDim = 4096;
constexpr qint64 kMaxPixels = 4 * 1024 * 1024;

} // namespace

bool spriteFrameTable(const QByteArray &data, QVector<int> *offsets)
{
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    const int size = data.size();
    if (size < 4)
        return false;

    const int count = le16(p);
    if (count <= 0 || count > 10000)
        return false;
    if (2 + count * 2 > size)
        return false;

    QVector<int> offs(count);
    for (int i = 0; i < count; ++i) {
        int off = int(le16(p + 2 + 2 * i)) * 2;
        if (off == 0x18444)
            off = 0x8444; // sdlpal's broken-sprite workaround
        if (off < 0 || off >= size)
            return false;
        offs[i] = off;
    }
    if (offsets)
        *offsets = offs;
    return true;
}

Bitmap8 decodeRleFrame(const uchar *frame, int avail)
{
    Bitmap8 bmp;
    if (avail < 8)
        return bmp;

    const uchar *p = frame;
    const uchar *end = frame + avail;

    // Optional 0x00000002 header.
    if (avail >= 4 && p[0] == 0x02 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x00)
        p += 4;

    if (end - p < 4)
        return bmp;
    const int w = le16(p);
    const int h = le16(p + 2);
    p += 4;
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim ||
        qint64(w) * h > kMaxPixels)
        return bmp;

    const qint64 total = qint64(w) * h;
    QByteArray out(int(total), '\0');
    uchar *dst = reinterpret_cast<uchar *>(out.data());

    qint64 pos = 0;
    while (pos < total) {
        if (p >= end)
            return {}; // truncated stream
        const uchar T = *p++;
        if ((T & 0x80) && T <= 0x80 + w) {
            pos += T - 0x80; // transparent run (may cross rows)
        } else {
            for (int k = 0; k < T; ++k) {
                if (p >= end)
                    return {};
                const uchar v = *p++;
                if (pos < total)
                    dst[pos] = v;
                pos++;
            }
        }
    }

    bmp.w = w;
    bmp.h = h;
    bmp.px = out;
    return bmp;
}

Bitmap8 spriteFrame(const QByteArray &data, int index)
{
    QVector<int> offs;
    if (!spriteFrameTable(data, &offs))
        return {};
    if (index < 0 || index >= offs.size())
        return {};
    const int off = offs[index];
    return decodeRleFrame(reinterpret_cast<const uchar *>(data.constData()) + off,
                          data.size() - off);
}

bool looksLikeSprite(const QByteArray &data)
{
    QVector<int> offs;
    if (!spriteFrameTable(data, &offs))
        return false;
    // Frame offsets should be ordered; frame 0 must sit after the table.
    const int tableEnd = 2 + offs.size() * 2;
    if (offs[0] < tableEnd)
        return false;
    const Bitmap8 bmp = spriteFrame(data, 0);
    return bmp.valid();
}

QImage toImage(const Bitmap8 &bmp, const QVector<QRgb> &palette)
{
    if (!bmp.valid())
        return {};
    QImage img(bmp.w, bmp.h, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    const int n = palette.size();
    for (int y = 0; y < bmp.h; ++y) {
        const uchar *row = reinterpret_cast<const uchar *>(bmp.px.constData()) +
                           y * bmp.w;
        QRgb *out = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < bmp.w; ++x) {
            const uchar idx = row[x];
            if (idx == 0)
                out[x] = qRgba(0, 0, 0, 0);
            else if (idx < n)
                out[x] = palette[idx] | 0xff000000u;
            else
                out[x] = qRgba(0, 0, 0, 0);
        }
    }
    return img;
}

} // namespace pal
