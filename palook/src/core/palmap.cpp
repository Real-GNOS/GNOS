#include "palmap.h"

#include "rle.h"

#include <QMap>

namespace pal {

namespace {

inline quint32 le32(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) |
           (quint32(p[3]) << 24);
}

// Frame index of a tile on the given layer (map.c PAL_MapGetTileBitmap).
// Returns -1 when the cell is empty.
int tileFrame(quint32 d, int layer, bool inRange)
{
    if (layer == 0) {
        const int idx = int((d & 0xFFu) | ((d >> 4) & 0x100u));
        // An out-of-range cell is rendered as "empty" by the caller, which
        // skips it instead of duplicating tile (0,0,0) like the game does.
        return inRange ? idx : -1;
    }
    d >>= 16;
    if (d == 0)
        return -1;
    return int((d & 0xFFu) | ((d >> 4) & 0x100u)) - 1;
}

void drawTile(QImage &canvas, const QImage &tile, int x, int y)
{
    // Clip against the canvas.
    const QRect bounds(0, 0, canvas.width(), canvas.height());
    QRect src(0, 0, tile.width(), tile.height());
    QRect dst(x, y, tile.width(), tile.height());
    const QRect clipped = dst & bounds;
    if (clipped.isEmpty())
        return;
    src.moveTopLeft(src.topLeft() + (clipped.topLeft() - dst.topLeft()));
    src.setSize(clipped.size());

    const QImage part = tile.copy(src);
    for (int row = 0; row < part.height(); ++row) {
        const QRgb *s = reinterpret_cast<const QRgb *>(part.constScanLine(row));
        QRgb *d = reinterpret_cast<QRgb *>(canvas.scanLine(clipped.top() + row)) +
                  clipped.left();
        for (int col = 0; col < part.width(); ++col) {
            if (s[col] >> 24) // alpha > 0
                d[col] = s[col];
        }
    }
}

QRect nonTransparentBBox(const QImage &img)
{
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (row[x] >> 24) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
        return {};
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

} // namespace

MapData parseMapChunk(const QByteArray &data)
{
    MapData m;
    if (data.size() != int(MapData::kRows * MapData::kCols * MapData::kHalf * 4))
        return m;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    for (int y = 0; y < MapData::kRows; ++y)
        for (int x = 0; x < MapData::kCols; ++x)
            for (int h = 0; h < MapData::kHalf; ++h) {
                m.tiles[y][x][h] = le32(p);
                p += 4;
            }
    m.valid = true;
    return m;
}

QImage renderMap(const MapData &map, const QByteArray &gop,
                 const QVector<QRgb> &palette, int layer)
{
    if (!map.valid)
        return {};

    // Canvas: full map pixel area plus room for the off-by-one edge cells.
    const int kPad = 64;
    const int canvasW = MapData::kCols * 32 + kPad * 2;
    const int canvasH = MapData::kRows * 16 + kPad * 2;
    QImage canvas(canvasW, canvasH, QImage::Format_ARGB32);
    canvas.fill(Qt::transparent);

    // Map of decoded tile frames (tiles repeat a lot).
    QMap<int, QImage> cache;
    auto frameImage = [&](int idx) -> QImage {
        if (idx < 0)
            return {};
        auto it = cache.find(idx);
        if (it != cache.end())
            return it.value();
        const Bitmap8 bmp = spriteFrame(gop, idx);
        QImage img = bmp.valid() ? toImage(bmp, palette) : QImage();
        cache.insert(idx, img);
        return img;
    };

    // Same traversal as PAL_MapBlitToSurface with srcRect (0, 0, 2048, 2048).
    const int sx = -1, dx = MapData::kCols * 32 / 32 + 2;
    const int sy = -1, dy = MapData::kRows * 16 / 16 + 2;

    for (int pass = 0; pass < 2; ++pass) {
        const int l = (layer == 2) ? pass : layer;
        if (layer != 2 && pass == 1)
            break;

        int yPos = sy * 16 - 8 + kPad;
        for (int y = sy; y < dy; ++y) {
            for (int h = 0; h < 2; ++h, yPos += 8) {
                int xPos = sx * 32 + h * 16 - 16 + kPad;
                for (int x = sx; x < dx; ++x, xPos += 32) {
                    const bool inRange = x >= 0 && x < MapData::kCols &&
                                         y >= 0 && y < MapData::kRows;
                    int idx = -1;
                    if (inRange)
                        idx = tileFrame(map.tiles[y][x][h], l, true);
                    if (idx < 0) {
                        if (l == 1)
                            continue;         // empty top layer cell
                        if (!inRange)
                            continue;         // viewer: no border filler
                        idx = 0;              // game falls back to tile (0,0,0)
                    }
                    const QImage tile = frameImage(idx);
                    if (tile.isNull())
                        continue;
                    drawTile(canvas, tile, xPos, yPos);
                }
            }
        }
    }

    const QRect bbox = nonTransparentBBox(canvas);
    if (bbox.isEmpty())
        return {};
    return canvas.copy(bbox);
}

QString describeTile(quint32 d)
{
    const int bottom = int((d & 0xFFu) | ((d >> 4) & 0x100u));
    const quint32 hi = d >> 16;
    const int top = hi == 0 ? -1 : int((hi & 0xFFu) | ((hi >> 4) & 0x100u)) - 1;
    return QStringLiteral("bottom #%1, top #%2%3")
        .arg(bottom)
        .arg(top)
        .arg((d & 0x2000u) ? QStringLiteral(", blocked") : QString());
}

} // namespace pal
