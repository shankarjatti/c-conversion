#pragma once
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <functional>
#include <map>

#include "airspec.h"
#include "link.h"
#include "mapwidget.h"
#include "tiles.h"

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const QString& subAddr, const QString& cmdAddr, const QString& mbtiles);
    ~MainWindow() override;
    QJsonObject summary() const;
    QPixmap screenshot();
    void showTab(int i);
    void runMapTest();
private slots:
    void onFrame(const Frame& f);
    void onGap(const QString& topic, int lost);
    void tick();
    void onReply(int id, const QJsonObject& hdr, const QByteArray& payload);
private:
    void buildUi();
    void command(const QJsonObject& req, std::function<void(const QJsonObject&, const QByteArray&)> cb = nullptr);
    void logLine(const QString& type, const QString& text);
    void setDecoderBadge(int state);
    void setBands(const QJsonArray& bands);
    void requestState();
    void upsert(bool ship, const QJsonObject& t);
    void remove(bool ship, const QString& id);
    void replaceAll(bool ship, const QJsonArray& list);
    void refreshMap();
    void renderTables();
    void jsonFeed(const QString& topic, const QJsonObject& payload);
    static bool hasPos(const QJsonObject& t) { return t.value("lat").isDouble() && t.value("lon").isDouble(); }
    static QString num(const QJsonValue& v, int dec = 1);

    QString subAddr_, cmdAddr_;
    SubThread* sub_;
    QThread cmdThread_;
    CommandClient* cmd_;
    int nextId_ = 1;
    std::map<int, std::function<void(const QJsonObject&, const QByteArray&)>> pending_;
    TileStore* tiles_;

    QMap<QString, QJsonObject> ais_, adsb_;          // id -> latest target record
    QMap<QString, qint64> lastMsg_;                  // "ais"/"adsb" -> time of the last target update
    QMap<QString, long long> topicCount_, topicLost_;
    QJsonObject lastStatus_, testResults_;
    bool decoderRunning_ = false, framed_ = false, jsonPaused_ = false;
    qint64 lastFrameAt_ = 0, lastStateReq_ = -10000;
    QString bandsSig_, mapSig_;
    QElapsedTimer clock_;
    long long specFrames_ = 0;

    AirSpectrum* spec_;
    MapWidget* map_;
    QTableWidget *signalsT_, *targetsT_;
    QTabWidget* tabs_;
    QPlainTextEdit *console_, *json_;
    QLabel *lblSystem_, *dotSystem_, *lblDec_, *dotDec_, *lblClock_, *lblShips_, *lblPlanes_, *lblStatus_, *lblPeak_;
    QCheckBox *chkShips_, *chkPlanes_, *chkHold_;
    QSpinBox* spinAvg_;
    QTimer* timer_;
};
