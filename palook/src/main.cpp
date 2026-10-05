// PALook — browser for PAL (仙剑奇侠传) game data.
//
// Modes:
//   palook [file|dir ...]           interactive browser
//   palook --selftest <path>        decode everything, print a report
//   palook --screenshot <out.png>   render the first image lump, exit

#include "ui/mainwindow.h"

#include <QApplication>
#include <QPixmap>

#include <cstdio>

static void printUsage()
{
    std::printf(
        "PALook - browser for PAL (XianJiaQiXiaZhuan) game data\n"
        "usage: palook [options] [file|dir ...]\n"
        "  --selftest <path>     decode every lump, print a report, exit\n"
        "  --uitest <path>       show one lump per view kind, exit\n"
        "  --screenshot <png>    render the first image lump, write it, exit\n"
        "  -h, --help            this text\n");
}

int main(int argc, char *argv[])
{
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    QString selftest, uitest, screenshot;
    QStringList paths;
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        if (a == QLatin1String("--selftest") && i + 1 < args.size())
            selftest = args[++i];
        else if (a == QLatin1String("--uitest") && i + 1 < args.size())
            uitest = args[++i];
        else if (a == QLatin1String("--screenshot") && i + 1 < args.size())
            screenshot = args[++i];
        else if (a == QLatin1String("-h") || a == QLatin1String("--help")) {
            printUsage();
            return 0;
        } else {
            paths << a;
        }
    }

    // Headless fallback: no display and no platform chosen -> offscreen.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") &&
        qEnvironmentVariableIsEmpty("DISPLAY") &&
        qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PALook"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));
    QApplication::setOrganizationName(QStringLiteral("PALook"));

    if (!selftest.isEmpty())
        return MainWindow::runSelfTest(selftest);

    if (!uitest.isEmpty()) {
        // Pre-validate headlessly: MainWindow::openPaths would show a modal
        // dialog on failure, which can never close without a user.
        pal::GamePack probe;
        if (!probe.openPaths({uitest})) {
            std::fprintf(stderr, "uitest: cannot open %s: %s\n",
                         qPrintable(uitest), qPrintable(probe.errorString()));
            return 2;
        }
        MainWindow w;
        if (!w.openPaths({uitest}))
            return 2;
        w.show();
        for (int i = 0; i < 3; ++i)
            QApplication::processEvents();
        return w.runUiTest();
    }

    MainWindow w;
    const bool opened = !paths.isEmpty() ? w.openPaths(paths) : false;

    if (!screenshot.isEmpty()) {
        if (!opened) {
            std::fprintf(stderr, "screenshot: nothing to render\n");
            return 3;
        }
        w.resize(1280, 800);
        w.show();
        for (int i = 0; i < 5; ++i)
            QApplication::processEvents();
        const QPixmap px = w.grab();
        if (!px.save(screenshot)) {
            std::fprintf(stderr, "screenshot: cannot write %s\n",
                         qPrintable(screenshot));
            return 4;
        }
        std::printf("wrote %s (%dx%d)\n", qPrintable(screenshot), px.width(),
                    px.height());
        return 0;
    }

    w.show();
    return app.exec();
}
