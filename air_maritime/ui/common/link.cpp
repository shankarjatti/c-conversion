#include "link.h"

#include <QJsonDocument>
#include <zmq.h>

void SubThread::run() {
    void* ctx = zmq_ctx_new();
    void* sub = zmq_socket(ctx, ZMQ_SUB);
    int hwm = 100000, linger = 0, to = 200;
    zmq_setsockopt(sub, ZMQ_RCVHWM, &hwm, sizeof hwm);
    zmq_setsockopt(sub, ZMQ_LINGER, &linger, sizeof linger);
    zmq_setsockopt(sub, ZMQ_RCVTIMEO, &to, sizeof to);
    QByteArray pre = prefix_.toUtf8();
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, pre.constData(), pre.size());
    zmq_connect(sub, addr_.toUtf8().constData());       // ZeroMQ reconnects by itself when the backend restarts
    QMap<QString, long long> last;
    while (!stop_) {
        zmq_msg_t m[3];
        int n = 0;
        bool ok = true;
        for (; n < 8; ++n) {
            zmq_msg_t part;
            zmq_msg_init(&part);
            if (zmq_msg_recv(&part, sub, 0) < 0) { zmq_msg_close(&part); ok = false; break; }
            int more = zmq_msg_more(&part);
            if (n < 3) { zmq_msg_init(&m[n]); zmq_msg_copy(&m[n], &part); }
            zmq_msg_close(&part);
            if (!more) { ++n; break; }
        }
        if (!ok) { for (int i = 0; i < std::min(n, 3); ++i) zmq_msg_close(&m[i]); continue; }
        if (n == 3) {
            Frame f;
            f.topic = QString::fromUtf8((const char*)zmq_msg_data(&m[0]), (int)zmq_msg_size(&m[0]));   // compared EXACTLY downstream (prefix trap)
            QJsonParseError err;
            f.hdr = QJsonDocument::fromJson(QByteArray((const char*)zmq_msg_data(&m[1]), (int)zmq_msg_size(&m[1])), &err).object();
            if (err.error == QJsonParseError::NoError) {
                f.payload = QByteArray((const char*)zmq_msg_data(&m[2]), (int)zmq_msg_size(&m[2]));
                long long seq = (long long)f.hdr.value("seq").toDouble(-1);
                if (last.contains(f.topic) && seq > last[f.topic] + 1) emit gap(f.topic, (int)(seq - last[f.topic] - 1));
                last[f.topic] = seq;
                ++messages;
                emit frame(f);
            }
        }
        for (int i = 0; i < std::min(n, 3); ++i) zmq_msg_close(&m[i]);
    }
    zmq_close(sub);
    zmq_ctx_term(ctx);
}

void CommandClient::reconnect() {
    if (sock_) zmq_close(sock_);
    if (!ctx_) ctx_ = zmq_ctx_new();
    sock_ = zmq_socket(ctx_, ZMQ_REQ);
    int to = 8000, linger = 0;
    zmq_setsockopt(sock_, ZMQ_RCVTIMEO, &to, sizeof to);
    zmq_setsockopt(sock_, ZMQ_SNDTIMEO, &to, sizeof to);
    zmq_setsockopt(sock_, ZMQ_LINGER, &linger, sizeof linger);
    zmq_connect(sock_, addr_.toUtf8().constData());
}

void CommandClient::call(int id, const QJsonObject& req) {
    if (!sock_) reconnect();
    QByteArray b = QJsonDocument(req).toJson(QJsonDocument::Compact);
    QJsonObject hdr{{"ok", false}, {"error", "no reply from the backend (timeout)"}};
    QByteArray payload;
    if (zmq_send(sock_, b.constData(), b.size(), 0) >= 0) {
        zmq_msg_t m;
        zmq_msg_init(&m);
        if (zmq_msg_recv(&m, sock_, 0) >= 0) {
            hdr = QJsonDocument::fromJson(QByteArray((const char*)zmq_msg_data(&m), (int)zmq_msg_size(&m))).object();
            int more = zmq_msg_more(&m);
            zmq_msg_close(&m);
            if (more) {
                zmq_msg_init(&m);
                if (zmq_msg_recv(&m, sock_, 0) >= 0) payload = QByteArray((const char*)zmq_msg_data(&m), (int)zmq_msg_size(&m));
                zmq_msg_close(&m);
            }
        } else {
            zmq_msg_close(&m);
            reconnect();                                   // a REQ socket that timed out is unusable until recreated
        }
    } else {
        reconnect();
    }
    emit reply(id, hdr, payload);
}
