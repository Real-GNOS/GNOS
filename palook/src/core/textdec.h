// Text resources: codepage detection + WORD.DAT / M.MSG / DESC.DAT parsing.
// Detection mirrors sdlpal's PAL_DetectCodePageForString scoring.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pal {

enum Codepage {
    CP_BIG5 = 0,
    CP_GBK = 1,   // decoded with Qt's GB18030 codec (superset of GBK)
};

struct CodepageGuess {
    int cp = CP_BIG5;
    int confidence = 0; // 0..100
    QString name() const;
};

// Choose the codepage with the fewest characters outside the ranges PAL
// text is allowed to use (CJK, ASCII, fullwidth forms, ...).
CodepageGuess detectCodepage(const QByteArray &raw);

// Decode bytes with the given codepage.
QString decode(const QByteArray &raw, int cp);

struct PalText {
    QStringList headers;
    QVector<QString> rows;
    int cp = CP_BIG5;
    int confidence = 0;

    QString toPlainText() const;
};

// WORD.DAT: fixed-size records (10 bytes for the Chinese versions),
// space padded, trailing '1' stripped like the game does.
PalText parseWords(const QByteArray &raw, int recordLen = 10);

// M.MSG with the per-message offset table from SSS.MKF chunk 3.
PalText parseMessages(const QByteArray &msg, const QByteArray &offsetsChunk);

// DESC.DAT: lines of "<hex id>=<description>".
PalText parseDesc(const QByteArray &raw);

} // namespace pal
