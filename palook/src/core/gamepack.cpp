#include "gamepack.h"

#include "decompress.h"
#include "palmap.h"
#include "rle.h"
#include "rng.h"
#include "sound.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace pal {

namespace {

constexpr int kMaxSpriteOut = 4 * 1024 * 1024;
constexpr int kFbpSize = 320 * 200;
constexpr int kMapSize = 128 * 64 * 2 * 4;
constexpr int kMaxRngFrames = 400;
constexpr int kMaxSpriteFrames = 4096;

FileRole roleFromName(const QString &l)
{
    if (l == QLatin1String("pat.mkf")) return FileRole::Pat;
    if (l == QLatin1String("map.mkf")) return FileRole::Map;
    if (l == QLatin1String("gop.mkf")) return FileRole::Gop;
    if (l == QLatin1String("fbp.mkf")) return FileRole::Fbp;
    if (l == QLatin1String("sounds.mkf")) return FileRole::Sounds;
    if (l == QLatin1String("voc.mkf")) return FileRole::Voc;
    if (l == QLatin1String("midi.mkf")) return FileRole::Midi;
    if (l == QLatin1String("mus.mkf")) return FileRole::Mus;
    if (l == QLatin1String("rng.mkf")) return FileRole::Rng;
    if (l == QLatin1String("sss.mkf")) return FileRole::Sss;
    if (l == QLatin1String("data.mkf")) return FileRole::Data;
    if (l == QLatin1String("word.dat")) return FileRole::Words;
    if (l == QLatin1String("m.msg")) return FileRole::Msg;
    if (l == QLatin1String("desc.dat")) return FileRole::Desc;
    if (l.endsWith(QLatin1String(".mkf"))) {
        // Sprites that live in their own archives.
        static const char *spriteArchives[] = {
            "mgo.mkf", "ball.mkf", "f.mkf", "fire.mkf", "rgm.mkf", "abc.mkf",
        };
        for (const char *s : spriteArchives)
            if (l == QLatin1String(s))
                return FileRole::SpriteMkf;
        return FileRole::Unknown;
    }
    if (l.endsWith(QLatin1String(".dat"))) return FileRole::Unknown;
    if (l.endsWith(QLatin1String(".msg"))) return FileRole::Msg;
    return FileRole::Unknown;
}

QString sniff(const QByteArray &b)
{
    if (b.size() >= 12 && b.startsWith("RIFF") &&
        b.mid(8, 4) == QByteArrayLiteral("WAVE"))
        return QStringLiteral("WAV");
    if (b.size() >= 20 && b.startsWith("Creative Voice File"))
        return QStringLiteral("VOC");
    if (b.size() >= 4 && b.startsWith("MThd"))
        return QStringLiteral("MIDI");
    if (hasYj1Signature(b))
        return QStringLiteral("YJ1 compressed");
    return {};
}

// Fill a table result from fixed-size WORD records.
bool fillTable(const QByteArray &data, int recSize, const QStringList &fields,
               bool opHex, LumpResult &r, const QString &label)
{
    if (recSize <= 0 || data.isEmpty() || data.size() % recSize != 0 ||
        fields.isEmpty())
        return false;
    const int n = data.size() / recSize;
    r.view = LumpResult::View::Table;
    r.headers = fields;
    r.rows.reserve(n);
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    for (int i = 0; i < n; ++i) {
        QStringList row;
        row.reserve(fields.size() + 1);
        row << QString::number(i);
        const uchar *rec = p + i * recSize;
        for (int f = 0; f < recSize / 2 && f < fields.size(); ++f) {
            const quint16 v = quint16(rec[2 * f]) | (quint16(rec[2 * f + 1]) << 8);
            if (opHex && f == 0)
                row << QStringLiteral("0x%1").arg(v, 4, 16, QLatin1Char('0'));
            else
                row << QString::number(v);
        }
        r.rows.append(row);
    }
    if (r.headers.isEmpty() || r.headers.first() != QLatin1String("#"))
        r.headers.prepend(QStringLiteral("#"));
    r.detail = QStringLiteral("%1 — %2 records × %3 bytes")
                   .arg(label)
                   .arg(n)
                   .arg(recSize);
    return true;
}

const QStringList &enemyFields()
{
    static const QStringList f = {
        QStringLiteral("#"), QStringLiteral("IdleFrames"),
        QStringLiteral("MagicFrames"), QStringLiteral("AttackFrames"),
        QStringLiteral("IdleAnimSpeed"), QStringLiteral("ActWaitFrames"),
        QStringLiteral("YPosOffset"), QStringLiteral("AttackSound"),
        QStringLiteral("ActionSound"), QStringLiteral("MagicSound"),
        QStringLiteral("DeathSound"), QStringLiteral("CallSound"),
        QStringLiteral("Health"), QStringLiteral("Exp"),
        QStringLiteral("Cash"), QStringLiteral("Level"),
        QStringLiteral("Magic"), QStringLiteral("MagicRate"),
        QStringLiteral("AttackEquivItem"), QStringLiteral("AttackEquivItemRate"),
        QStringLiteral("StealItem"), QStringLiteral("StealItemCount"),
        QStringLiteral("AttackStrength"), QStringLiteral("MagicStrength"),
        QStringLiteral("Defense"), QStringLiteral("Dexterity"),
        QStringLiteral("FleeRate"), QStringLiteral("PoisonResist"),
        QStringLiteral("Elem0"), QStringLiteral("Elem1"),
        QStringLiteral("Elem2"), QStringLiteral("Elem3"),
        QStringLiteral("Elem4"), QStringLiteral("PhysicalResist"),
        QStringLiteral("DualMove"), QStringLiteral("CollectValue"),
    };
    return f;
}

QString sssChunkLabel(int i)
{
    switch (i) {
    case 0: return QStringLiteral("Event objects");
    case 1: return QStringLiteral("Scenes");
    case 2: return QStringLiteral("Object definitions");
    case 3: return QStringLiteral("Message offsets");
    case 4: return QStringLiteral("Scripts");
    default: return QStringLiteral("Data");
    }
}

QString dataChunkLabel(int i)
{
    switch (i) {
    case 0: return QStringLiteral("Store table");
    case 1: return QStringLiteral("Enemy table");
    case 2: return QStringLiteral("Enemy team table");
    case 3: return QStringLiteral("Player roles");
    case 4: return QStringLiteral("Magic table");
    case 5: return QStringLiteral("Battlefield table");
    case 6: return QStringLiteral("Level-up magic");
    case 9: return QStringLiteral("UI sprite");
    case 10: return QStringLiteral("Battle effect sprite");
    default: return QStringLiteral("Data");
    }
}

} // namespace

QString fileRoleName(FileRole r)
{
    switch (r) {
    case FileRole::Pat: return QStringLiteral("Palette");
    case FileRole::Map: return QStringLiteral("Maps");
    case FileRole::Gop: return QStringLiteral("Map tiles");
    case FileRole::Fbp: return QStringLiteral("Backgrounds");
    case FileRole::Sounds: return QStringLiteral("Sounds (WAV)");
    case FileRole::Voc: return QStringLiteral("Sounds (VOC)");
    case FileRole::Midi: return QStringLiteral("Music (MIDI)");
    case FileRole::Mus: return QStringLiteral("Music (RIX)");
    case FileRole::Rng: return QStringLiteral("Animations");
    case FileRole::Sss: return QStringLiteral("Scripts / scenes");
    case FileRole::Data: return QStringLiteral("Game data");
    case FileRole::Words: return QStringLiteral("Word list");
    case FileRole::Msg: return QStringLiteral("Messages");
    case FileRole::Desc: return QStringLiteral("Descriptions");
    case FileRole::SpriteMkf: return QStringLiteral("Sprites");
    case FileRole::Unknown: break;
    }
    return QStringLiteral("Data");
}

// ---------------------------------------------------------------------------

bool GamePack::openPaths(const QStringList &paths)
{
    close();
    for (const QString &p : paths) {
        QFileInfo fi(p);
        if (fi.isDir())
            openDir(fi.absoluteFilePath());
        else if (fi.exists())
            openFile(fi.absoluteFilePath());
        else
            m_error = QStringLiteral("not found: %1").arg(p);
    }
    if (m_files.isEmpty()) {
        if (m_error.isEmpty())
            m_error = QStringLiteral("no readable files");
        return false;
    }
    detectVersion();
    loadPalette();
    return true;
}

void GamePack::close()
{
    m_files.clear();
    m_error.clear();
    m_win95 = false;
    m_versionNote.clear();
    m_palette = PalSet();
    m_palChunk = 0;
}

bool GamePack::openFile(const QString &path)
{
    PackFile f;
    f.path = path;
    f.name = QFileInfo(path).fileName().toLower();
    f.role = roleFromName(f.name);
    f.size = QFileInfo(path).size();

    if (path.endsWith(QLatin1String(".mkf"), Qt::CaseInsensitive)) {
        f.isMkf = true;
        if (!f.mkf.open(path)) {
            f.error = f.mkf.errorString();
            return false;
        }
    } else {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        f.raw = file.readAll();
    }
    m_files.append(f);
    return true;
}

void GamePack::openDir(const QString &dir)
{
    QDir d(dir);
    const QStringList entries = d.entryList(QDir::Files | QDir::Readable,
                                            QDir::Name);

    // Well-known PAL resources first, in a stable order.
    static const char *known[] = {
        "map.mkf", "gop.mkf", "pat.mkf", "fbp.mkf", "mgo.mkf", "ball.mkf",
        "f.mkf",   "fire.mkf", "rgm.mkf", "abc.mkf", "rng.mkf", "data.mkf",
        "sss.mkf", "sounds.mkf", "voc.mkf", "midi.mkf", "mus.mkf",
        "word.dat", "m.msg", "desc.dat",
    };
    QStringList lower = entries;
    for (QString &s : lower)
        s = s.toLower();

    QStringList ordered;
    for (const char *k : known) {
        const int i = lower.indexOf(QLatin1String(k));
        if (i >= 0)
            ordered << entries[i];
    }
    // Then every other MKF.
    for (int i = 0; i < entries.size(); ++i) {
        if (ordered.contains(entries[i]))
            continue;
        if (entries[i].endsWith(QLatin1String(".mkf"), Qt::CaseInsensitive))
            ordered << entries[i];
    }

    for (const QString &e : ordered)
        openFile(d.filePath(e));
}

void GamePack::detectVersion()
{
    int scanned = 0, nonYj1 = 0;
    static const char *probe[] = {
        "map.mkf", "mgo.mkf", "f.mkf", "fbp.mkf", "fire.mkf", "abc.mkf",
    };
    for (const PackFile &f : m_files) {
        bool matched = false;
        for (const char *p : probe) {
            if (f.name == QLatin1String(p)) {
                matched = true;
                break;
            }
        }
        if (!matched || !f.isMkf)
            continue;
        for (int i = 0; i < f.mkf.count(); ++i) {
            if (f.mkf.chunkSize(i) == 0)
                continue;
            ++scanned;
            if (!hasYj1Signature(f.mkf.peek(i, 4)))
                ++nonYj1;
            break; // only the first non-empty subfile, like sdlpal
        }
    }

    if (scanned > 0 && nonYj1 == scanned) {
        m_win95 = true;
        m_versionNote = QStringLiteral("Windows 95 style (YJ2 / raw)");
    } else if (scanned > 0) {
        m_win95 = false;
        m_versionNote = QStringLiteral("DOS style (YJ1 compressed)");
    } else {
        // Fall back to the OBJECT record size (DOS = 12 bytes, WIN95 = 14).
        m_win95 = false;
        m_versionNote = QStringLiteral("undetected, assuming DOS layout");
        for (const PackFile &f : m_files) {
            if (f.role != FileRole::Sss || !f.isMkf || f.mkf.count() <= 2)
                continue;
            const quint32 sz = f.mkf.chunkSize(2);
            if (sz > 0 && sz % 14 == 0 && sz % 12 != 0) {
                m_win95 = true;
                m_versionNote = QStringLiteral("WIN95 layout (object record size)");
            } else if (sz > 0 && sz % 12 == 0 && sz % 14 == 0) {
                m_versionNote = QStringLiteral("ambiguous object size, assuming DOS");
            } else if (sz > 0 && sz % 12 == 0) {
                m_versionNote = QStringLiteral("DOS layout (object record size)");
            }
            break;
        }
    }
}

const PackFile *GamePack::findRole(FileRole r) const
{
    for (const PackFile &f : m_files)
        if (f.role == r)
            return &f;
    return nullptr;
}

int GamePack::paletteChunkCount() const
{
    const PackFile *pat = findRole(FileRole::Pat);
    return (pat && pat->isMkf) ? pat->mkf.count() : 0;
}

bool GamePack::setPaletteChunk(int idx)
{
    const PackFile *pat = findRole(FileRole::Pat);
    if (!pat || !pat->isMkf || idx < 0 || idx >= pat->mkf.count())
        return false;
    PalSet ps = parsePaletteChunk(pat->mkf.chunk(idx));
    if (!ps.valid)
        return false;
    m_palette = ps;
    m_palChunk = idx;
    return true;
}

void GamePack::loadPalette()
{
    if (!setPaletteChunk(0) && m_files.isEmpty())
        m_palette = PalSet();
    // Detect the text codepage once from word.dat when present.
    if (const PackFile *w = findRole(FileRole::Words)) {
        if (!w->raw.isEmpty())
            m_fallbackCp = detectCodepage(w->raw).cp;
    }
}

QVector<QRgb> GamePack::activePalette() const
{
    if (m_palette.valid)
        return m_palette.colors(m_night);
    return defaultPalette();
}

QByteArray GamePack::messageOffsets() const
{
    const PackFile *sss = findRole(FileRole::Sss);
    if (!sss || !sss->isMkf || sss->mkf.count() <= 3)
        return {};
    return sss->mkf.chunk(3);
}

// ---------------------------------------------------------------------------
// Decoding

bool GamePack::decodeSprite(const QByteArray &raw, bool allowDecompress,
                            LumpResult &r) const
{
    QByteArray data;
    QString method;

    if (allowDecompress && hasYj1Signature(raw)) {
        QByteArray d;
        if (yj1Decompress(raw, d, kMaxSpriteOut) >= 0 && looksLikeSprite(d)) {
            data = d;
            method = QStringLiteral("YJ1");
        }
    }
    if (data.isEmpty() && looksLikeSprite(raw)) {
        data = raw;
        method = QStringLiteral("raw");
    }
    if (data.isEmpty() && allowDecompress && !hasYj1Signature(raw)) {
        QString m;
        QByteArray d = autoDecompress(raw, kMaxSpriteOut, &m);
        if (!d.isEmpty() && looksLikeSprite(d)) {
            data = d;
            method = m;
        }
    }
    if (data.isEmpty())
        return false;

    QVector<int> offs;
    if (!spriteFrameTable(data, &offs))
        return false;

    const int count = qMin(offs.size(), kMaxSpriteFrames);
    const QVector<QRgb> pal = activePalette();
    r.frames.reserve(count);
    int w = 0, h = 0;
    for (int i = 0; i < count; ++i) {
        const Bitmap8 bmp = spriteFrame(data, i);
        if (bmp.valid()) {
            w = bmp.w;
            h = bmp.h;
            r.frames.append(toImage(bmp, pal));
        } else {
            r.frames.append(QImage());
        }
    }
    if (r.frames.isEmpty())
        return false;

    r.view = LumpResult::View::Image;
    r.typeLabel = QStringLiteral("Sprite (RLE)");
    r.detail = QStringLiteral("%1 frames, %2, frame 0 is %3×%4%5")
                   .arg(offs.size())
                   .arg(method)
                   .arg(w)
                   .arg(h)
                   .arg(offs.size() > count
                            ? QStringLiteral(" (showing first %1)").arg(count)
                            : QString());
    return true;
}

bool GamePack::decodeTable(const QByteArray &data, int recSize,
                           const QStringList &fields, const QString &label,
                           bool opHex, LumpResult &r) const
{
    if (recSize <= 0 || data.isEmpty() || data.size() % recSize != 0)
        return false;
    return fillTable(data, recSize, fields, opHex, r, label);
}

LumpResult GamePack::decode(int fi, int li) const
{
    LumpResult r;
    if (fi < 0 || fi >= m_files.size()) {
        r.error = QStringLiteral("bad file index");
        return r;
    }
    const PackFile &f = m_files[fi];
    r.raw = f.isMkf ? f.mkf.chunk(li) : f.raw;
    r.typeLabel = typeHint(fi, li);
    if (f.isMkf && r.raw.isEmpty()) {
        // Empty lumps are normal (archives pad their tables).
        r.typeLabel = QStringLiteral("Empty");
        r.detail = QStringLiteral("empty lump");
        return r;
    }

    auto soundFrom = [&](const SoundInfo &si) {
        if (!si.ok) {
            r.error = si.error;
            return;
        }
        r.view = LumpResult::View::Sound;
        r.snd = si;
        r.typeLabel = QStringLiteral("Sound (%1)").arg(si.kind);
        r.sndKind = si.kind;
        r.rate = si.rate;
        r.channels = si.channels;
        r.bits = si.bits;
        r.duration = si.duration;
        r.wav = si.wav;
        r.detail = QStringLiteral("%1 Hz, %2 bit, %3 channel(s), %4")
                       .arg(si.rate)
                       .arg(si.bits)
                       .arg(si.channels)
                       .arg(QString::number(si.duration, 'f', 2) +
                            QStringLiteral(" s"));
    };

    switch (f.role) {
    case FileRole::Pat: {
        PalSet ps = parsePaletteChunk(r.raw);
        if (!ps.valid) {
            r.error = QStringLiteral("not a 768-byte palette chunk");
            break;
        }
        auto swatch = [](const QVector<QRgb> &c) {
            constexpr int kBlock = 24;
            QImage img(16 * kBlock, 16 * kBlock, QImage::Format_ARGB32);
            for (int i = 0; i < 256; ++i) {
                const QColor col(c[i]);
                for (int y = 0; y < kBlock; ++y)
                    for (int x = 0; x < kBlock; ++x)
                        img.setPixel((i % 16) * kBlock + x,
                                     (i / 16) * kBlock + y, col.rgb());
            }
            return img;
        };
        r.view = LumpResult::View::Image;
        r.typeLabel = QStringLiteral("Palette");
        r.frames.append(swatch(ps.day));
        r.frameLabels << QStringLiteral("Day");
        if (!ps.night.isEmpty()) {
            r.frames.append(swatch(ps.night));
            r.frameLabels << QStringLiteral("Night");
        }
        r.detail = ps.describe();
        break;
    }

    case FileRole::Map: {
        QByteArray d = r.raw.size() == kMapSize
                           ? r.raw
                           : autoDecompress(r.raw, kMapSize);
        const MapData m = parseMapChunk(d);
        if (!m.valid) {
            r.error = QStringLiteral("map chunk does not decompress to "
                                     "128×64×2 tiles");
            break;
        }
        const PackFile *gop = findRole(FileRole::Gop);
        QByteArray gopData;
        if (gop && gop->isMkf && li < gop->mkf.count())
            gopData = gop->mkf.chunk(li);
        QVector<int> offs;
        if (gopData.isEmpty() || !spriteFrameTable(gopData, &offs)) {
            r.error = QStringLiteral("gop.mkf chunk %1 is missing or not a "
                                     "tile sprite").arg(li);
            break;
        }
        const QVector<QRgb> pal = activePalette();
        r.view = LumpResult::View::Map;
        r.isMap = true;
        r.typeLabel = QStringLiteral("Map");
        r.frames.append(renderMap(m, gopData, pal, 0));
        r.frames.append(renderMap(m, gopData, pal, 1));
        r.frames.append(renderMap(m, gopData, pal, 2));
        r.frameLabels << QStringLiteral("Bottom layer")
                      << QStringLiteral("Top layer") << QStringLiteral("Both");
        r.detail = QStringLiteral("64×128 cells, %1 tile frames from gop.mkf#%2")
                       .arg(offs.size())
                       .arg(li);
        break;
    }

    case FileRole::Gop:
        if (!decodeSprite(r.raw, false, r))
            r.error = QStringLiteral("not an RLE tile sprite");
        break;

    case FileRole::Fbp: {
        QByteArray d = r.raw.size() == kFbpSize
                           ? r.raw
                           : autoDecompress(r.raw, kMaxSpriteOut);
        if (d.size() < kFbpSize) {
            r.error = QStringLiteral("background does not decode to 320×200");
            break;
        }
        Bitmap8 bmp;
        bmp.w = 320;
        bmp.h = 200;
        bmp.px = d.left(kFbpSize);
        r.view = LumpResult::View::Image;
        r.typeLabel = QStringLiteral("Background");
        r.frames.append(toImage(bmp, activePalette()));
        r.detail = QStringLiteral("320×200, %1")
                       .arg(r.raw.size() == kFbpSize
                                ? QStringLiteral("raw")
                                : QStringLiteral("decompressed"));
        break;
    }

    case FileRole::SpriteMkf:
        if (!decodeSprite(r.raw, true, r)) {
            const QString sn = sniff(r.raw);
            if (sn == QLatin1String("WAV")) {
                soundFrom(parseWav(r.raw));
            } else {
                r.error = QStringLiteral("not a decodable RLE sprite");
            }
        }
        break;

    case FileRole::Sounds:
    case FileRole::Voc:
        soundFrom(parseSound(r.raw));
        break;

    case FileRole::Midi: {
        const MidiInfo mi = parseMidi(r.raw);
        if (!mi.ok) {
            r.error = mi.error;
            break;
        }
        r.view = LumpResult::View::Table;
        r.typeLabel = QStringLiteral("MIDI file");
        r.headers = {QStringLiteral("#"), QStringLiteral("Property"),
                     QStringLiteral("Value")};
        r.rows = {
            {QStringLiteral("0"), QStringLiteral("Format"),
             QString::number(mi.format)},
            {QStringLiteral("1"), QStringLiteral("Tracks"),
             QString::number(mi.tracks)},
            {QStringLiteral("2"),
             mi.division < 0 ? QStringLiteral("SMPTE frames/sec")
                             : QStringLiteral("Ticks/quarter"),
             QString::number(qAbs(mi.division))},
        };
        r.detail = QStringLiteral("MThd, format %1, %2 track(s)")
                       .arg(mi.format)
                       .arg(mi.tracks);
        break;
    }

    case FileRole::Mus:
        r.error.clear();
        r.detail = QStringLiteral("RIX/AdLib music data — use Export to save "
                                  "the raw lump");
        r.typeLabel = QStringLiteral("Music (RIX)");
        break;

    case FileRole::Rng: {
        const RngAnimation an = decodeRngChunk(r.raw);
        if (!an.valid) {
            r.error = an.error;
            break;
        }
        const QVector<QRgb> pal = activePalette();
        const int n = qMin(an.frameCount, kMaxRngFrames);
        r.view = LumpResult::View::Image;
        r.typeLabel = QStringLiteral("Animation (RNG)");
        r.frames.reserve(n);
        for (int i = 0; i < n; ++i) {
            Bitmap8 bmp;
            bmp.w = 320;
            bmp.h = 200;
            bmp.px = an.frames[i];
            r.frames.append(toImage(bmp, pal));
        }
        r.detail = QStringLiteral("%1 frames, 320×200%2")
                       .arg(an.frameCount)
                       .arg(an.frameCount > n
                                ? QStringLiteral(" (showing first %1)").arg(n)
                                : QString());
        break;
    }

    case FileRole::Sss: {
        bool done = false;
        switch (li) {
        case 0: {
            static const QStringList fields = {
                QStringLiteral("#"), QStringLiteral("VanishTime"),
                QStringLiteral("X"), QStringLiteral("Y"),
                QStringLiteral("Layer"), QStringLiteral("TriggerScript"),
                QStringLiteral("AutoScript"), QStringLiteral("State"),
                QStringLiteral("TriggerMode"), QStringLiteral("SpriteNum"),
                QStringLiteral("SpriteFrames"), QStringLiteral("Direction"),
                QStringLiteral("CurrentFrame"), QStringLiteral("IdleFrame"),
                QStringLiteral("SpritePtrOffset"), QStringLiteral("FramesAuto"),
                QStringLiteral("IdleAuto"),
            };
            done = decodeTable(r.raw, 32, fields, sssChunkLabel(li), false, r);
            break;
        }
        case 1: {
            static const QStringList fields = {
                QStringLiteral("#"), QStringLiteral("MapNum"),
                QStringLiteral("ScriptOnEnter"),
                QStringLiteral("ScriptOnTeleport"),
                QStringLiteral("EventObjectIndex"),
            };
            done = decodeTable(r.raw, 8, fields, sssChunkLabel(li), false, r);
            break;
        }
        case 2: {
            const int rec = m_win95 ? 14 : 12;
            QStringList fields = {QStringLiteral("#")};
            for (int i = 0; i < rec / 2; ++i)
                fields << QStringLiteral("W%1").arg(i);
            done = decodeTable(r.raw, rec, fields, sssChunkLabel(li), false, r);
            break;
        }
        case 3: {
            const int n = r.raw.size() / 4;
            if (n <= 0)
                break;
            r.view = LumpResult::View::Table;
            r.headers = {QStringLiteral("#"), QStringLiteral("Offset"),
                         QStringLiteral("Byte")};
            const uchar *p = reinterpret_cast<const uchar *>(r.raw.constData());
            for (int i = 0; i < n; ++i) {
                const quint32 v = quint32(p[4 * i]) | (quint32(p[4 * i + 1]) << 8) |
                                  (quint32(p[4 * i + 2]) << 16) |
                                  (quint32(p[4 * i + 3]) << 24);
                r.rows << (QStringList{QString::number(i),
                                       QStringLiteral("0x%1").arg(v, 8, 16,
                                                                   QLatin1Char('0')),
                                       QString::number(v)});
            }
            r.detail = QStringLiteral("%1 message offsets — M.MSG holds %2 "
                                      "messages")
                           .arg(n)
                           .arg(qMax(0, n - 1));
            done = true;
            break;
        }
        case 4: {
            static const QStringList fields = {
                QStringLiteral("#"), QStringLiteral("Op"),
                QStringLiteral("Arg1"), QStringLiteral("Arg2"),
                QStringLiteral("Arg3"),
            };
            done = decodeTable(r.raw, 8, fields, sssChunkLabel(li), true, r);
            break;
        }
        default:
            break;
        }
        if (!done) {
            r.typeLabel = QStringLiteral("Data");
            r.detail = sssChunkLabel(li) + QStringLiteral(" (unstructured)");
        }
        break;
    }

    case FileRole::Data: {
        static const int recSizes[] = {18, 70, 10, -1, 32, 12, 20, -1, -1, -1, -1};
        const int nKnown = int(sizeof(recSizes) / sizeof(recSizes[0]));
        bool done = false;
        if (li < nKnown && recSizes[li] > 0) {
            QStringList fields = {QStringLiteral("#")};
            QString label = dataChunkLabel(li);
            if (li == 0) {
                for (int i = 0; i < 9; ++i)
                    fields << QStringLiteral("Item%1").arg(i + 1);
            } else if (li == 1) {
                fields = enemyFields();
            } else if (li == 2) {
                for (int i = 0; i < 5; ++i)
                    fields << QStringLiteral("Enemy%1").arg(i + 1);
            } else if (li == 4) {
                static const char *mf[] = {
                    "Effect", "Type", "XOffset", "YOffset", "Specific",
                    "Speed", "KeepEffect", "FireDelay", "EffectTimes",
                    "Shake", "Wave", "Unknown", "CostMP", "BaseDamage",
                    "Elemental", "Sound",
                };
                for (const char *m : mf)
                    fields << QString::fromLatin1(m);
            } else if (li == 5) {
                static const char *bf[] = {
                    "ScreenWave", "Color0", "Color1", "Color2", "Color3",
                    "Color4",
                };
                for (const char *m : bf)
                    fields << QString::fromLatin1(m);
            } else if (li == 6) {
                for (int i = 0; i < 5; ++i)
                    fields << QStringLiteral("P%1Magic").arg(i)
                           << QStringLiteral("P%1Level").arg(i);
            }
            done = decodeTable(r.raw, recSizes[li], fields, label, false, r);
        } else if (li == 9 || li == 10) {
            done = decodeSprite(r.raw, true, r);
            if (done)
                r.typeLabel = dataChunkLabel(li) + QStringLiteral(" (sprite)");
        }
        if (!done) {
            r.typeLabel = QStringLiteral("Data");
            r.detail = dataChunkLabel(li) + QStringLiteral(" (unstructured)");
        }
        break;
    }

    case FileRole::Words: {
        PalText t = parseWords(r.raw, 10);
        r.view = LumpResult::View::Text;
        r.typeLabel = QStringLiteral("Word list");
        r.headers = t.headers;
        for (int i = 0; i < t.rows.size(); ++i)
            r.rows << (QStringList{QString::number(i), t.rows[i]});
        r.headers.prepend(QStringLiteral("#"));
        r.plainText = t.toPlainText();
        r.cp = t.cp;
        r.confidence = t.confidence;
        r.detail = QStringLiteral("%1 words, %2 (confidence %3%)")
                       .arg(t.rows.size())
                       .arg(t.cp == CP_GBK ? QStringLiteral("GBK/GB18030")
                                           : QStringLiteral("Big5"))
                       .arg(t.confidence);
        break;
    }

    case FileRole::Msg: {
        const QByteArray offs = messageOffsets();
        PalText t;
        if (!offs.isEmpty()) {
            t = parseMessages(r.raw, offs);
        }
        if (t.rows.isEmpty()) {
            const CodepageGuess g = detectCodepage(r.raw);
            t.cp = g.cp;
            t.confidence = g.confidence;
            t.headers = {QStringLiteral("Message")};
            t.rows.append(pal::decode(r.raw, g.cp));
        }
        r.view = LumpResult::View::Text;
        r.typeLabel = QStringLiteral("Messages");
        r.headers = t.headers;
        r.headers.prepend(QStringLiteral("#"));
        for (int i = 0; i < t.rows.size(); ++i)
            r.rows << (QStringList{QString::number(i), t.rows[i]});
        r.plainText = t.toPlainText();
        r.cp = t.cp;
        r.confidence = t.confidence;
        r.detail = QStringLiteral("%1 messages, %2 (confidence %3%)")
                       .arg(t.rows.size())
                       .arg(t.cp == CP_GBK ? QStringLiteral("GBK/GB18030")
                                           : QStringLiteral("Big5"))
                       .arg(t.confidence);
        break;
    }

    case FileRole::Desc: {
        PalText t = parseDesc(r.raw);
        r.view = LumpResult::View::Text;
        r.typeLabel = QStringLiteral("Descriptions");
        r.headers = t.headers;
        r.headers.prepend(QStringLiteral("#"));
        for (int i = 0; i < t.rows.size(); ++i) {
            QStringList row{QString::number(i)};
            row << t.rows[i];
            r.rows.append(row);
        }
        r.plainText = t.toPlainText();
        r.cp = t.cp;
        r.confidence = t.confidence;
        r.detail = QStringLiteral("%1 entries, %2 (confidence %3%)")
                       .arg(t.rows.size())
                       .arg(t.cp == CP_GBK ? QStringLiteral("GBK/GB18030")
                                           : QStringLiteral("Big5"))
                       .arg(t.confidence);
        break;
    }

    case FileRole::Unknown: {
        // Sniff cascade: sound -> sprite -> background -> hex.
        const SoundInfo si = parseSound(r.raw);
        if (si.ok) {
            soundFrom(si);
            break;
        }
        if (decodeSprite(r.raw, true, r))
            break;
        if (r.raw.size() == kFbpSize) {
            Bitmap8 bmp;
            bmp.w = 320;
            bmp.h = 200;
            bmp.px = r.raw;
            r.view = LumpResult::View::Image;
            r.typeLabel = QStringLiteral("Raw 320×200 image");
            r.frames.append(toImage(bmp, activePalette()));
            r.detail = QStringLiteral("raw 8-bit 320×200");
            break;
        }
        r.typeLabel = QStringLiteral("Binary data");
        r.detail = QStringLiteral("%1 bytes — hex view, export as raw").arg(
            r.raw.size());
        break;
    }
    }

    return r;
}

QString GamePack::typeHint(int fi, int li) const
{
    if (fi < 0 || fi >= m_files.size())
        return {};
    const PackFile &f = m_files[fi];

    switch (f.role) {
    case FileRole::Pat: return QStringLiteral("Palette");
    case FileRole::Map: return QStringLiteral("Map");
    case FileRole::Gop: return QStringLiteral("Tiles");
    case FileRole::Fbp: return QStringLiteral("Background");
    case FileRole::Sounds: return QStringLiteral("Sound");
    case FileRole::Voc: return QStringLiteral("Sound (VOC)");
    case FileRole::Midi: return QStringLiteral("MIDI");
    case FileRole::Mus: return QStringLiteral("Music (RIX)");
    case FileRole::Rng: return QStringLiteral("Animation");
    case FileRole::Words: return QStringLiteral("Word list");
    case FileRole::Msg: return QStringLiteral("Messages");
    case FileRole::Desc: return QStringLiteral("Descriptions");
    case FileRole::Sss: return sssChunkLabel(li);
    case FileRole::Data: return dataChunkLabel(li);
    case FileRole::SpriteMkf: return QStringLiteral("Sprite");
    case FileRole::Unknown: break;
    }

    if (!f.isMkf) {
        const QString sn = sniff(f.raw.left(64));
        return sn.isEmpty() ? QStringLiteral("Data") : sn;
    }
    const QString sn = sniff(f.mkf.peek(li, 64));
    if (!sn.isEmpty())
        return sn;
    return QStringLiteral("Data");
}

} // namespace pal
