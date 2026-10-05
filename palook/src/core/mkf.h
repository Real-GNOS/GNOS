// MKF archive container reader.
//
// MKF format (little endian):
//   DWORD offset[0]  = offset of first chunk data (= 4 * (count + 1))
//   DWORD offset[1..count] = end offsets of chunks
//   chunk i occupies file bytes [offset[i], offset[i+1])
//
// This mirrors the reference implementation in sdlpal (palcommon.c:
// PAL_MKFGetChunkCount / PAL_MKFGetChunkSize / PAL_MKFReadChunk).

#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace pal {

class MkfArchive
{
public:
    MkfArchive() = default;

    // Read an .mkf file fully into memory and parse the offset table.
    bool open(const QString &path);
    // Parse an in-memory archive (used by nested tables too).
    bool openData(const QByteArray &data, const QString &name);

    bool isOpen() const { return !m_data.isEmpty() && m_count > 0; }
    QString path() const { return m_path; }
    QString errorString() const { return m_error; }

    int count() const { return m_count; }
    quint32 chunkOffset(int i) const;      // offset of chunk i
    quint32 chunkEnd(int i) const;         // offset past chunk i
    quint32 chunkSize(int i) const;

    // Raw (possibly still compressed) bytes of a chunk.
    QByteArray chunk(int i) const;
    // First n bytes of a chunk (cheap sniff for type hints).
    QByteArray peek(int i, int n) const;

    void close();

    // Helpers for nested offset tables (rng.mkf frames use the same layout
    // inside a chunk).
    static int nestedCount(const QByteArray &data);
    static QByteArray nestedChunk(const QByteArray &data, int i);

    static int countFromHeader(const char *base, qint64 size);

private:
    QString    m_path;
    QString    m_error;
    QByteArray m_data;
    int        m_count = 0;
    QVector<quint32> m_offsets;  // count+1 entries
};

} // namespace pal
