#include "MainWindow.h"
#include "BuildWorker.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>
#include <QStatusBar>
#include <QStyle>
#include <QApplication>
#include <QThread>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include "kernel_resolver.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("gelf2linux — ELF to Linux ISO Builder");
    setMinimumSize(680, 520);

    QWidget* central = new QWidget;
    setCentralWidget(central);
    QVBoxLayout* mainLayout = new QVBoxLayout(central);

    // --- input group ---
    QGroupBox* inputBox = new QGroupBox("Input");
    QFormLayout* form = new QFormLayout(inputBox);

    elfEdit = new QLineEdit;
    elfEdit->setPlaceholderText("/path/to/your.elf");
    form->addRow("ELF File:", elfEdit);
    connect(elfEdit, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (!t.isEmpty() && nameEdit->text().isEmpty()) {
            QFileInfo fi(t);
            QString n = fi.completeBaseName();
            QString cleaned;
            for (const QChar& c : n)
                cleaned += (c.isLetterOrNumber() || c == '-' || c == '_') ? c : '_';
            if (!cleaned.isEmpty()) nameEdit->setText(cleaned);
        }
    });

    outputEdit = new QLineEdit;
    outputEdit->setPlaceholderText("output.iso (auto-generated if empty)");
    form->addRow("Output:", outputEdit);

    nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText("myos");
    form->addRow("OS Name:", nameEdit);

    kernelEdit = new QLineEdit;
    kernelEdit->setPlaceholderText("auto-detect host kernel");
    form->addRow("Kernel:", kernelEdit);

    QHBoxLayout* kernelRow = new QHBoxLayout;
    kernelRow->addWidget(kernelEdit);
    QPushButton* kernelBtn = new QPushButton("Auto");
    kernelBtn->setFixedWidth(50);
    connect(kernelBtn, &QPushButton::clicked, this, [this]() {
        KernelInfo k = find_host_kernel();
        if (!k.vmlinuz.empty()) {
            kernelEdit->setText(QString::fromStdString(k.vmlinuz));
        } else {
            QMessageBox::information(this, "Kernel", "No kernel found in /boot/");
        }
    });
    kernelRow->addWidget(kernelBtn);
    // replace kernel row
    form->removeRow(3);
    form->addRow("Kernel:", kernelRow);

    QHBoxLayout* tmpRow = new QHBoxLayout;
    tmpDirEdit = new QLineEdit("/tmp");
    tmpDirEdit->setPlaceholderText("temporary build space (need disk room)");
    tmpRow->addWidget(tmpDirEdit);
    QPushButton* tmpBtn = new QPushButton("Browse");
    tmpBtn->setFixedWidth(70);
    connect(tmpBtn, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this, "Select temp directory", tmpDirEdit->text());
        if (!dir.isEmpty()) tmpDirEdit->setText(dir);
    });
    tmpRow->addWidget(tmpBtn);
    form->addRow("Temp dir:", tmpRow);

    mainLayout->addWidget(inputBox);

    // --- options ---
    QGroupBox* optBox = new QGroupBox("Options");
    QHBoxLayout* optLayout = new QHBoxLayout(optBox);
    stripCheck = new QCheckBox("Strip .so files");
    stripCheck->setChecked(true);
    verboseCheck = new QCheckBox("Verbose output");
    optLayout->addWidget(stripCheck);
    optLayout->addWidget(verboseCheck);
    optLayout->addStretch();
    mainLayout->addWidget(optBox);

    // --- pro options ---
    proBox = new QGroupBox("Pro Options");
    proBox->setCheckable(true);
    proBox->setChecked(false);
    QFormLayout* proForm = new QFormLayout(proBox);

    kcmdEdit = new QLineEdit("console=tty0 init=/init quiet");
    kcmdEdit->setEnabled(false);
    proForm->addRow("Kernel cmdline:", kcmdEdit);

    gzipSpin = new QSpinBox;
    gzipSpin->setRange(1, 9);
    gzipSpin->setValue(9);
    gzipSpin->setEnabled(false);
    proForm->addRow("gzip level:", gzipSpin);

    QHBoxLayout* extraRow = new QHBoxLayout;
    extraFilesList = new QListWidget;
    extraFilesList->setMaximumHeight(80);
    extraFilesList->setEnabled(false);
    extraRow->addWidget(extraFilesList, 1);
    QVBoxLayout* extraBtns = new QVBoxLayout;
    QPushButton* addFileBtn = new QPushButton("Add");
    QPushButton* delFileBtn = new QPushButton("Remove");
    addFileBtn->setEnabled(false);
    delFileBtn->setEnabled(false);
    extraBtns->addWidget(addFileBtn);
    extraBtns->addWidget(delFileBtn);
    extraBtns->addStretch();
    extraRow->addLayout(extraBtns);
    proForm->addRow("Extra files:", extraRow);

    initCmdEdit = new QLineEdit("exec /bin/os.elf");
    initCmdEdit->setEnabled(false);
    proForm->addRow("Init command:", initCmdEdit);

    keepTmpCheck = new QCheckBox("Keep temporary build files");
    keepTmpCheck->setEnabled(false);
    proForm->addRow("", keepTmpCheck);

    connect(proBox, &QGroupBox::toggled, this, [this, addFileBtn, delFileBtn](bool on) {
        kcmdEdit->setEnabled(on);
        gzipSpin->setEnabled(on);
        extraFilesList->setEnabled(on);
        addFileBtn->setEnabled(on);
        delFileBtn->setEnabled(on);
        initCmdEdit->setEnabled(on);
        keepTmpCheck->setEnabled(on);
    });
    connect(addFileBtn, &QPushButton::clicked, this, &MainWindow::onAddExtraFile);
    connect(delFileBtn, &QPushButton::clicked, this, &MainWindow::onRemoveExtraFile);
    mainLayout->addWidget(proBox);

    // --- buttons ---
    QHBoxLayout* btnLayout = new QHBoxLayout;
    buildBtn = new QPushButton("Build");
    buildBtn->setStyleSheet("QPushButton{font-weight:bold;padding:8px 24px;}");
    cancelBtn = new QPushButton("Cancel");
    cancelBtn->setEnabled(false);
    btnLayout->addStretch();
    btnLayout->addWidget(buildBtn);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);
    connect(buildBtn, &QPushButton::clicked, this, &MainWindow::onStartBuild);
    connect(cancelBtn, &QPushButton::clicked, this, &MainWindow::onCancelBuild);

    // --- log ---
    logView = new QPlainTextEdit;
    logView->setReadOnly(true);
    logView->setMaximumBlockCount(5000);
    logView->setStyleSheet("QPlainTextEdit{background:#1a1a2e;color:#e0e0e0;font-family:monospace;font-size:10pt;}");
    mainLayout->addWidget(logView, 1);

    // --- status bar ---
    progressBar = new QProgressBar;
    progressBar->setValue(0);
    progressBar->setFixedHeight(20);
    mainLayout->addWidget(progressBar);

    statusLabel = new QLabel("Ready");
    statusBar()->addWidget(statusLabel);

    worker = nullptr;
}

void MainWindow::onBrowseElf() {
    QString path = QFileDialog::getOpenFileName(this, "Select ELF file", "", "All files (*)");
    if (!path.isEmpty()) elfEdit->setText(path);
}

void MainWindow::onBrowseOutput() {
    QString path = QFileDialog::getSaveFileName(this, "Save ISO", "", "ISO files (*.iso);;All files (*)");
    if (!path.isEmpty()) outputEdit->setText(path);
}

void MainWindow::onBrowseKernel() {
    QString path = QFileDialog::getOpenFileName(this, "Select kernel", "/boot", "Kernel (vmlinuz*);;All files (*)");
    if (!path.isEmpty()) kernelEdit->setText(path);
}

void MainWindow::onAddExtraFile() {
    QString src = QFileDialog::getOpenFileName(this, "Select host file to add");
    if (src.isEmpty()) return;
    bool ok = false;
    QString dst = QInputDialog::getText(this, "Destination path",
                                        "Path inside initramfs (e.g. /root/mydata):",
                                        QLineEdit::Normal, QFileInfo(src).fileName(), &ok);
    if (!ok || dst.isEmpty()) return;
    if (!dst.startsWith('/')) dst.prepend('/');
    QListWidgetItem* item = new QListWidgetItem(QString("%1  ->  %2").arg(src, dst));
    item->setData(Qt::UserRole, src);
    item->setData(Qt::UserRole + 1, dst);
    extraFilesList->addItem(item);
}

void MainWindow::onRemoveExtraFile() {
    delete extraFilesList->takeItem(extraFilesList->currentRow());
}

void MainWindow::onStartBuild() {
    QString elf = elfEdit->text().trimmed();
    QString name = nameEdit->text().trimmed();
    QString output = outputEdit->text().trimmed();
    QString kernel = kernelEdit->text().trimmed();

    if (elf.isEmpty()) {
        QMessageBox::warning(this, "Input", "Please select an ELF file.");
        return;
    }
    if (!QFile::exists(elf)) {
        QMessageBox::warning(this, "Input", "ELF file does not exist.");
        return;
    }
    if (name.isEmpty()) name = "myos";
    if (output.isEmpty()) output = name + ".iso";

    buildBtn->setEnabled(false);
    cancelBtn->setEnabled(true);
    logView->clear();
    progressBar->setValue(0);
    statusLabel->setText("Building...");

    QThread* thread = QThread::create([this, elf, output, name, kernel]() {
        ProOptions pro;
        pro.enabled = proBox->isChecked();
        if (pro.enabled) {
            pro.kcmd = kcmdEdit->text().toStdString();
            pro.gzip_level = gzipSpin->value();
            for (int i = 0; i < extraFilesList->count(); i++) {
                QListWidgetItem* it = extraFilesList->item(i);
                pro.extra_files.emplace_back(
                    it->data(Qt::UserRole).toString().toStdString(),
                    it->data(Qt::UserRole + 1).toString().toStdString());
            }
            pro.init_cmd = initCmdEdit->text().toStdString();
            pro.keep_tmp = keepTmpCheck->isChecked();
        }
        BuildWorker w(elf, output, name, kernel, tmpDirEdit->text(),
                      stripCheck->isChecked(), verboseCheck->isChecked(),
                      pro);
        connect(&w, &BuildWorker::log, this, &MainWindow::onBuildLog, Qt::QueuedConnection);
        connect(&w, &BuildWorker::progress, this, &MainWindow::onBuildProgress, Qt::QueuedConnection);
        connect(&w, &BuildWorker::done, this, &MainWindow::onBuildDone, Qt::QueuedConnection);
        connect(this, &MainWindow::destroyed, &w, &BuildWorker::cancel);
        w.start();
    });
    worker = reinterpret_cast<BuildWorker*>(thread);
    thread->start();
}

void MainWindow::onCancelBuild() {
    // worker will be cleaned up by thread exit
    buildBtn->setEnabled(true);
    cancelBtn->setEnabled(false);
    statusLabel->setText("Cancelled");
    logView->appendPlainText("[cancelled]");
}

void MainWindow::onBuildLog(const QString& msg) {
    logView->appendPlainText(msg);
}

void MainWindow::onBuildProgress(int pct) {
    progressBar->setValue(pct);
}

void MainWindow::onBuildDone(bool ok, const QString& msg) {
    buildBtn->setEnabled(true);
    cancelBtn->setEnabled(false);
    if (ok) {
        statusLabel->setText(msg);
        logView->appendPlainText("\n" + msg);
        QMessageBox::information(this, "Build Complete", msg);
    } else {
        statusLabel->setText("Failed: " + msg);
        logView->appendPlainText("\n[ERROR] " + msg);
        QMessageBox::critical(this, "Build Failed", msg);
    }
    progressBar->setValue(ok ? 100 : 0);
}
