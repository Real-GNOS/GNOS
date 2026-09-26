#pragma once

#include <QMainWindow>
#include <QLineEdit>
#include <QSpinBox>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QListWidget>
#include <QThread>

#include "pro_options.h"

class BuildWorker;
class QGroupBox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void onBrowseElf();
    void onBrowseOutput();
    void onBrowseKernel();
    void onStartBuild();
    void onCancelBuild();
    void onBuildLog(const QString& msg);
    void onBuildProgress(int pct);
    void onBuildDone(bool ok, const QString& msg);
    void onAddExtraFile();
    void onRemoveExtraFile();

private:
    QLineEdit* elfEdit;
    QLineEdit* outputEdit;
    QLineEdit* nameEdit;
    QLineEdit* kernelEdit;
    QLineEdit* tmpDirEdit;
    QCheckBox* stripCheck;
    QCheckBox* verboseCheck;
    QPushButton* buildBtn;
    QPushButton* cancelBtn;
    QProgressBar* progressBar;
    QLabel* statusLabel;
    QPlainTextEdit* logView;
    BuildWorker* worker;

    // pro options
    QGroupBox* proBox;
    QLineEdit* kcmdEdit;
    QSpinBox* gzipSpin;
    QListWidget* extraFilesList;
    QLineEdit* initCmdEdit;
    QCheckBox* keepTmpCheck;
};
