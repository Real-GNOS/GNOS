#include "sound.h"

#include <cstring>

namespace pal {

namespace {

inline quint16 le16(const uchar *p)
{
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

inline quint32 le32(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) |
           (quint32(p[3]) << 24);
}

QByteArray buildWav(int rate, int channels, int bits, const QByteArray &pcm)
{
    QByteArray out;
    out.reserve(pcm.size() + 44);
    auto putU32 = [&](quint32 v) {
        char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
        out.append(b, 4);
    };
    auto putU16 = [&](quint16 v) {
        char b[2] = {char(v), char(v >> 8)};
        out.append(b, 2);
    };

    const quint16 align = quint16(channels * bits / 8);
    out.append("RIFF", 4);
    putU32(quint32(36 + pcm.size()));
    out.append("WAVE", 4);
    out.append("fmt ", 4);
    putU32(16);
    putU16(1);                       // PCM
    putU16(quint16(channels));
    putU32(quint32(rate));
    putU32(quint32(rate * align));
    putU16(align);
    putU16(quint16(bits));
    out.append("data", 4);
    putU32(quint32(pcm.size()));
    out.append(pcm);
    return out;
}

// Walk the RIFF chunks of a WAV buffer; returns false on malformed input.
bool walkWav(const QByteArray &data, int *rate, int *channels, int *bits,
             QByteArray *pcm)
{
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    const int size = data.size();
    if (size < 12 || std::memcmp(p, "RIFF", 4) != 0 ||
        std::memcmp(p + 8, "WAVE", 4) != 0)
        return false;

    bool haveFmt = false;
    int pos = 12;
    while (pos + 8 <= size) {
        const quint32 len = le32(p + pos + 4);
        const int body = pos + 8;
        if (quint64(body) + len > quint64(size))
            return false;
        if (std::memcmp(p + pos, "fmt ", 4) == 0 && len >= 16) {
            const quint16 tag = le16(p + body);
            if (tag != 1)
                return false;
            *channels = le16(p + body + 2);
            *rate = int(le32(p + body + 4));
            *bits = le16(p + body + 14);
            haveFmt = true;
        } else if (std::memcmp(p + pos, "data", 4) == 0) {
            *pcm = data.mid(body, int(len));
        }
        pos = body + int(len) + (len & 1);
    }
    return haveFmt && rate && *rate > 0 && *channels >= 1 && *bits >= 8 &&
           (*bits == 8 || *bits == 16);
}

} // namespace

SoundInfo parseWav(const QByteArray &data)
{
    SoundInfo si;
    si.kind = QStringLiteral("WAV");
    int rate = 0, ch = 0, bits = 0;
    QByteArray pcm;
    if (!walkWav(data, &rate, &ch, &bits, &pcm)) {
        si.error = QStringLiteral("malformed or unsupported RIFF/WAVE");
        return si;
    }
    si.rate = rate;
    si.channels = ch;
    si.bits = bits;
    si.frames = pcm.size() / (ch * (bits / 8));
    si.duration = rate > 0 ? double(si.frames) / rate : 0;
    si.wav = buildWav(rate, ch, bits, pcm);
    si.ok = true;
    return si;
}

SoundInfo parseVoc(const QByteArray &data)
{
    SoundInfo si;
    si.kind = QStringLiteral("VOC");

    static const char kSig[] = "Creative Voice File\x1A";
    if (data.size() < 0x16 || std::memcmp(data.constData(), kSig, 0x14) != 0) {
        si.error = QStringLiteral("not a Creative Voice file");
        return si;
    }
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    const int offset = le16(p + 0x14);
    if (offset >= data.size()) {
        si.error = QStringLiteral("bad VOC data offset");
        return si;
    }

    QByteArray pcm;
    int rate = 0;
    int tc = -1;
    int pos = offset;
    while (pos + 4 <= data.size()) {
        const int type = p[pos];
        if (type == 0)
            break;
        const quint32 len = quint32(p[pos + 1]) | (quint32(p[pos + 2]) << 8) |
                            (quint32(p[pos + 3]) << 16);
        if (quint64(pos) + 4 + len > quint64(data.size()))
            break; // truncated; keep what we got
        if (type == 1) {
            if (len < 2)
                break;
            const int blockTc = p[pos + 4];
            const int pack = p[pos + 5];
            if (pack != 0) {
                if (pcm.isEmpty())
                    si.error = QStringLiteral("VOC block packing %1 unsupported")
                                   .arg(pack);
                break;
            }
            if (tc < 0) {
                tc = blockTc;
                const int div = 256 - blockTc;
                rate = div > 0 ? ((1000000 / div) + 99) / 100 * 100 : 11025;
            } else if (blockTc != tc) {
                break; // sample rate change mid-file: keep first segment
            }
            pcm.append(data.mid(pos + 6, int(len) - 2));
        }
        // Other block types are skipped (continuation/data extension...).
        pos += 4 + int(len);
    }

    if (pcm.isEmpty()) {
        if (si.error.isEmpty())
            si.error = QStringLiteral("no type-01 sample block found");
        return si;
    }
    si.rate = rate;
    si.channels = 1;
    si.bits = 8;
    si.frames = pcm.size();
    si.duration = rate > 0 ? double(si.frames) / rate : 0;
    si.wav = buildWav(rate, 1, 8, pcm);
    si.ok = true;
    return si;
}

SoundInfo parseSound(const QByteArray &data)
{
    if (data.size() >= 12 && std::memcmp(data.constData(), "RIFF", 4) == 0)
        return parseWav(data);
    if (data.size() >= 0x16 && data.startsWith("Creative Voice File"))
        return parseVoc(data);
    SoundInfo si;
    si.error = QStringLiteral("unrecognized audio container");
    return si;
}

Envelope waveform(const SoundInfo &si, int columns)
{
    Envelope env;
    if (!si.ok || columns <= 0 || si.wav.isEmpty())
        return env;

    int rate = 0, ch = 0, bits = 0;
    QByteArray pcm;
    if (!walkWav(si.wav, &rate, &ch, &bits, &pcm))
        return env;

    const int step = ch * (bits / 8);
    if (step <= 0)
        return env;
    const qint64 total = pcm.size() / step;
    if (total <= 0)
        return env;

    columns = int(qMin<qint64>(columns, total));
    env.minV.resize(columns);
    env.maxV.resize(columns);
    const uchar *p = reinterpret_cast<const uchar *>(pcm.constData());

    auto sampleAt = [&](qint64 i) -> qint16 {
        const uchar *s = p + i * step; // channel 0
        if (bits == 8)
            return qint16((int(s[0]) - 128) << 8);
        return qint16(le16(s));
    };

    for (int c = 0; c < columns; ++c) {
        const qint64 a = total * c / columns;
        const qint64 b = total * (c + 1) / columns;
        qint16 mn = 32767, mx = -32768;
        for (qint64 i = a; i < b; ++i) {
            const qint16 v = sampleAt(i);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        if (a >= b) {
            const qint16 v = sampleAt(a < total ? a : total - 1);
            mn = mx = v;
        }
        env.minV[c] = mn;
        env.maxV[c] = mx;
    }
    env.valid = true;
    return env;
}

MidiInfo parseMidi(const QByteArray &data)
{
    MidiInfo mi;
    if (data.size() < 14 || std::memcmp(data.constData(), "MThd", 4) != 0) {
        mi.error = QStringLiteral("not a standard MIDI file");
        return mi;
    }
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    const quint32 hdrLen = le32(p + 4);
    if (hdrLen < 6) {
        mi.error = QStringLiteral("bad MIDI header length");
        return mi;
    }
    mi.format = le16(p + 8);
    mi.tracks = le16(p + 10);
    const quint16 div = le16(p + 12);
    mi.division = (div & 0x8000) ? -int(div & 0x7fff) : int(div);
    mi.ok = true;
    return mi;
}

} // namespace pal
