// Main window: file tree (files -> lumps), stacked previews, palette
// controls, export actions.

#pragma once

#include "core/gamepack.h"

#include <QMainWindow>

class QTreeView;
class QStandardItemModel;
class QStackedWidget;
class QPlainTextEdit;
class QSpinBox;
class QCheckBox;
class QLabel;

class ImagePreview;
class SoundPreview;
class TablePreview;
class HexPreview;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Open files/directories; false when nothing could be loaded.
    bool openPaths(const QStringList &paths);

    // Pick the first lump that decodes to an image (nice default view).
    void selectFirstImageLump();
    void selectFirstLump();

    // Non-GUI self test: decode every lump of a file/directory, print a
    // report. Returns 0 when all lumps decoded without error.
    static int runSelfTest(const QString &path);

    // GUI smoke test: show one lump per view kind (hex/image/map/sound/
    // text/table) through the real widgets. Returns 0 when every view
    // kind was exercised without crashing.
    int runUiTest();

private slots:
    void openFiles();
    void openDirectory();
    void exportLump();
    void exportPreview();
    void exportAll();
    void paletteChanged();
    void nightToggled(bool on);

private:
    void buildTree();
    void onTreeCurrentChanged();
    void decodeAndShow();
    void showResult(const pal::LumpResult &r);
    void updateInfoBar();
    void selectLump(int file, int lump);
    bool currentIndices(int *file, int *lump) const;
    QString suggestedBaseName() const;

    pal::GamePack m_pack;
    pal::LumpResult m_current;

    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    QStackedWidget *m_stack = nullptr;
    ImagePreview *m_image = nullptr;
    SoundPreview *m_sound = nullptr;
    TablePreview *m_table = nullptr;
    HexPreview *m_hex = nullptr;

    QPlainTextEdit *m_info = nullptr;
    QSpinBox *m_palSpin = nullptr;
    QCheckBox *m_night = nullptr;
    QLabel *m_status = nullptr;

    int m_curFile = -1;
    int m_curLump = -1;
};
