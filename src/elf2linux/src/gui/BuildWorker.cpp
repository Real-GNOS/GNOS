#include "BuildWorker.h"
#include "elf_reader.h"
#include "dep_collector.h"
#include "initramfs.h"
#include "kernel_resolver.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTextStream>

static QString sanitize_name(const QString& in) {
    QString out;
    for (const QChar& c : in) {
        if (c.isLetterOrNumber() || c == '-' || c == '_')
            out += c;
        else
            out += '_';
    }
    return out.isEmpty() ? "myos" : out;
}

BuildWorker::BuildWorker(const QString& elf, const QString& output,
                         const QString& osName, const QString& kernel,
                         const QString& tmpDir, bool strip, bool verbose,
                         const ProOptions& pro, QObject* parent)
    : QObject(parent), elfPath_(elf), outputPath_(output),
      osName_(sanitize_name(osName)), kernelPath_(kernel),
      tmpDir_(tmpDir.trimmed()),
      doStrip_(strip), verbose_(verbose), pro_(pro) {
    if (tmpDir_.isEmpty()) tmpDir_ = "/tmp";
    while (tmpDir_.endsWith('/'))
        tmpDir_.chop(1);
}

void BuildWorker::cancel() { cancelled_ = true; }

void BuildWorker::start() {
    auto run_cmd = [](const QStringList& args, QString* out = nullptr, QString* err = nullptr) -> int {
        QProcess p;
        p.start(args.first(), args.mid(1));
        p.waitForFinished(300000);
        if (out) *out = p.readAllStandardOutput();
        if (err) *err = p.readAllStandardError();
        return p.exitCode();
    };

    emit progress(0);

    // 1. Parse ELF
    emit log(QString("[1/5] Parsing ELF: %1").arg(elfPath_));
    std::string elf_std = elfPath_.toStdString();
    ElfInfo elf = read_elf_info(elf_std);
    if (!elf.valid) {
        emit done(false, "Not a valid ELF file");
        return;
    }
    emit log(QString("  arch: %1, type: %2")
             .arg(elf.is_64bit ? "x86_64" : "i386")
             .arg(elf.is_static ? "static" : "dynamic"));
    if (!elf.is_static)
        emit log(QString("  interp: %1").arg(QString::fromStdString(elf.interp)));
    emit progress(10);

    if (cancelled_) { emit done(false, "Cancelled"); return; }

    // 2. Find kernel
    emit log("[2/5] Locating host kernel...");
    KernelInfo kinfo;
    if (!kernelPath_.isEmpty()) {
        kinfo.vmlinuz = kernelPath_.toStdString();
    } else {
        kinfo = find_host_kernel();
    }
    if (kinfo.vmlinuz.empty()) {
        emit done(false, "No kernel found. Use kernel selector to specify one.");
        return;
    }
    emit log(QString("  kernel: %1").arg(QString::fromStdString(kinfo.vmlinuz)));
    emit progress(20);

    if (cancelled_) { emit done(false, "Cancelled"); return; }

    // 3. Build initramfs
    emit log("[3/5] Building initramfs...");
    QDir().mkpath(tmpDir_);
    QString tmpBase = tmpDir_ + "/elf2linux_" + osName_;
    QString initramfsDir = tmpBase + "_initramfs";
    QString initramfsPath = tmpBase + ".cpio.gz";

    // clean and create dirs
    QDir(initramfsDir).removeRecursively();
    QDir().mkpath(initramfsDir + "/bin");
    QDir().mkpath(initramfsDir + "/proc");
    QDir().mkpath(initramfsDir + "/sys");
    QDir().mkpath(initramfsDir + "/dev/pts");
    QDir().mkpath(initramfsDir + "/dev/shm");
    QDir().mkpath(initramfsDir + "/tmp");
    QDir().mkpath(initramfsDir + "/etc");

    // collect deps
    collect_deps(elf, elf_std, initramfsDir.toStdString(), doStrip_);
    emit log("  dependencies collected");

    // pro: copy extra files into the initramfs
    for (const auto& [src, dst] : pro_.extra_files) {
        QString host(QString::fromStdString(src));
        QString target(initramfsDir + QString::fromStdString(dst));
        if (!QFile::exists(host)) {
            emit done(false, QString("Extra file not found: %1").arg(host));
            return;
        }
        QDir d = QFileInfo(target).dir();
        if (!d.exists()) QDir().mkpath(d.absolutePath());
        QFile::copy(host, target);
        emit log(QString("  added %1 -> %2").arg(host, QString::fromStdString(dst)));
    }
    emit progress(40);

    if (cancelled_) { emit done(false, "Cancelled"); return; }

    // 4. Pack initramfs
    emit log("[4/5] Packing initramfs...");
    {
        // write init script
        QString initPath = initramfsDir + "/init";
        QFile f(initPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write("#!/bin/sh\n"
                    "mount -t proc proc /proc\n"
                    "mount -t sysfs sysfs /sys\n"
                    "mount -t devtmpfs devtmpfs /dev\n"
                    "mkdir -p /dev/pts /dev/shm\n"
                    "mount -t devpts devpts /dev/pts 2>/dev/null\n"
                    "mount -t tmpfs tmpfs /dev/shm\n"
                    "mount -t tmpfs tmpfs /tmp\n"
                    "export PATH=/bin:/usr/bin\n"
                    "export HOME=/tmp\n"
                    "export LD_LIBRARY_PATH=/lib:/lib64:/usr/lib:/usr/lib/x86_64-linux-gnu\n"
                    "echo \"\"\n"
                    "echo \"==============================\"\n"
                    "echo \"  elf2linux - ELF Boot\"\n"
                    "echo \"==============================\"\n"
                    "echo \"\"\n");
            f.write(QString("%1\n").arg(QString::fromStdString(pro_.init_cmd)).toUtf8());
            f.close();
            QFile::setPermissions(initPath, QFile::ReadUser | QFile::ExeUser);
        }
    }

    {
        QStringList args;
        args << "sh" << "-c"
             << QString("cd \"%1\" && find . -print0 | cpio -0 -o -H newc 2>/dev/null | gzip -%2 > \"%3\"")
                    .arg(initramfsDir)
                    .arg(pro_.gzip_level)
                    .arg(initramfsPath);
        QProcess p;
        p.start("sh", QStringList() << "-c" << args.last());
        p.waitForFinished(60000);
    }

    if (!QFile::exists(initramfsPath)) {
        // fallback without -print0/-0
        QStringList args;
        args << "-c"
             << QString("cd \"%1\" && find . -print | cpio -o -H newc 2>/dev/null | gzip -%2 > \"%3\"")
                    .arg(initramfsDir)
                    .arg(pro_.gzip_level)
                    .arg(initramfsPath);
        QProcess p;
        p.start("sh", args);
        p.waitForFinished(60000);
    }

    QFileInfo fi(initramfsPath);
    if (!QFile::exists(initramfsPath) || fi.size() < 100) {
        emit done(false, "cpio packing failed: initramfs archive is empty");
        return;
    }
    emit log(QString("  initramfs: %1 (%2 MB)")
             .arg(initramfsPath)
             .arg(fi.size() / 1048576.0, 0, 'f', 1));
    emit progress(60);

    if (cancelled_) { emit done(false, "Cancelled"); return; }

    // 5. Build ISO
    emit log("[5/5] Building ISO...");
    QString isoDir = tmpBase + "_iso";
    QDir(isoDir).removeRecursively();
    QDir().mkpath(isoDir + "/boot/grub");

    QFile::copy(QString::fromStdString(kinfo.vmlinuz), isoDir + "/boot/bzImage");
    QFile::copy(initramfsPath, isoDir + "/boot/initramfs.cpio.gz");

    // grub.cfg
    {
        QFile f(isoDir + "/boot/grub/grub.cfg");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "set timeout=0\nset default=0\n\n"
               << "menuentry \"" << osName_ << "\" {\n"
               << "    linux /boot/bzImage "
               << QString::fromStdString(pro_.kcmd) << "\n"
               << "    initrd /boot/initramfs.cpio.gz\n"
               << "}\n";
            f.close();
        }
    }

    // try grub-mkrescue
    int rc = 1;
    for (const QString& cmd : {"grub-mkrescue", "grub2-mkrescue"}) {
        if (QProcess::execute(cmd, {"-o", outputPath_, isoDir}) == 0) {
            rc = 0;
            break;
        }
    }

    if (rc != 0) {
        // try xorriso
        for (const QString& cmd : {"xorriso", "xorrisofs"}) {
            rc = QProcess::execute(cmd, {
                "-as", "mkisofs", "-iso-level", "3",
                "-full-iso9660-filenames",
                "-volid", osName_.left(32),
                "-output", outputPath_, isoDir
            });
            if (rc == 0) break;
        }
    }

    // cleanup
    if (pro_.keep_tmp) {
        emit log(QString("  keeping temp files: %1%2, %3%4, %5")
                 .arg(initramfsDir, "", isoDir, "", initramfsPath));
    } else {
        QDir(initramfsDir).removeRecursively();
        QDir(isoDir).removeRecursively();
        QFile::remove(initramfsPath);
    }

    if (rc != 0 || !QFile::exists(outputPath_)) {
        emit done(false, "ISO creation failed. Install grub-mkrescue or xorriso.");
        return;
    }

    emit progress(100);
    QFileInfo isoInfo(outputPath_);
    emit done(true, QString("ISO created: %1 (%2 MB)")
              .arg(outputPath_)
              .arg(isoInfo.size() / 1048576.0, 0, 'f', 1));
}
