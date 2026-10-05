#include "rng.h"

#include "decompress.h"
#include "mkf.h"

namespace pal {

namespace {

constexpr int kW = 320;
constexpr int kH = 200;
constexpr int kScreen = kW * kH;   // 64000
constexpr int kMaxFrameBytes = 65000;

// Apply one delta frame onto the accumulator.
// Returns false on malformed streams (bounds hardened vs. sdlpal).
bool applyRngDelta(QByteArray &screen, const QByteArray &delta)
{
    const uchar *rng = reinterpret_cast<const uchar *>(delta.constData());
    const int length = delta.size();
    int ptr = 0;
    qint64 dst = 0;

    auto put2 = [&](int a, int b) -> bool {
        if (dst + 2 > kScreen)
            return false;
        screen[int(dst)] = char(a);
        screen[int(dst) + 1] = char(b);
        dst += 2;
        return true;
    };

    auto readByte = [&](int *pos) -> int {
        if (*pos >= length)
            return -1;
        return rng[(*pos)++];
    };

    while (ptr < length) {
        int data = readByte(&ptr);
        if (data < 0)
            break;
        switch (data) {
        case 0x00:
        case 0x13:
            return true; // end of frame
        case 0x02:
            dst += 2;
            break;
        case 0x03: {
            const int v = readByte(&ptr);
            if (v < 0)
                return false;
            dst += (v + 1) * 2;
            break;
        }
        case 0x04: {
            if (ptr + 2 > length)
                return false;
            const int w = rng[ptr] | (rng[ptr + 1] << 8);
            ptr += 2;
            dst += (w + 1) * 2;
            break;
        }
        case 0x0a:
        case 0x09:
        case 0x08:
        case 0x07:
        case 0x06: {
            // Cascading cases: 0x0a writes five pairs, ..., 0x06 writes one.
            const int pairs = data - 0x05; // 0x06 -> 1 ... 0x0a -> 5
            for (int i = 0; i < pairs; ++i) {
                const int a = readByte(&ptr);
                const int b = readByte(&ptr);
                if (a < 0 || b < 0)
                    return false;
                if (!put2(a, b))
                    return false;
            }
            break;
        }
        case 0x0b: {
            const int n = readByte(&ptr);
            if (n < 0)
                return false;
            for (int i = 0; i <= n; ++i) {
                const int a = readByte(&ptr);
                const int b = readByte(&ptr);
                if (a < 0 || b < 0)
                    return false;
                if (!put2(a, b))
                    return false;
            }
            break;
        }
        case 0x0c: {
            if (ptr + 2 > length)
                return false;
            const int w = rng[ptr] | (rng[ptr + 1] << 8);
            ptr += 2;
            for (int i = 0; i <= w; ++i) {
                const int a = readByte(&ptr);
                const int b = readByte(&ptr);
                if (a < 0 || b < 0)
                    return false;
                if (!put2(a, b))
                    return false;
            }
            break;
        }
        case 0x0d:
        case 0x0e:
        case 0x0f:
        case 0x10: {
            // Same pixel pair repeated (data - 0x0b) times.
            if (ptr + 2 > length)
                return false;
            const int a = rng[ptr], b = rng[ptr + 1];
            for (int i = 0; i < data - 0x0b; ++i) {
                if (!put2(a, b))
                    return false;
            }
            ptr += 2;
            break;
        }
        case 0x11: {
            const int n = readByte(&ptr);
            if (n < 0)
                return false;
            if (ptr + 2 > length)
                return false;
            const int a = rng[ptr], b = rng[ptr + 1];
            for (int i = 0; i <= n; ++i) {
                if (!put2(a, b))
                    return false;
            }
            ptr += 2;
            break;
        }
        case 0x12: {
            if (ptr + 2 > length)
                return false;
            const int n = (rng[ptr] | (rng[ptr + 1] << 8)) + 1;
            ptr += 2;
            if (ptr + 2 > length)
                return false;
            const int a = rng[ptr], b = rng[ptr + 1];
            for (int i = 0; i < n; ++i) {
                if (!put2(a, b))
                    return false;
            }
            ptr += 2;
            break;
        }
        default:
            break; // unknown opcode: skip (sdlpal has no default either)
        }
        // Skips may legally run past the end of the screen (the game just
        // stops writing); only actual writes are guarded in put2().
    }
    return true;
}

} // namespace

int rngFrameCount(const QByteArray &chunk)
{
    return MkfArchive::nestedCount(chunk);
}

RngAnimation decodeRngChunk(const QByteArray &chunk)
{
    RngAnimation an;
    const int n = MkfArchive::nestedCount(chunk);
    if (n <= 0) {
        an.error = QStringLiteral("RNG chunk has no frame table");
        return an;
    }

    QByteArray screen(kScreen, '\0');
    an.frames.reserve(n);
    for (int i = 0; i < n; ++i) {
        QByteArray frame = MkfArchive::nestedChunk(chunk, i);
        if (frame.isEmpty()) {
            an.error = QStringLiteral("frame %1 unreadable").arg(i);
            return an;
        }
        QByteArray delta;
        const QString method = hasYj1Signature(frame)
                                   ? QStringLiteral("YJ1")
                                   : QStringLiteral("YJ2/raw");
        (void)method;
        // RNG frames are compressed like everything else.
        delta = autoDecompress(frame, kMaxFrameBytes);
        if (delta.isEmpty()) {
            // Some rips ship raw delta streams.
            if (frame.size() <= kMaxFrameBytes)
                delta = frame;
            else {
                an.error = QStringLiteral("frame %1: cannot decompress").arg(i);
                return an;
            }
        }
        if (!applyRngDelta(screen, delta)) {
            an.error = QStringLiteral("frame %1: malformed delta stream").arg(i);
            return an;
        }
        an.frames.append(screen);
    }
    an.frameCount = n;
    an.valid = true;
    return an;
}

} // namespace pal
