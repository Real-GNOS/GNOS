#include "mainwindow.h"

#include "hexpreview.h"
#include "imagepreview.h"
#include "soundpreview.h"
#include "tablepreview.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTextStream>
#include <QTime>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>

#include <cstdio>

namespace {

enum ItemRole {
    RoleKind = Qt::UserRole,
    RoleFile,
    RoleLump,
};

enum ItemKind { KindFile = 1, KindLump = 2 };

QString fmtSize(qint64 n)
{
    if (n < 1024)
        return QStringLiteral("%1 B").arg(n);
    if (n < 1024 * 1024)
        return QStringLiteral("%1 KiB").arg(n / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MiB").arg(n / (1024.0 * 1024.0), 0, 'f', 2);
}

bool isImageRole(pal::FileRole r)
{
    using FR = pal::FileRole;
    switch (r) {
    case FR::Pat:
    case FR::Map:
    case FR::Gop:
    case FR::Fbp:
    case FR::SpriteMkf:
    case FR::Rng:
        return true;
    default:
        return false;
    }
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("PALook"));
    resize(1280, 800);

    // ---- central: tree | previews -------------------------------------
    m_tree = new QTreeView(this);
    m_model = new QStandardItemModel(this);
    m_tree->setModel(m_model);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Column resize modes are applied in buildTree(), once the model
    // actually has columns: the header is empty here and touching
    // section indexes of an empty header crashes in Qt 6.8.

    m_stack = new QStackedWidget(this);
    m_image = new ImagePreview(m_stack);
    m_sound = new SoundPreview(m_stack);
    m_table = new TablePreview(m_stack);
    m_hex = new HexPreview(m_stack);
    m_stack->addWidget(m_image);
    m_stack->addWidget(m_sound);
    m_stack->addWidget(m_table);
    m_stack->addWidget(m_hex);

    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(m_tree);
    split->addWidget(m_stack);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({400, 880});
    setCentralWidget(split);

    // ---- details dock --------------------------------------------------
    auto *dock = new QDockWidget(QStringLiteral("Details"), this);
    dock->setObjectName(QStringLiteral("detailsDock"));
    m_info = new QPlainTextEdit(dock);
    m_info->setReadOnly(true);
    m_info->setMaximumBlockCount(200);
    dock->setWidget(m_info);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    // ---- toolbar -------------------------------------------------------
    QToolBar *tb = addToolBar(QStringLiteral("Main"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);

    auto *actOpen = tb->addAction(QStringLiteral("Open…"), this,
                                  &MainWindow::openFiles);
    actOpen->setShortcut(QKeySequence::Open);
    auto *actOpenDir =
        tb->addAction(QStringLiteral("Open Folder…"), this,
                      &MainWindow::openDirectory);
    actOpenDir->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));

    tb->addSeparator();

    auto *actExpLump = tb->addAction(QStringLiteral("Export Lump"), this,
                                      &MainWindow::exportLump);
    actExpLump->setShortcut(QKeySequence::Save);
    auto *actExpPrev = tb->addAction(QStringLiteral("Export Preview"), this,
                                      &MainWindow::exportPreview);
    actExpPrev->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    auto *actExpAll = tb->addAction(QStringLiteral("Export All…"), this,
                                     &MainWindow::exportAll);
    actExpAll->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E")));

    tb->addSeparator();
    tb->addWidget(new QLabel(QStringLiteral("Palette "), this));
    m_palSpin = new QSpinBox(this);
    m_palSpin->setRange(0, 0);
    m_palSpin->setPrefix(QStringLiteral("#"));
    m_palSpin->setToolTip(QStringLiteral("Palette chunk from pat.mkf"));
    tb->addWidget(m_palSpin);
    m_night = new QCheckBox(QStringLiteral("Night"), this);
    m_night->setToolTip(QStringLiteral("Use the night palette (index + 128)"));
    tb->addWidget(m_night);

    m_status = new QLabel(this);
    statusBar()->addPermanentWidget(m_status, 1);
    statusBar()->showMessage(
        QStringLiteral("File → Open… to load a PAL .mkf or data folder"));

    // ---- connections ---------------------------------------------------
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &, const QModelIndex &) {
                onTreeCurrentChanged();
            });
    connect(m_palSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &MainWindow::paletteChanged);
    connect(m_night, &QCheckBox::toggled, this, &MainWindow::nightToggled);

    // ---- menu bar (shortcuts live on the toolbar actions) -------------
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(actOpen);
    fileMenu->addAction(actOpenDir);
    fileMenu->addSeparator();
    fileMenu->addAction(actExpLump);
    fileMenu->addAction(actExpPrev);
    fileMenu->addAction(actExpAll);
    fileMenu->addSeparator();
    auto *actQuit = fileMenu->addAction(QStringLiteral("Quit"));
    actQuit->setShortcut(QKeySequence::Quit);
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto *actAbout = helpMenu->addAction(QStringLiteral("About PALook"));
    connect(actAbout, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About PALook"),
            QStringLiteral(
                "<b>PALook</b> — browser for 仙剑奇侠传 (PAL) game data.<br>"
                "Opens MKF archives and resource files directly: sprites, "
                "maps, palettes,<br>backgrounds, sounds, MIDI, animations, "
                "messages and record tables.<br><br>"
                "Format decoding is a pure C++ port of the sdlpal format "
                "layer (GPLv3)."));
    });
}

// ---------------------------------------------------------------------------
// Opening

bool MainWindow::openPaths(const QStringList &paths)
{
    if (paths.isEmpty())
        return false;

    pal::GamePack fresh;
    if (!fresh.openPaths(paths)) {
        QMessageBox::warning(this, QStringLiteral("PALook"),
                             QStringLiteral("Could not open: %1")
                                 .arg(fresh.errorString().isEmpty()
                                          ? paths.join(QLatin1Char(' '))
                                          : fresh.errorString()));
        return false;
    }
    m_pack = std::move(fresh);

    m_curFile = m_curLump = -1;
    m_current = pal::LumpResult();
    buildTree();

    // Palette controls.
    const int palCount = m_pack.paletteChunkCount();
    m_palSpin->setRange(0, qMax(0, palCount - 1));
    m_palSpin->setValue(m_pack.paletteChunk());
    m_palSpin->setEnabled(palCount > 1);
    m_night->setChecked(m_pack.night());

    statusBar()->showMessage(
        QStringLiteral("Loaded %1 file(s) — %2")
            .arg(m_pack.files().size())
            .arg(m_pack.versionNote()),
        8000);

    selectFirstImageLump();
    return true;
}

void MainWindow::openFiles()
{
    QSettings s;
    const QString start = s.value(QStringLiteral("lastOpenDir")).toString();
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Open PAL files"), start,
        QStringLiteral("PAL resources (*.mkf *.dat *.msg);;All files (*)"));
    if (files.isEmpty())
        return;
    s.setValue(QStringLiteral("lastOpenDir"),
               QFileInfo(files.first()).absolutePath());
    openPaths(files);
}

void MainWindow::openDirectory()
{
    QSettings s;
    const QString start = s.value(QStringLiteral("lastOpenDir")).toString();
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open PAL data folder"), start);
    if (dir.isEmpty())
        return;
    s.setValue(QStringLiteral("lastOpenDir"), dir);
    openPaths({dir});
}

// ---------------------------------------------------------------------------
// Tree

void MainWindow::buildTree()
{
    m_tree->collapseAll();
    m_model->removeColumns(0, m_model->columnCount());
    m_model->setColumnCount(3);
    m_model->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Type"),
         QStringLiteral("Size")});
    QHeaderView *hdr = m_tree->header();
    hdr->setSectionResizeMode(0, QHeaderView::Stretch);
    hdr->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    const auto &files = m_pack.files();
    for (int fi = 0; fi < files.size(); ++fi) {
        const pal::PackFile &f = files[fi];

        auto *nameItem =
            new QStandardItem(QFileInfo(f.path).fileName());
        QString type = pal::fileRoleName(f.role);
        if (f.isMkf)
            type += QStringLiteral(" — %1 lumps").arg(f.lumpCount());
        auto *typeItem = new QStandardItem(type);
        auto *sizeItem = new QStandardItem(fmtSize(f.size));
        nameItem->setData(KindFile, RoleKind);
        nameItem->setData(fi, RoleFile);

        for (QStandardItem *it : {nameItem, typeItem, sizeItem}) {
            it->setEditable(false);
            it->setSelectable(true);
        }

        QList<QList<QStandardItem *>> rows;
        for (int li = 0; li < f.lumpCount(); ++li) {
            auto *ln = new QStandardItem(
                f.isMkf ? QStringLiteral("#%1").arg(li, 4)
                        : QStringLiteral("(file)"));
            auto *lt = new QStandardItem(m_pack.typeHint(fi, li));
            auto *lz = new QStandardItem(
                f.isMkf ? fmtSize(f.mkf.chunkSize(li)) : fmtSize(f.size));
            ln->setData(KindLump, RoleKind);
            ln->setData(fi, RoleFile);
            ln->setData(li, RoleLump);
            for (QStandardItem *it : {ln, lt, lz}) {
                it->setEditable(false);
                it->setSelectable(true);
            }
            rows.append(QList<QStandardItem *>() << ln << lt << lz);
        }
        for (const QList<QStandardItem *> &row : rows)
            nameItem->appendRow(row);
        m_model->appendRow(
            QList<QStandardItem *>() << nameItem << typeItem << sizeItem);
    }
}

bool MainWindow::currentIndices(int *file, int *lump) const
{
    *file = m_curFile;
    *lump = m_curLump;
    return *file >= 0 && *lump >= 0;
}

void MainWindow::onTreeCurrentChanged()
{
    const QModelIndex idx = m_tree->currentIndex();
    if (!idx.isValid())
        return;
    QStandardItem *it = m_model->itemFromIndex(idx.siblingAtColumn(0));
    if (!it)
        return;
    const int kind = it->data(RoleKind).toInt();
    if (kind == KindFile) {
        // Clicking a file row jumps to its first lump.
        if (it->rowCount() > 0) {
            const QModelIndex child = m_model->index(0, 0, it->index());
            if (m_tree->currentIndex() != child)
                m_tree->setCurrentIndex(child);
        }
        return;
    }
    if (kind != KindLump)
        return;

    m_curFile = it->data(RoleFile).toInt();
    m_curLump = it->data(RoleLump).toInt();
    decodeAndShow();
}

void MainWindow::decodeAndShow()
{
    if (m_curFile < 0 || m_curFile >= m_pack.files().size())
        return;
    const QTime t = QTime::currentTime();
    m_current = m_pack.decode(m_curFile, m_curLump);
    showResult(m_current);
    updateInfoBar();
    const int ms = t.msecsTo(QTime::currentTime());
    const pal::PackFile &f = m_pack.files()[m_curFile];
    m_status->setText(
        QStringLiteral("%1 — %2 — %3 (%4 ms)")
            .arg(QFileInfo(f.path).fileName())
            .arg(f.isMkf ? QStringLiteral("lump #%1").arg(m_curLump)
                         : QStringLiteral("file"))
            .arg(m_current.typeLabel)
            .arg(ms));
}

void MainWindow::showResult(const pal::LumpResult &r)
{
    switch (r.view) {
    case pal::LumpResult::View::Image:
    case pal::LumpResult::View::Map:
        m_image->setResult(r);
        m_stack->setCurrentWidget(m_image);
        break;
    case pal::LumpResult::View::Sound:
        m_sound->setResult(r.snd, r.detail);
        m_stack->setCurrentWidget(m_sound);
        break;
    case pal::LumpResult::View::Text:
    case pal::LumpResult::View::Table:
        m_table->setResult(r);
        m_stack->setCurrentWidget(m_table);
        break;
    case pal::LumpResult::View::Hex:
        m_hex->setBytes(r.raw);
        m_stack->setCurrentWidget(m_hex);
        break;
    }
}

void MainWindow::updateInfoBar()
{
    QStringList lines;
    if (m_curFile >= 0 && m_curFile < m_pack.files().size()) {
        const pal::PackFile &f = m_pack.files()[m_curFile];
        lines << QStringLiteral("File: %1 (%2)")
                     .arg(QFileInfo(f.path).fileName(),
                          pal::fileRoleName(f.role));
        if (f.isMkf) {
            lines << QStringLiteral("Lump: #%1  offset 0x%2  size %3")
                         .arg(m_curLump)
                         .arg(f.mkf.chunkOffset(m_curLump), 0, 16)
                         .arg(f.mkf.chunkSize(m_curLump));
        } else {
            lines << QStringLiteral("File content: %1 bytes").arg(f.size);
        }
    }
    if (!m_current.typeLabel.isEmpty())
        lines << QStringLiteral("Type: %1").arg(m_current.typeLabel);
    if (!m_current.detail.isEmpty())
        lines << m_current.detail;
    if (!m_current.error.isEmpty())
        lines << QString();
    if (!m_current.error.isEmpty())
        lines << QStringLiteral("ERROR: %1").arg(m_current.error);
    lines << QString();
    lines << QStringLiteral("Game version: %1").arg(m_pack.versionNote());
    if (m_pack.hasPalette()) {
        lines << QStringLiteral("Palette: chunk #%1%2")
                     .arg(m_pack.paletteChunk())
                     .arg(m_pack.night() ? QStringLiteral(" (night)")
                                         : QString());
    } else {
        lines << QStringLiteral("Palette: not loaded (defaults are used)");
    }
    m_info->setPlainText(lines.join(QLatin1Char('\n')));
}

// ---------------------------------------------------------------------------
// Selection helpers

void MainWindow::selectLump(int file, int lump)
{
    const QModelIndex fileIdx = m_model->index(file, 0);
    if (!fileIdx.isValid())
        return;
    const QModelIndex lumpIdx = m_model->index(lump, 0, fileIdx);
    if (!lumpIdx.isValid())
        return;
    m_tree->setCurrentIndex(lumpIdx);
    m_tree->scrollTo(lumpIdx);
    // If it was already the current index the signal did not fire.
    if (m_curFile != file || m_curLump != lump)
        onTreeCurrentChanged();
}

void MainWindow::selectFirstLump()
{
    const auto &files = m_pack.files();
    for (int fi = 0; fi < files.size(); ++fi) {
        if (files[fi].lumpCount() > 0) {
            selectLump(fi, 0);
            return;
        }
    }
}

void MainWindow::selectFirstImageLump()
{
    const auto &files = m_pack.files();
    for (int pass = 0; pass < 2; ++pass) {
        for (int fi = 0; fi < files.size(); ++fi) {
            const bool pref = isImageRole(files[fi].role);
            if ((pass == 0) != pref)
                continue;
            if (files[fi].lumpCount() <= 0)
                continue;
            const pal::LumpResult r = m_pack.decode(fi, 0);
            if ((r.view == pal::LumpResult::View::Image ||
                 r.view == pal::LumpResult::View::Map) &&
                !r.frames.isEmpty()) {
                selectLump(fi, 0);
                return;
            }
        }
    }
    selectFirstLump();
}

// ---------------------------------------------------------------------------
// Palette

void MainWindow::paletteChanged()
{
    if (!m_pack.setPaletteChunk(m_palSpin->value()))
        return;
    if (m_curFile >= 0)
        decodeAndShow();
}

void MainWindow::nightToggled(bool on)
{
    m_pack.setNight(on);
    if (m_curFile >= 0)
        decodeAndShow();
}

// ---------------------------------------------------------------------------
// Export

QString MainWindow::suggestedBaseName() const
{
    if (m_curFile < 0)
        return QStringLiteral("lump");
    const pal::PackFile &f = m_pack.files()[m_curFile];
    QString base = QFileInfo(f.path).fileName();
    if (f.isMkf)
        base += QStringLiteral("-%1").arg(m_curLump, 4, 10, QLatin1Char('0'));
    return base;
}

void MainWindow::exportLump()
{
    int fi, li;
    if (!currentIndices(&fi, &li))
        return;
    const QString def = suggestedBaseName() + QStringLiteral(".bin");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export lump (raw bytes)"), def,
        QStringLiteral("All files (*)"));
    if (path.isEmpty())
        return;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("PALook"),
                             out.errorString());
        return;
    }
    out.write(m_current.raw);
    out.close();
    statusBar()->showMessage(QStringLiteral("Exported %1 (%2)")
                                 .arg(path)
                                 .arg(fmtSize(m_current.raw.size())),
                             6000);
}

void MainWindow::exportPreview()
{
    int fi, li;
    if (!currentIndices(&fi, &li))
        return;

    switch (m_current.view) {
    case pal::LumpResult::View::Image:
    case pal::LumpResult::View::Map: {
        const QImage img = m_image->currentImage();
        if (img.isNull()) {
            QMessageBox::information(this, QStringLiteral("PALook"),
                                     QStringLiteral("No image to export."));
            return;
        }
        QString frame;
        if (m_image->frameCount() > 1)
            frame = QStringLiteral("-%1").arg(m_image->currentFrameLabel());
        frame.replace(QLatin1Char(' '), QLatin1Char('_'));
        const QString def = suggestedBaseName() + frame +
                            QStringLiteral(".png");
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export preview as PNG"), def,
            QStringLiteral("PNG image (*.png)"));
        if (path.isEmpty())
            return;
        if (!img.save(path, "PNG")) {
            QMessageBox::warning(this, QStringLiteral("PALook"),
                                 QStringLiteral("Failed to write %1").arg(path));
            return;
        }
        statusBar()->showMessage(QStringLiteral("Exported %1").arg(path),
                                 6000);
        break;
    }
    case pal::LumpResult::View::Sound: {
        if (m_sound->wav().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("PALook"),
                                     QStringLiteral("No audio to export."));
            return;
        }
        const QString def = suggestedBaseName() + QStringLiteral(".wav");
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export audio as WAV"), def,
            QStringLiteral("WAV audio (*.wav)"));
        if (path.isEmpty())
            return;
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, QStringLiteral("PALook"),
                                 out.errorString());
            return;
        }
        out.write(m_sound->wav());
        statusBar()->showMessage(QStringLiteral("Exported %1").arg(path),
                                 6000);
        break;
    }
    case pal::LumpResult::View::Text:
    case pal::LumpResult::View::Table: {
        const QString def = suggestedBaseName() + QStringLiteral(".txt");
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export table as text"), def,
            QStringLiteral("Text (*.txt)"));
        if (path.isEmpty())
            return;
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate |
                      QIODevice::Text)) {
            QMessageBox::warning(this, QStringLiteral("PALook"),
                                 out.errorString());
            return;
        }
        out.write(m_table->exportText().toUtf8());
        statusBar()->showMessage(QStringLiteral("Exported %1").arg(path),
                                 6000);
        break;
    }
    case pal::LumpResult::View::Hex:
        exportLump();
        break;
    }
}

void MainWindow::exportAll()
{
    const auto &files = m_pack.files();
    if (files.isEmpty())
        return;

    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Export all lumps into…"));
    if (dir.isEmpty())
        return;

    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    int lumps = 0, failed = 0;
    const auto &packs = m_pack.files();
    for (int fi = 0; fi < packs.size(); ++fi) {
        const pal::PackFile &f = packs[fi];
        QString subName = QFileInfo(f.path).fileName();
        const QString sub = dir + QLatin1Char('/') + subName;
        QDir().mkpath(sub);

        QFile idx(sub + QStringLiteral("/index.txt"));
        idx.open(QIODevice::WriteOnly | QIODevice::Text);
        QTextStream ts(&idx);
        ts << "# file\tlump\ttype\tsize\n";

        if (f.isMkf) {
            for (int li = 0; li < f.mkf.count(); ++li) {
                const QByteArray data = f.mkf.chunk(li);
                QFile out(
                    sub + QStringLiteral("/lump_%1.bin").arg(li, 4, 10,
                                                             QLatin1Char('0')));
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    ++failed;
                    continue;
                }
                out.write(data);
                ++lumps;
                ts << subName << '\t' << li << '\t' << m_pack.typeHint(fi, li)
                   << '\t' << data.size() << '\n';
            }
        } else {
            QFile out(sub + QLatin1Char('/') + QFileInfo(f.path).fileName());
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(f.raw);
                ++lumps;
                ts << subName << '\t' << "-\t" << m_pack.typeHint(fi, 0)
                   << '\t' << f.raw.size() << '\n';
            } else {
                ++failed;
            }
        }
    }
    QApplication::restoreOverrideCursor();

    QMessageBox::information(
        this, QStringLiteral("Export complete"),
        QStringLiteral("Exported %1 lump(s) from %2 file(s)%3.")
            .arg(lumps)
            .arg(packs.size())
            .arg(failed ? QStringLiteral(", %1 failed").arg(failed)
                        : QString()));
}

// ---------------------------------------------------------------------------
// Self test

int MainWindow::runSelfTest(const QString &path)
{
    pal::GamePack pack;
    if (!pack.openPaths({path})) {
        std::fprintf(stderr, "selftest: cannot open %s: %s\n",
                     qPrintable(path), qPrintable(pack.errorString()));
        return 2;
    }

    int lumps = 0, errors = 0;
    const auto &files = pack.files();
    std::printf("== %d file(s), version: %s\n", int(files.size()),
                qPrintable(pack.versionNote()));

    for (int fi = 0; fi < files.size(); ++fi) {
        const pal::PackFile &f = files[fi];
        const int n = f.lumpCount();
        for (int li = 0; li < n; ++li) {
            const pal::LumpResult r = pack.decode(fi, li);
            ++lumps;
            const bool bad = !r.error.isEmpty();
            if (bad)
                ++errors;
            std::printf("[%s] %-16s #%-4d %-26s %9lld  %s%s%s\n",
                        bad ? "ERR" : " ok", qPrintable(f.name), li,
                        qPrintable(r.typeLabel),
                        static_cast<long long>(r.raw.size()),
                        qPrintable(r.detail),
                        r.error.isEmpty() ? "" : " | ",
                        r.error.isEmpty() ? "" : qPrintable(r.error));
        }
        if (n == 0) {
            std::printf("[ERR] %-16s (no lumps)\n", qPrintable(f.name));
            ++errors;
        }
    }
    std::printf("== %d lump(s), %d error(s)\n", lumps, errors);
    return errors == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// UI smoke test

int MainWindow::runUiTest()
{
    // Must match the order of LumpResult::View.
    static const char *kViewNames[] = {"Hex", "Image", "Map",
                                       "Sound", "Text", "Table"};
    constexpr int kViewCount =
        int(sizeof(kViewNames) / sizeof(kViewNames[0]));

    const auto &files = m_pack.files();
    bool seen[kViewCount] = {};
    int tested = 0;
    for (int fi = 0; fi < files.size(); ++fi) {
        const pal::PackFile &f = files[fi];
        for (int li = 0; li < f.lumpCount(); ++li) {
            const int v = int(m_pack.decode(fi, li).view);
            if (v < 0 || v >= kViewCount || seen[v])
                continue;
            seen[v] = true;
            selectLump(fi, li);       // drives decodeAndShow + the widgets
            QApplication::processEvents();
            ++tested;
            std::fprintf(stderr, "[uitest] %-5s <- %s#%d ok\n", kViewNames[v],
                         qPrintable(f.name), li);
        }
    }

    int missing = 0;
    for (int v = 0; v < kViewCount; ++v) {
        if (!seen[v]) {
            std::fprintf(stderr, "[uitest] %-5s NOT EXERCISED\n",
                         kViewNames[v]);
            ++missing;
        }
    }
    std::fprintf(stderr, "== ui: %d/%d view kind(s) exercised, %d missing\n",
                 tested, kViewCount, missing);
    return missing == 0 ? 0 : 1;
}
