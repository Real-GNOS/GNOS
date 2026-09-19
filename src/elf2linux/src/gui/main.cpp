#include <QApplication>
#include <QStyleFactory>
#include <QMessageBox>
#include <unistd.h>
#include <cstring>
#include "MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("gelf2linux");
    app.setApplicationVersion("2.0.0");
    app.setStyle(QStyleFactory::create("Fusion"));

    // root check: warn (not hard-fail) if not running as root
    bool skip_root_check = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-root-check") == 0 ||
            strcmp(argv[i], "-R") == 0) {
            skip_root_check = true;
        }
    }
    if (getuid() != 0 && !skip_root_check) {
        QMessageBox::StandardButton ret = QMessageBox::warning(
            nullptr, "gelf2linux — not running as root",
            "gelf2linux is not running with root privileges.\n\n"
            "Kernel discovery (/boot) and some library copies (/lib) "
            "may fail.\n\n"
            "Continue anyway?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return 1;
    }

    MainWindow w;
    w.show();
    return app.exec();
}