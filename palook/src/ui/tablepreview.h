// Table / text preview: a read-only grid with an optional text filter.
// Used for record tables (enemies, scripts, …) and decoded message lists.

#pragma once

#include "../core/gamepack.h"

#include <QWidget>

class QTableView;
class QStandardItemModel;
class QSortFilterProxyModel;
class QLineEdit;
class QLabel;

class TablePreview : public QWidget
{
    Q_OBJECT
public:
    explicit TablePreview(QWidget *parent = nullptr);

    void setResult(const pal::LumpResult &r);
    void clear();

    // Full text export (tab separated, with header).
    QString exportText() const;

private:
    QTableView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    QSortFilterProxyModel *m_proxy = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_info = nullptr;
    QStringList m_headers;
};
