// SecureCOM terrestrial console (C++ / Qt5 / ZeroMQ). Subscribes to the backend's terr/... results and sends commands on its REP socket.
//   terr_ui [--sub tcp://127.0.0.1:5560] [--cmd tcp://127.0.0.1:5561]
//   self-test (no user needed, e.g. QT_QPA_PLATFORM=offscreen): --selftest SECONDS [--shot file.png] [--shot-after S]
// prints one JSON summary: message counts, sequence gaps, per-band states, recording rows.
#include <QApplication>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>

#include "mainwindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QString sub = "tcp://127.0.0.1:5560", cmd = "tcp://127.0.0.1:5561", shot, delFile;
    int tab = 0;
    bool recTest = false, zoomTest = false;
    double selftest = 0, shotAfter = -1;
    for (int i = 1; i < argc; ++i) {
        QString a = argv[i];
        auto next = [&]() { return i + 1 < argc ? QString(argv[++i]) : QString(); };
        if (a == "--sub") sub = next();
        else if (a == "--cmd") cmd = next();
        else if (a == "--selftest") selftest = next().toDouble();
        else if (a == "--shot") shot = next();
        else if (a == "--shot-after") shotAfter = next().toDouble();
        else if (a == "--tab") tab = next().toInt();
        else if (a == "--test-zoom") zoomTest = true;
        else if (a == "--test-recordings") { recTest = true; delFile = next(); }
    }
    MainWindow w(sub, cmd);
    w.show();
    if (tab) w.showTab(tab);
    if (selftest > 0) {
        if (zoomTest) QTimer::singleShot(5000, [&] { w.runZoomTest(); });
        if (recTest) QTimer::singleShot(6000, [&] { w.runRecordingsTest(delFile == "-" ? QString() : delFile); });
        if (!shot.isEmpty()) QTimer::singleShot((int)((shotAfter > 0 ? shotAfter : selftest - 1) * 1000), [&] { w.screenshot().save(shot); });
        QTimer::singleShot((int)(selftest * 1000), [&] {
            std::printf("%s\n", QJsonDocument(w.summary()).toJson(QJsonDocument::Indented).constData());
            std::fflush(stdout);
            app.quit();
        });
    }
    return app.exec();
}
