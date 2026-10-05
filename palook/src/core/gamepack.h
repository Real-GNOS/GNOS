// GamePack: high-level view of a PAL data directory (or a single file).
//
// Knows the role of every known resource file, decides how each lump has to
// be decoded (sprite / map / palette / sound / text / record table / hex)
// and provides the context that decoding needs (palette, GOP tiles, the
// SSS message offset table, DOS vs. WIN95 layout).

#pragma once

#include "mkf.h"
#include "palette.h"
#include "sound.h"
#include "textdec.h"

#include <QImage>
#include <QStringList>
#include <QVector>

namespace pal {

enum class FileRole {
    Pat, Map, Gop, Fbp, Sounds, Voc, Midi, Mus, Rng, Sss, Data,
    Words, Msg, Desc, SpriteMkf, Unknown
};

QString fileRoleName(FileRole r);

struct PackFile {
    QString path;
    QString name;          // base name, lower case
    FileRole role = FileRole::Unknown;
    bool isMkf = false;
    MkfArchive mkf;        // valid when isMkf
    QByteArray raw;        // contents when !isMkf
    qint64 size = 0;       // file size on disk
    QString error;

    int lumpCount() const
    {
        if (isMkf)
            return mkf.count();
        return raw.isEmpty() ? 0 : 1;
    }
};

struct LumpResult {
    enum class View { Hex, Image, Map, Sound, Text, Table };

    View view = View::Hex;
    QString typeLabel;
    QString detail;
    QString error;
    QByteArray raw;

    // Image / Map
    // Map: frames[0] = bottom layer, frames[1] = top layer, frames[2] = both.
    QVector<QImage> frames;
    QStringList frameLabels;
    bool isMap = false;

    // Sound
    SoundInfo snd;         // normalized (ok, kind, rate, wav, ...)
    QString sndKind;
    int rate = 0, channels = 0, bits = 0;
    double duration = 0;
    QByteArray wav;

    // Text / Table
    QStringList headers;
    QVector<QStringList> rows;
    QString plainText;
    int cp = -1;
    int confidence = -1;
};

class GamePack
{
public:
    // paths: files and/or directories
    bool openPaths(const QStringList &paths);
    void close();

    bool ok() const { return !m_files.isEmpty(); }
    QString errorString() const { return m_error; }
    const QVector<PackFile> &files() const { return m_files; }

    bool isWin95() const { return m_win95; }
    QString versionNote() const { return m_versionNote; }

    // Palette context -------------------------------------------------------
    bool hasPalette() const { return m_palette.valid; }
    const PalSet &paletteSet() const { return m_palette; }
    int paletteChunk() const { return m_palChunk; }
    int paletteChunkCount() const;
    bool setPaletteChunk(int idx);   // loads pat.mkf chunk idx
    void setNight(bool n) { m_night = n; }
    bool night() const { return m_night; }
    QVector<QRgb> activePalette() const;

    // Decoding --------------------------------------------------------------
    LumpResult decode(int fileIdx, int lumpIdx) const;
    // Cheap type label for the tree (peek + role, no full decode).
    QString typeHint(int fileIdx, int lumpIdx) const;

    // Message offsets (SSS chunk 3), empty when unavailable.
    QByteArray messageOffsets() const;

private:
    bool openFile(const QString &path);
    void openDir(const QString &dir);
    void detectVersion();
    void loadPalette();

    const PackFile *findRole(FileRole r) const;

    bool decodeSprite(const QByteArray &raw, bool allowDecompress,
                      LumpResult &r) const;
    bool decodeTable(const QByteArray &data, int recSize,
                     const QStringList &fields, const QString &label,
                     bool opHex, LumpResult &r) const;

    QVector<PackFile> m_files;
    QString m_error;
    bool m_win95 = false;
    QString m_versionNote;

    PalSet m_palette;
    int m_palChunk = 0;
    bool m_night = false;
    int m_fallbackCp = CP_BIG5;
};

} // namespace pal
