#pragma once

#include <QObject>
#include <QThread>
#include "pro_options.h"

class BuildWorker : public QObject {
    Q_OBJECT
public:
    explicit BuildWorker(const QString& elf, const QString& output,
                         const QString& osName, const QString& kernel,
                         const QString& tmpDir, bool strip, bool verbose,
                         const ProOptions& pro = ProOptions(),
                         QObject* parent = nullptr);

public slots:
    void start();
    void cancel();

signals:
    void log(const QString& msg);
    void progress(int pct);
    void done(bool ok, const QString& msg);

private:
    QString elfPath_, outputPath_, osName_, kernelPath_, tmpDir_;
    bool doStrip_, verbose_;
    ProOptions pro_;
    bool cancelled_ = false;
};
