#include "tablepreview.h"

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

TablePreview::TablePreview(QWidget *parent)
    : QWidget(parent)
{
    auto *top = new QHBoxLayout;
    top->setContentsMargins(4, 4, 4, 0);

    m_info = new QLabel(this);
    top->addWidget(m_info, 1);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(QStringLiteral("Filter…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(260);
    top->addWidget(m_search);

    m_model = new QStandardItemModel(this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(-1);

    m_view = new QTableView(this);
    m_view->setModel(m_proxy);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setAlternatingRowColors(true);
    m_view->verticalHeader()->setDefaultSectionSize(20);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addLayout(top);
    lay->addWidget(m_view, 1);

    connect(m_search, &QLineEdit::textChanged, m_proxy,
            &QSortFilterProxyModel::setFilterFixedString);
}

void TablePreview::setResult(const pal::LumpResult &r)
{
    m_model->clear();
    m_model->setHorizontalHeaderLabels(r.headers);
    m_headers = r.headers;

    m_model->setRowCount(r.rows.size());
    for (int i = 0; i < r.rows.size(); ++i) {
        const QStringList &row = r.rows[i];
        for (int c = 0; c < row.size(); ++c) {
            auto *item = new QStandardItem(row[c]);
            if (c > 0)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_model->setItem(i, c, item);
        }
    }

    QString info = r.detail;
    if (r.cp >= 0) {
        info += QStringLiteral(" — %1, confidence %2%")
                    .arg(r.cp == pal::CP_GBK ? QStringLiteral("GBK/GB18030")
                                             : QStringLiteral("Big5"))
                    .arg(r.confidence);
    }
    m_info->setText(info);
    m_search->clear();
    m_view->resizeColumnsToContents();
    m_view->horizontalHeader()->setStretchLastSection(true);
}

void TablePreview::clear()
{
    m_model->clear();
    m_headers.clear();
    m_info->clear();
    m_search->clear();
}

QString TablePreview::exportText() const
{
    QString out;
    out += m_headers.join(QLatin1Char('\t')) + QLatin1Char('\n');
    const int rows = m_model->rowCount();
    const int cols = m_model->columnCount();
    for (int r = 0; r < rows; ++r) {
        QStringList cells;
        cells.reserve(cols);
        for (int c = 0; c < cols; ++c) {
            const QStandardItem *it = m_model->item(r, c);
            cells << (it ? it->text() : QString());
        }
        out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
    }
    return out;
}
