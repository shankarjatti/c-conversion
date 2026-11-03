// Sound: live decoded audio (all bands mixed, only while "Listen" is on) and recording playback, via PulseAudio.
// Nothing here makes a sound unless the user switches Listen on or presses a recording's play button.
#pragma once
#include <QByteArray>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>
#include <atomic>
#include <deque>

class AudioMixer : public QThread {
    Q_OBJECT
public:
    explicit AudioMixer(QObject* p = nullptr) : QThread(p) {}
    void setListen(bool on);
    bool listening() const { return on_; }
    void push(const QString& band, int rate, const QByteArray& pcm16);   // int16 little endian, mono
    void stop() { quit_ = true; }
    QString error() const { return err_; }
signals:
    void failed(const QString& why);
protected:
    void run() override;
private:
    struct Chan { std::deque<float> q; int rate = 48000; double phase = 0; };
    QMutex mu_;
    QMap<QString, Chan> ch_;
    std::atomic<bool> on_{false}, quit_{false};
    QString err_;
};

// Plays one 16-bit mono WAV image; position is reported while playing. Pause/resume keep the position.
class WavPlayer : public QThread {
    Q_OBJECT
public:
    WavPlayer(const QByteArray& wav, QObject* p = nullptr) : QThread(p), wav_(wav) {}
    bool parse();                       // false if it is not a 16-bit PCM WAV
    double duration() const { return rate_ ? (double)frames_ / rate_ : 0; }
    void pause(bool p) { paused_ = p; }
    bool paused() const { return paused_; }
    void stop() { quit_ = true; }
signals:
    void position(double seconds);
    void finished2(const QString& error);
protected:
    void run() override;
private:
    QByteArray wav_;
    int rate_ = 0;
    qint64 dataOff_ = 0, frames_ = 0;
    std::atomic<bool> paused_{false}, quit_{false};
};
