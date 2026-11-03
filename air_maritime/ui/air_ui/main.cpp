// SecureCOM air & maritime console (C++ / Qt5 / ZeroMQ).
//   air_ui [--sub tcp://127.0.0.1:5565] [--cmd tcp://127.0.0.1:5566] [--tiles data/world_zoom12.mbtiles]
// self-test without screen or sound: --selftest SECONDS [--shot f.png] [--shot-after S] [--tab N] [--test-map]
#include <QApplication>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>

#include "mainwindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QString sub = "tcp://127.0.0.1:5565", cmd = "tcp://127.0.0.1:5566", shot, tiles = "data/world_zoom12.mbtiles";
    double selftest = 0, shotAfter = -1;
    int tab = 0;
    bool mapTest = false;
    for (int i = 1; i < argc; ++i) {
        QString a = argv[i];
        auto next = [&]() { return i + 1 < argc ? QString(argv[++i]) : QString(); };
        if (a == "--sub") sub = next();
        else if (a == "--cmd") cmd = next();
        else if (a == "--tiles") tiles = next();
        else if (a == "--selftest") selftest = next().toDouble();
        else if (a == "--shot") shot = next();
        else if (a == "--shot-after") shotAfter = next().toDouble();
        else if (a == "--tab") tab = next().toInt();
        else if (a == "--test-map") mapTest = true;
    }
    MainWindow w(sub, cmd, tiles);
    w.show();
    if (tab) w.showTab(tab);
    if (selftest > 0) {
        if (mapTest) QTimer::singleShot((int)(selftest * 500), [&] { w.runMapTest(); });
        if (!shot.isEmpty()) QTimer::singleShot((int)((shotAfter > 0 ? shotAfter : selftest - 1) * 1000), [&] { w.screenshot().save(shot); });
        QTimer::singleShot((int)(selftest * 1000), [&] {
            std::printf("%s\n", QJsonDocument(w.summary()).toJson(QJsonDocument::Indented).constData());
            std::fflush(stdout);
            app.quit();
        });
    }
    return app.exec();
}
