// ZeroMQ links of the console: one SUB thread (all terr/... results) and one REQ command thread.
// Envelope (docs/ZMQ_INTERFACE.md): frame 0 topic, frame 1 JSON header {seq, t, ...}, frame 2 payload bytes.
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>

struct Frame {
    QString topic;
    QJsonObject hdr;
    QByteArray payload;
};
Q_DECLARE_METATYPE(Frame)

class SubThread : public QThread {
    Q_OBJECT
public:
    SubThread(const QString& addr, const QString& prefix, QObject* parent = nullptr) : QThread(parent), addr_(addr), prefix_(prefix) {}
    void stop() { stop_ = true; }
    std::atomic<long long> messages{0};
signals:
    void frame(const Frame& f);
    void gap(const QString& topic, int lost);       // sequence numbers skipped on this topic: messages lost on the way
protected:
    void run() override;
private:
    QString addr_, prefix_;
    std::atomic<bool> stop_{false};
};

// Blocking REQ/REP calls run in this object's thread; replies come back as a signal (id = the caller's number).
class CommandClient : public QObject {
    Q_OBJECT
public:
    explicit CommandClient(const QString& addr) : addr_(addr) {}
public slots:
    void call(int id, const QJsonObject& req);
signals:
    void reply(int id, const QJsonObject& hdr, const QByteArray& payload);
private:
    QString addr_;
    void* ctx_ = nullptr;
    void* sock_ = nullptr;
    void reconnect();
};
