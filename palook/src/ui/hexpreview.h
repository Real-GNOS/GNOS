// Hex dump viewer for lumps that have no structured interpretation.

#pragma once

#include <QWidget>

class QPlainTextEdit;

class HexPreview : public QWidget
{
    Q_OBJECT
public:
    explicit HexPreview(QWidget *parent = nullptr);

    void setBytes(const QByteArray &data);
    void clear();

private:
    QPlainTextEdit *m_edit = nullptr;
};
