#include "hexpreview.h"

#include <QFont>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace {

QString hexDump(const QByteArray &data, int maxBytes, bool *truncated)
{
    const int n = qMin(data.size(), maxBytes);
    *truncated = data.size() > n;

    QString out;
    out.reserve(n * 5 + (n / 16) * 24);
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    for (int i = 0; i < n; i += 16) {
        out += QStringLiteral("%1  ").arg(i, 8, 16, QLatin1Char('0'));
        QString ascii;
        for (int j = 0; j < 16; ++j) {
            if (i + j < n) {
                const uchar b = p[i + j];
                out += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
                ascii += (b >= 0x20 && b < 0x7f) ? QChar(b) : QLatin1Char('.');
            } else {
                out += QLatin1String("   ");
                ascii += QLatin1Char(' ');
            }
            if (j == 7)
                out += QLatin1Char(' ');
        }
        out += QLatin1Char(' ');
        out += ascii;
        out += QLatin1Char('\n');
    }
    return out;
}

} // namespace

HexPreview::HexPreview(QWidget *parent)
    : QWidget(parent)
{
    m_edit = new QPlainTextEdit(this);
    m_edit->setReadOnly(true);
    QFont f = m_edit->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::Monospace);
    m_edit->setFont(f);
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_edit);
}

void HexPreview::setBytes(const QByteArray &data)
{
    constexpr int kMax = 512 * 1024;
    bool truncated = false;
    QString text = hexDump(data, kMax, &truncated);
    if (truncated) {
        text += QStringLiteral("\n… truncated at %1 bytes "
                               "(use Export Lump for the full data)\n")
                    .arg(kMax);
    }
    m_edit->setPlainText(text);
}

void HexPreview::clear()
{
    m_edit->clear();
}
