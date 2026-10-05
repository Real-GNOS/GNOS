#include "mkf.h"

#include <QFile>

namespace pal {

static inline quint32 le32(const void *p)
{
    const uchar *b = static_cast<const uchar *>(p);
    return quint32(b[0]) | (quint32(b[1]) << 8) | (quint32(b[2]) << 16) |
           (quint32(b[3]) << 24);
}

int MkfArchive::countFromHeader(const char *base, qint64 size)
{
    if (size < 4)
        return 0;
    quint32 first = le32(base);
    if (first < 4 || first > quint32(size))
        return 0;
    return int((first - 4) / 4);
}

bool MkfArchive::open(const QString &path)
{
    close();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = f.errorString();
        return false;
    }
    const QByteArray data = f.readAll();
    if (!openData(data, path)) {
        m_error = m_error.isEmpty() ? QStringLiteral("bad MKF file")
                                    : m_error;
        return false;
    }
    return true;
}

bool MkfArchive::openData(const QByteArray &data, const QString &name)
{
    close();
    if (data.size() < 8) {
        m_error = QStringLiteral("file too small to be an MKF archive");
        return false;
    }
    m_data = data;
    m_path = name;

    m_count = countFromHeader(m_data.constData(), m_data.size());
    if (m_count <= 0) {
        m_error = QStringLiteral("bad MKF offset table");
        close();
        m_error = QStringLiteral("bad MKF offset table");
        return false;
    }

    m_offsets.resize(m_count + 1);
    quint32 prev = 0;
    for (int i = 0; i <= m_count; ++i) {
        quint32 o = le32(m_data.constData() + 4 * i);
        if (i > 0 && (o < prev || o > quint32(m_data.size()))) {
            const QString err =
                QStringLiteral("offset table out of range at entry %1").arg(i);
            close();
            m_error = err;
            return false;
        }
        m_offsets[i] = o;
        prev = o;
    }
    // Tolerate trailing padding after the last chunk.
    if (m_offsets[m_count] < quint32(m_data.size()))
        m_offsets[m_count] = quint32(m_data.size());

    m_error.clear();
    return true;
}

quint32 MkfArchive::chunkOffset(int i) const
{
    if (i < 0 || i >= m_count)
        return 0;
    return m_offsets[i];
}

quint32 MkfArchive::chunkEnd(int i) const
{
    if (i < 0 || i >= m_count)
        return 0;
    return m_offsets[i + 1];
}

quint32 MkfArchive::chunkSize(int i) const
{
    if (i < 0 || i >= m_count)
        return 0;
    return m_offsets[i + 1] - m_offsets[i];
}

QByteArray MkfArchive::chunk(int i) const
{
    if (!m_count || i < 0 || i >= m_count)
        return {};
    return m_data.mid(int(m_offsets[i]),
                      int(m_offsets[i + 1] - m_offsets[i]));
}

QByteArray MkfArchive::peek(int i, int n) const
{
    if (!m_count || i < 0 || i >= m_count || n <= 0)
        return {};
    const int a = int(m_offsets[i]);
    const int len = int(qMin<quint32>(quint32(n), m_offsets[i + 1] - m_offsets[i]));
    return m_data.mid(a, len);
}

void MkfArchive::close()
{
    m_data.clear();
    m_count = 0;
    m_offsets.clear();
    m_path.clear();
    m_error.clear();
}

int MkfArchive::nestedCount(const QByteArray &data)
{
    return countFromHeader(data.constData(), data.size());
}

QByteArray MkfArchive::nestedChunk(const QByteArray &data, int i)
{
    const int n = nestedCount(data);
    if (i < 0 || i >= n)
        return {};
    const quint32 a = le32(data.constData() + 4 * i);
    const quint32 b = le32(data.constData() + 4 * (i + 1));
    if (b < a || b > quint32(data.size()))
        return {};
    return data.mid(int(a), int(b - a));
}

} // namespace pal
