#pragma once
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QVector>
#include <functional>
#include <map>

#include "audio.h"
#include "link.h"
#include "plots.h"

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTimer;
class MeterBar;

struct BandState {
    QString key, name, tracePalette;
    QColor color;
    double freq = 0, rate = 200000;
    int channels = 1, audioRate = 8000;
    double chanBwHz = 200000;
    QVector<float> spec, tim, chan;
    bool active = false;
    double rmsDb = -120, floorDb = 0, bestDb = 0;
    int best = -1;
    QVector<int> occ;
    qint64 tIq = 0;
    bool audActive = false;
    double audRms = -120;
    qint64 tAud = 0;
    long long ticksRf = 0, ticksDecoding = 0;      // for the self-test summary
    long long audMsgs = 0, audMsgsActive = 0;
    double audVoiceSec = 0;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(const QString& subAddr, const QString& cmdAddr);
    ~MainWindow() override;
    QJsonObject summary() const;                    // numbers for the self-test (message counts, per-band states)
    QPixmap screenshot();
    void showTab(int i);
    void runZoomTest();
    void runRecordingsTest(const QString& deleteFile);   // self-test: fetch + parse a recording, delete a throw-away one
private slots:
    void onFrame(const Frame& f);
    void onGap(const QString& topic, int lost);
    void tick();
    void onReply(int id, const QJsonObject& hdr, const QByteArray& payload);
private:
    void buildUi();
    void setupBands(const QJsonArray& bands);
    void command(const QJsonObject& req, std::function<void(const QJsonObject&, const QByteArray&)> cb = nullptr);
    void logLine(const QString& band, const QString& text);
    void setDecoderBadge(int state);                // 0 unknown, 1 running, 2 stopped
    void setListen(bool on);
    void renderRecordings(const QJsonArray& items);
    void fetchRecording(const QString& file, std::function<void(const QByteArray&)> done);
    void playRecording(const QString& file, QPushButton* btn, QLabel* timeLbl);
    void updateSignalTables();
    void rebuildSpectrum();
    void requestState();
    int bandIndex(const QString& key) const { return index_.value(key, -1); }

    QString subAddr_, cmdAddr_;
    SubThread* sub_;
    QThread cmdThread_;
    CommandClient* cmd_;
    AudioMixer* mixer_;
    int nextId_ = 1;
    std::map<int, std::function<void(const QJsonObject&, const QByteArray&)>> pending_;

    QVector<BandState> bands_;
    QMap<QString, int> index_;
    QVector<float> bins_;                           // composite spectrum
    QVector<QVector<float>> wfRows_;
    QVector<float> timeBuf_;
    QString activeBand_;
    bool decoderRunning_ = false;
    int decoderState_ = 0;
    QJsonObject lastStatus_, lastTetra_;
    qint64 lastTetraAt_ = 0, lastFrameAt_ = 0, lastStatusAt_ = 0;
    double fmin_ = 419, fmax_ = 454;
    QElapsedTimer clock_;
    QMap<QString, long long> topicCount_, topicLost_;
    QString recSig_, bandsSig_;
    QJsonArray recItems_;
    QJsonObject testResults_;
    qint64 lastStateReq_ = -10000;
    QString playingFile_;
    WavPlayer* player_ = nullptr;
    QPushButton* playBtn_ = nullptr;
    QLabel* playTime_ = nullptr;

    SpectrumPlot* spec_;
    WaterfallPlot* wf_;
    TimePlot* time_;
    QTableWidget *signalsT_, *chanT_, *decodeT_;
    QTabWidget* tabs_;
    QListWidget* recList_;
    QLabel *lblSystem_, *lblDec_, *lblClock_, *dotSystem_, *dotDec_, *lblStatus_;
    QPushButton *btnStart_, *btnStop_, *btnListen_;
    QPlainTextEdit* console_;
    QWidget* audioPanel_;
    QMap<QString, MeterBar*> meters_;
    QTimer* timer_;
};
