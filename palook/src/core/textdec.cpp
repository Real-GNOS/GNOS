#include "textdec.h"

#include <climits>
#include <QStringDecoder>

namespace pal {

namespace {

// Valid ranges from sdlpal's PAL_DetectCodePageForString.
bool inValidRange(uint u)
{
    static const int ranges[][2] = {
        { 0x4E00, 0x9FFF }, // CJK Unified Ideographs
        { 0x3400, 0x4DBF }, // CJK Unified Ideographs Extension A
        { 0xF900, 0xFAFF }, // CJK Compatibility Ideographs
        { 0x0020, 0x007E }, // Basic ASCII
        { 0x3000, 0x301E }, // CJK Symbols
        { 0xFF01, 0xFF5E }, // Fullwidth Forms
    };
    for (const auto &r : ranges)
        if (u >= uint(r[0]) && u <= uint(r[1]))
            return true;
    return false;
}

} // namespace

QString CodepageGuess::name() const
{
    return cp == CP_GBK ? QStringLiteral("GBK/GB18030")
                        : QStringLiteral("Big5");
}

QString decode(const QByteArray &rawIn, int cp)
{
    // The game's converter stops at the first NUL byte; do the same so
    // space-padded records behave identically.
    const int nul = rawIn.indexOf('\0');
    const QByteArrayView raw = nul >= 0 ? QByteArrayView(rawIn.constData(), nul)
                                        : QByteArrayView(rawIn);
    QStringDecoder dec(cp == CP_GBK ? "GB18030" : "Big5");
    if (!dec.isValid())
        return QString::fromUtf8(raw);
    return dec(raw);
}

CodepageGuess detectCodepage(const QByteArray &rawIn)
{
    CodepageGuess guess;
    QByteArray raw = rawIn;
    // Eliminate NULs so the whole buffer participates in scoring.
    raw.replace('\0', ' ');

    int minInvalids = INT_MAX;
    int best = CP_BIG5;
    for (int cp = CP_BIG5; cp <= CP_GBK; ++cp) {
        const QString s = decode(raw, cp);
        int invalids = 0;
        for (const QChar c : s) {
            if (!inValidRange(c.unicode()))
                ++invalids;
        }
        if (invalids < minInvalids) {
            minInvalids = invalids;
            best = cp;
        }
    }
    guess.cp = best;

    const int textLen = raw.size();
    if (textLen > 0 && minInvalids < textLen / 2)
        guess.confidence = (textLen / 2 - minInvalids) * 200 / textLen;
    else
        guess.confidence = 0;
    if (guess.confidence > 100)
        guess.confidence = 100;
    if (guess.confidence < 0)
        guess.confidence = 0;
    return guess;
}

QString PalText::toPlainText() const
{
    QStringList lines;
    lines.reserve(rows.size());
    for (const QString &r : rows)
        lines.append(r);
    return lines.join(QLatin1Char('\n'));
}

PalText parseWords(const QByteArray &raw, int recordLen)
{
    PalText out;
    if (recordLen <= 0)
        recordLen = 10;
    const CodepageGuess g = detectCodepage(raw);
    out.cp = g.cp;
    out.confidence = g.confidence;
    out.headers << QStringLiteral("Word");

    const int n = (raw.size() + recordLen - 1) / recordLen;
    for (int i = 0; i < n; ++i) {
        QByteArray rec = raw.mid(i * recordLen, recordLen);
        // Trailing spaces become NULs (the converter stops at the first NUL).
        for (int j = rec.size() - 1; j >= 0 && rec[j] == ' '; --j)
            rec[j] = '\0';
        QString s = decode(rec, g.cp);
        // The game strips a trailing '1' marker from each word.
        if (!s.isEmpty() && s.back() == QLatin1Char('1'))
            s.chop(1);
        out.rows.append(s);
    }
    return out;
}

PalText parseMessages(const QByteArray &msg, const QByteArray &offsetsChunk)
{
    PalText out;
    const CodepageGuess g = detectCodepage(msg);
    out.cp = g.cp;
    out.confidence = g.confidence;
    out.headers << QStringLiteral("Message");

    const int count = offsetsChunk.size() / 4;
    if (count < 2)
        return out;

    auto le32 = [](const char *p) -> quint32 {
        const uchar *b = reinterpret_cast<const uchar *>(p);
        return quint32(b[0]) | (quint32(b[1]) << 8) | (quint32(b[2]) << 16) |
               (quint32(b[3]) << 24);
    };

    const int nMsgs = count - 1;
    for (int i = 0; i < nMsgs; ++i) {
        const quint32 a = le32(offsetsChunk.constData() + 4 * i);
        const quint32 b = le32(offsetsChunk.constData() + 4 * (i + 1));
        if (b < a || a > quint32(msg.size())) {
            out.rows.append(QString());
            continue;
        }
        const quint32 end = qMin(b, quint32(msg.size()));
        out.rows.append(decode(msg.mid(int(a), int(end - a)), g.cp));
    }
    return out;
}

PalText parseDesc(const QByteArray &raw)
{
    PalText out;
    const CodepageGuess g = detectCodepage(raw);
    out.cp = g.cp;
    out.confidence = g.confidence;
    out.headers << QStringLiteral("ID") << QStringLiteral("Description");

    const QList<QByteArray> lines = raw.split('\n');
    for (QByteArray line : lines) {
        if (!line.isEmpty() && line.endsWith('\r'))
            line.chop(1);
        const int eq = line.indexOf('=');
        if (eq < 0)
            continue;
        bool ok = false;
        const uint id = line.left(eq).toUInt(&ok, 16);
        if (!ok)
            continue;
        const QString text = decode(line.mid(eq + 1), g.cp);
        out.rows.append({QStringLiteral("%1").arg(id, 0, 16), text});
    }
    return out;
}

} // namespace pal
