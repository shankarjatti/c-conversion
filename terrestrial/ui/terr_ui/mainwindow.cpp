#include "mainwindow.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------- per-protocol trace colours (same as the HTML page)
static QColor traceColor(const QString& key) {
    static const QMap<QString, QString> C = {{"dmr", "#1a5f8a"}, {"nxdn", "#b02a2a"}, {"p25", "#1f7a4d"}, {"tetra", "#9a6a08"},
                                              {"nxdn48", "#7a3fa0"}, {"dstar", "#0f7f86"}, {"x2tdma", "#c2410c"}, {"provoice", "#5b6b12"}};
    return QColor(C.value(key, "#4d545e"));
}

// ---------------------------------------------------------------- audio level meter row: dot, name, bar
class MeterBar : public QWidget {
public:
    MeterBar(const QString& name, QWidget* p = nullptr) : QWidget(p), name_(name) { setFixedHeight(20); }
    void setLevel(double pct, bool live) { pct_ = pct; live_ = live; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(live_ ? QColor("#b02a2a") : QColor("#7b838e"));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(8, height() / 2.0), 3.5, 3.5);
        p.setPen(QColor("#1f2329"));
        p.setFont(QFont("DejaVu Sans", 8));
        p.drawText(QRectF(18, 0, 64, height()), Qt::AlignVCenter | Qt::AlignLeft, name_);
        QRectF bar(84, height() / 2.0 - 3, width() - 94, 6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e4e7eb"));
        p.drawRoundedRect(bar, 2, 2);
        p.setBrush(live_ ? QColor("#1f7a4d") : QColor("#7b838e"));
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), std::max(2.0, bar.width() * pct_ / 100.0), bar.height()), 2, 2);
    }
private:
    QString name_;
    double pct_ = 0;
    bool live_ = false;
};

static QString dotStyle(const char* c) { return QString("background:%1;border-radius:4px;min-width:8px;max-width:8px;min-height:8px;max-height:8px;").arg(c); }

static QTableWidget* makeTable(const QStringList& heads, QWidget* parent) {
    auto* t = new QTableWidget(0, heads.size(), parent);
    t->setHorizontalHeaderLabels(heads);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(22);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setFocusPolicy(Qt::NoFocus);
    t->setShowGrid(false);
    t->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    t->setFont(QFont("DejaVu Sans Mono", 8));
    return t;
}
static void setCell(QTableWidget* t, int r, int c, const QString& s, const QColor& col = QColor("#1f2329"), bool bold = false) {
    QTableWidgetItem* it = t->item(r, c);
    if (!it) { it = new QTableWidgetItem; t->setItem(r, c, it); }
    it->setText(s);
    it->setForeground(col);
    QFont f = it->font();
    f.setBold(bold);
    it->setFont(f);
}

MainWindow::MainWindow(const QString& subAddr, const QString& cmdAddr) : subAddr_(subAddr), cmdAddr_(cmdAddr) {
    qRegisterMetaType<Frame>("Frame");
    qRegisterMetaType<QJsonObject>("QJsonObject");
    clock_.start();
    timeBuf_.fill(0, 600);
    buildUi();
    mixer_ = new AudioMixer(this);
    connect(mixer_, &AudioMixer::failed, this, [this](const QString& why) { logLine("sys", "[GUI] audio output failed: " + why); setListen(false); });
    mixer_->start();
    sub_ = new SubThread(subAddr_, "terr/", this);
    connect(sub_, &SubThread::frame, this, &MainWindow::onFrame, Qt::QueuedConnection);
    connect(sub_, &SubThread::gap, this, &MainWindow::onGap, Qt::QueuedConnection);
    sub_->start();
    cmd_ = new CommandClient(cmdAddr_);
    cmd_->moveToThread(&cmdThread_);
    connect(cmd_, &CommandClient::reply, this, &MainWindow::onReply, Qt::QueuedConnection);
    cmdThread_.start();
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::tick);
    timer_->start(90);
    requestState();
    logLine("sys", "[GUI] subscribing to " + subAddr_ + ", commands on " + cmdAddr_);
}

MainWindow::~MainWindow() {
    if (player_) { player_->stop(); player_->wait(2000); }
    mixer_->stop();
    mixer_->setListen(false);
    mixer_->wait(2000);
    sub_->stop();
    sub_->wait(2000);
    cmdThread_.quit();
    cmdThread_.wait(9000);
    delete cmd_;
}

void MainWindow::buildUi() {
    setWindowTitle("SecureCOM — COUPLING Console (terrestrial, ZeroMQ)");
    resize(1360, 820);
    setStyleSheet(
        "QMainWindow,QWidget#root{background:#f4f5f7;} QLabel{color:#1f2329;} "
        "QPushButton{background:#ffffff;border:1px solid #d3d7dd;border-radius:3px;padding:3px 9px;font-size:11px;} "
        "QPushButton:hover{background:#eceef1;} QPushButton#primary{background:#1a5f8a;color:white;border-color:#1a5f8a;} "
        "QPushButton#danger{color:#b02a2a;} QTabBar::tab{background:#ffffff;border:1px solid #d3d7dd;padding:5px 14px;color:#4d545e;} "
        "QTabBar::tab:selected{background:#eceef1;color:#1f2329;border-bottom:2px solid #1a5f8a;} QTabWidget::pane{border:1px solid #d3d7dd;background:#ffffff;} "
        "QTableWidget{background:#ffffff;border:none;} QHeaderView::section{background:#ffffff;color:#7b838e;border:none;border-bottom:1px solid #d3d7dd;padding:3px;font-size:10px;} "
        "QPlainTextEdit{background:#ffffff;border:none;} QListWidget{background:#ffffff;border:none;}");
    auto* root = new QWidget;
    root->setObjectName("root");
    setCentralWidget(root);
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // header
    auto* head = new QWidget;
    head->setObjectName("head");
    head->setStyleSheet("QWidget#head{background:#ffffff;border-bottom:1px solid #d3d7dd;}");
    head->setFixedHeight(44);
    auto* h = new QHBoxLayout(head);
    h->setContentsMargins(14, 0, 14, 0);
    h->setSpacing(14);
    auto* tcol = new QVBoxLayout;
    tcol->setSpacing(0);
    auto* title = new QLabel("SECURECOM — COUPLING CONSOLE");
    title->setStyleSheet("font-weight:600;font-size:13px;");
    auto* sub = new QLabel("Terrestrial Radio Decoder · ZeroMQ");
    sub->setStyleSheet("color:#4d545e;font-size:10px;");
    tcol->addWidget(title);
    tcol->addWidget(sub);
    h->addLayout(tcol);
    h->addStretch(1);
    auto badge = [&](QLabel*& dot, QLabel*& lbl, const QString& text) {
        auto* w = new QWidget;
        w->setObjectName("badge");
        w->setStyleSheet("QWidget#badge{border:1px solid #d3d7dd;border-radius:3px;}");
        auto* l = new QHBoxLayout(w);
        l->setContentsMargins(8, 3, 8, 3);
        l->setSpacing(6);
        dot = new QLabel;
        dot->setStyleSheet(dotStyle("#9a6a08"));
        lbl = new QLabel(text);
        lbl->setStyleSheet("font-family:'DejaVu Sans Mono';font-size:10px;");
        l->addWidget(dot);
        l->addWidget(lbl);
        h->addWidget(w);
    };
    badge(dotSystem_, lblSystem_, "NOT CONNECTED");
    badge(dotDec_, lblDec_, "Decoder · unknown");
    btnStart_ = new QPushButton("Start decoder");
    btnStart_->setObjectName("primary");
    btnStop_ = new QPushButton("Stop decoder");
    btnStop_->setObjectName("danger");
    btnListen_ = new QPushButton("🔊 Listen: off");
    btnListen_->setToolTip("sound is only played while this is on");
    h->addWidget(btnStart_);
    h->addWidget(btnStop_);
    h->addWidget(btnListen_);
    lblClock_ = new QLabel("--:--:--");
    lblClock_->setStyleSheet("font-family:'DejaVu Sans Mono';color:#4d545e;font-size:11px;");
    h->addWidget(lblClock_);
    v->addWidget(head);

    // graphs + sidebar
    auto* graphs = new QSplitter(Qt::Vertical);
    spec_ = new SpectrumPlot;
    wf_ = new WaterfallPlot;
    time_ = new TimePlot;
    graphs->addWidget(spec_);
    graphs->addWidget(wf_);
    graphs->addWidget(time_);
    graphs->setSizes({330, 330, 100});
    graphs->setChildrenCollapsible(false);
    tabs_ = new QTabWidget;
    tabs_->setMinimumWidth(300);
    auto* sig = new QWidget;
    auto* sl = new QVBoxLayout(sig);
    sl->setContentsMargins(8, 8, 8, 8);
    signalsT_ = makeTable({"Band", "Freq MHz", "State", "RMS dBFS"}, sig);
    chanT_ = makeTable({"Ch", "Offset kHz", "Level dB"}, sig);
    decodeT_ = makeTable({"Key", "Value"}, sig);
    auto cap = [](const QString& s) { auto* l = new QLabel(s); l->setStyleSheet("color:#7b838e;font-size:10px;margin-top:8px;"); return l; };
    sl->addWidget(signalsT_, 3);
    sl->addWidget(cap("Channel activity (measured per channelizer channel)"));
    sl->addWidget(chanT_, 2);
    sl->addWidget(cap("Decoded field"));
    sl->addWidget(decodeT_, 3);
    tabs_->addTab(sig, "Signals");
    auto* rec = new QWidget;
    auto* rl = new QVBoxLayout(rec);
    rl->setContentsMargins(8, 8, 8, 8);
    auto* rb = new QHBoxLayout;
    auto* btnRef = new QPushButton("Refresh");
    auto* btnDelAll = new QPushButton("Delete all");
    btnDelAll->setObjectName("danger");
    rb->addWidget(btnRef);
    rb->addWidget(btnDelAll);
    rb->addStretch(1);
    rl->addLayout(rb);
    recList_ = new QListWidget;
    rl->addWidget(recList_, 1);
    auto* note = new QLabel("Entries are real: the backend writes a .wav whenever a protocol's decoded audio contains voice; Play streams the saved file.");
    note->setWordWrap(true);
    note->setStyleSheet("color:#7b838e;font-size:10px;");
    rl->addWidget(note);
    tabs_->addTab(rec, "Recordings");
    auto* mainSplit = new QSplitter(Qt::Horizontal);
    mainSplit->addWidget(graphs);
    mainSplit->addWidget(tabs_);
    mainSplit->setSizes({1060, 300});
    mainSplit->setChildrenCollapsible(false);

    // bottom: audio meters + console
    auto* bottom = new QSplitter(Qt::Horizontal);
    audioPanel_ = new QWidget;
    audioPanel_->setMinimumWidth(240);
    auto* al = new QVBoxLayout(audioPanel_);
    al->setContentsMargins(8, 4, 8, 4);
    al->setSpacing(0);
    auto* at = new QLabel("AUDIO");
    at->setStyleSheet("color:#7b838e;font-size:10px;font-weight:600;");
    al->addWidget(at);
    console_ = new QPlainTextEdit;
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(300);
    console_->setFont(QFont("DejaVu Sans Mono", 8));
    bottom->addWidget(audioPanel_);
    bottom->addWidget(console_);
    bottom->setSizes({300, 1060});
    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(mainSplit);
    vs->addWidget(bottom);
    vs->setSizes({620, 190});
    vs->setChildrenCollapsible(false);
    v->addWidget(vs, 1);
    lblStatus_ = new QLabel;
    lblStatus_->setStyleSheet("color:#7b838e;font-size:10px;padding:2px 10px;background:#ffffff;border-top:1px solid #d3d7dd;");
    v->addWidget(lblStatus_);

    connect(btnStart_, &QPushButton::clicked, this, [this] {
        command({{"cmd", "start_decoder"}}, [this](const QJsonObject& h, const QByteArray&) { logLine("sys", h.value("ok").toBool() ? "[GUI] decoder start requested" : "[GUI] start failed: " + h.value("error").toString()); });
    });
    connect(btnStop_, &QPushButton::clicked, this, [this] {
        command({{"cmd", "stop_decoder"}}, [this](const QJsonObject&, const QByteArray&) { logLine("sys", "[GUI] decoder stop requested"); });
    });
    connect(btnListen_, &QPushButton::clicked, this, [this] { setListen(!mixer_->listening()); });
    connect(btnRef, &QPushButton::clicked, this, [this] {
        command({{"cmd", "list_recordings"}, {"limit", 200}}, [this](const QJsonObject& h, const QByteArray&) { if (h.value("ok").toBool()) renderRecordings(h.value("items").toArray()); });
    });
    connect(btnDelAll, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Delete all", "Delete ALL recordings on disk?") == QMessageBox::Yes)
            command({{"cmd", "delete_all_recordings"}}, [this](const QJsonObject& h, const QByteArray&) { logLine("sys", QString("[GUI] deleted %1 recording(s)").arg(h.value("deleted").toInt())); });
    });
}

// A late joiner must not wait for the next periodic re-publish: ask the backend for the band list and status directly.
void MainWindow::requestState() {
    lastStateReq_ = clock_.elapsed();
    command({{"cmd", "get_state"}}, [this](const QJsonObject& h, const QByteArray&) {
        if (!h.value("ok").toBool()) return;
        setupBands(h.value("bands").toArray());
        QJsonObject st = h.value("status").toObject();
        if (!st.isEmpty()) { lastStatus_ = st; decoderRunning_ = st.value("decoder_running").toBool(); setDecoderBadge(decoderRunning_ ? 1 : 2); }
        command({{"cmd", "list_recordings"}, {"limit", 200}}, [this](const QJsonObject& r, const QByteArray&) { if (r.value("ok").toBool()) renderRecordings(r.value("items").toArray()); });
    });
}

void MainWindow::setListen(bool on) {
    mixer_->setListen(on);
    btnListen_->setText(on ? "🔊 Listen: on" : "🔊 Listen: off");
}

void MainWindow::setDecoderBadge(int state) {
    decoderState_ = state;
    dotDec_->setStyleSheet(dotStyle(state == 1 ? "#1f7a4d" : state == 2 ? "#b02a2a" : "#9a6a08"));
    lblDec_->setText(state == 1 ? "Decoder · RUNNING" : state == 2 ? "Decoder · stopped" : "Decoder · unknown");
}

void MainWindow::logLine(const QString& band, const QString& text) {
    QString b = band.isEmpty() || band == "null" ? "sys" : band;
    QColor c = b == "sys" ? QColor("#4d545e") : traceColor(b);
    QString esc = text.toHtmlEscaped();
    console_->appendHtml(QString("<span style=\"color:#7b838e\">[%1]</span> <span style=\"color:%2;white-space:pre\">%3</span>")
                             .arg(QTime::currentTime().toString("HH:mm:ss"), c.name(), esc));
}

// ---------------------------------------------------------------- commands
void MainWindow::command(const QJsonObject& req, std::function<void(const QJsonObject&, const QByteArray&)> cb) {
    int id = nextId_++;
    if (cb) pending_[id] = std::move(cb);
    QMetaObject::invokeMethod(cmd_, "call", Qt::QueuedConnection, Q_ARG(int, id), Q_ARG(QJsonObject, req));
}
void MainWindow::onReply(int id, const QJsonObject& hdr, const QByteArray& payload) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    auto cb = std::move(it->second);
    pending_.erase(it);
    cb(hdr, payload);
}

// ---------------------------------------------------------------- incoming messages (topics compared exactly)
void MainWindow::onGap(const QString& topic, int lost) { topicLost_[topic.section('/', 0, 1)] += lost; }

void MainWindow::setupBands(const QJsonArray& arr) {
    QString sig = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    if (sig == bandsSig_) return;                                  // the backend re-publishes the list every 10 s: only a real change rebuilds
    bandsSig_ = sig;
    QVector<BandState> nb;
    for (const QJsonValue& v : arr) {
        QJsonObject o = v.toObject();
        BandState b;
        b.key = o.value("key").toString();
        b.name = o.value("name").toString();
        b.freq = o.value("freq_mhz").toDouble();
        b.rate = o.value("iq_rate_sps").toDouble(200000);
        b.channels = o.value("channels").toInt(1);
        b.chanBwHz = o.value("chan_bw_hz").toDouble(b.rate);
        b.audioRate = o.value("audio_rate_hz").toInt(8000);
        b.color = traceColor(b.key);
        nb.push_back(b);
    }
    // keep live data of bands that are still there
    for (BandState& b : nb) { int i = bandIndex(b.key); if (i >= 0) { BandState old = bands_[i]; old.name = b.name; old.freq = b.freq; old.channels = b.channels; old.chanBwHz = b.chanBwHz; old.rate = b.rate; old.audioRate = b.audioRate; b = old; } }
    bands_ = nb;
    index_.clear();
    for (int i = 0; i < bands_.size(); ++i) index_[bands_[i].key] = i;
    rebuildSpectrum();
    // audio meter rows from the band list, never from a hard-coded one
    qDeleteAll(meters_);
    meters_.clear();
    for (BandState& b : bands_) {
        auto* m = new MeterBar(b.name);
        meters_[b.key] = m;
        audioPanel_->layout()->addWidget(m);
    }
}

void MainWindow::rebuildSpectrum() {
    if (bands_.isEmpty()) return;
    double lo = 1e9, hi = -1e9;
    for (auto& b : bands_) { lo = std::min(lo, b.freq - b.rate / 2e6); hi = std::max(hi, b.freq + b.rate / 2e6); }
    fmin_ = std::floor(lo - 0.9);
    fmax_ = std::ceil(hi + 0.9);
    spec_->setFullRange(fmin_, fmax_);
    wf_->setFullRange(fmin_, fmax_);
    bins_.fill(-95, 900);
    wfRows_.clear();
    QVector<QPair<QString, QColor>> leg;
    for (auto& b : bands_) leg.push_back({b.name, b.color});
    spec_->setLegend(leg);
}

void MainWindow::onFrame(const Frame& f) {
    lastFrameAt_ = clock_.elapsed();
    const QStringList p = f.topic.split('/');
    if (p.size() < 2 || p[0] != "terr") return;
    topicCount_[p.size() >= 3 ? p[0] + "/" + p[1] : f.topic]++;
    qint64 now = clock_.elapsed();
    if (p.size() == 3 && p[1] == "iq") {
        int i = bandIndex(p[2]);
        if (i < 0) return;                                                  // exact band key, e.g. "nxdn" never matches "nxdn48"
        BandState& b = bands_[i];
        int ns = f.hdr.value("n_spec").toInt(), nt = f.hdr.value("n_time").toInt(), nc = f.hdr.value("n_chan").toInt();
        if (f.payload.size() != 4 * (ns + nt + nc)) return;
        const float* d = (const float*)f.payload.constData();
        b.spec = QVector<float>(d, d + ns);
        b.tim = QVector<float>(d + ns, d + ns + nt);
        b.chan = QVector<float>(d + ns + nt, d + ns + nt + nc);
        b.active = f.hdr.value("active").toBool();
        b.rmsDb = f.hdr.value("rms_db").toDouble(-120);
        b.floorDb = f.hdr.value("floor_db").toDouble(0);
        b.best = f.hdr.value("chan_best").toInt(-1);
        b.bestDb = f.hdr.value("chan_best_db").toDouble(0);
        b.occ.clear();
        for (const QJsonValue& v : f.hdr.value("chan_occupied").toArray()) b.occ.push_back(v.toInt());
        if (f.hdr.contains("chan_bw_hz")) b.chanBwHz = f.hdr.value("chan_bw_hz").toDouble(b.chanBwHz);
        b.tIq = now;
        for (const BandState& x : bands_) if (x.active && now - x.tIq < 400) { activeBand_ = x.key; break; }
    } else if (p.size() == 3 && p[1] == "audio") {
        int i = bandIndex(p[2]);
        if (i < 0) return;
        BandState& b = bands_[i];
        b.audActive = f.hdr.value("active").toBool();
        b.audRms = f.hdr.value("rms_db").toDouble(-120);
        b.tAud = now;
        b.audMsgs++;
        int rate = f.hdr.value("rate").toInt(b.audioRate), n = f.hdr.value("n").toInt();
        if (b.audActive) { b.audMsgsActive++; b.audVoiceSec += rate ? (double)n / rate : 0; }
        if (n * 2 == f.payload.size()) mixer_->push(b.key, rate, f.payload);
    } else if (f.topic == "terr/bands") {
        setupBands(f.hdr.value("bands").toArray());
    } else if (f.topic == "terr/status") {
        lastStatus_ = f.hdr;
        lastStatusAt_ = now;
        decoderRunning_ = f.hdr.value("decoder_running").toBool();
        setDecoderBadge(decoderRunning_ ? 1 : 2);
    } else if (f.topic == "terr/decoder") {
        decoderRunning_ = f.hdr.value("running").toBool();
        setDecoderBadge(decoderRunning_ ? 1 : 2);
    } else if (f.topic == "terr/log") {
        logLine(f.hdr.value("band").toString(), f.hdr.value("text").toString());
    } else if (f.topic == "terr/recording") {
        command({{"cmd", "list_recordings"}, {"limit", 200}}, [this](const QJsonObject& h, const QByteArray&) { if (h.value("ok").toBool()) renderRecordings(h.value("items").toArray()); });
    } else if (f.topic == "terr/recordings") {
        renderRecordings(f.hdr.value("items").toArray());
    } else if (f.topic == "terr/tetra") {
        lastTetra_ = f.hdr.value("data").toObject();
        lastTetraAt_ = now;
    }
}

// ---------------------------------------------------------------- the 90 ms refresh
void MainWindow::tick() {
    qint64 now = clock_.elapsed();
    lblClock_->setText(QTime::currentTime().toString("HH:mm:ss"));
    bool live = lastFrameAt_ && now - lastFrameAt_ < 5000;
    dotSystem_->setStyleSheet(dotStyle(live ? "#1f7a4d" : "#9a6a08"));
    QString lab = live ? "LIVE — real IQ + real decode/audio" : "NOT CONNECTED";
    if (lab != lblSystem_->text() && qEnvironmentVariableIsSet("SECURECOM_UI_TRACE"))
        std::printf("[ui %6.1fs] %s\n", clock_.elapsed() / 1000.0, lab.toUtf8().constData()), std::fflush(stdout);
    lblSystem_->setText(lab);
    if (!live) setDecoderBadge(0);
    if (bands_.isEmpty() && now - lastStateReq_ > 2000) requestState();
    if (bands_.isEmpty() || !live) {
        signalsT_->setRowCount(0);
        if (!live) {
            decodeT_->setRowCount(1);
            setCell(decodeT_, 0, 0, "backend not connected — no data source", QColor("#7b838e"));
            decodeT_->setSpan(0, 0, 1, 2);
        }
        for (auto* m : meters_) m->setLevel(0, false);
        lblStatus_->setText(QString("waiting for %1").arg(subAddr_));
        return;
    }

    // scatter each band's real FFT into the shared spectrum (frequency from the band list)
    int NB = bins_.size();
    for (const BandState& b : bands_) {
        if (b.spec.isEmpty()) continue;
        int n = b.spec.size();
        double half = b.rate / 2e6;
        for (int i = 0; i < n; ++i) {
            double f = b.freq - half + (double)i / n * half * 2;
            int idx = (int)std::lround((f - fmin_) / (fmax_ - fmin_) * NB);
            if (idx >= 0 && idx < NB) bins_[idx] = b.spec[i];
        }
    }
    wfRows_.prepend(bins_);
    if (wfRows_.size() > 160) wfRows_.removeLast();
    int ai = bandIndex(activeBand_);
    if (ai >= 0 && !bands_[ai].tim.isEmpty() && now - bands_[ai].tIq < 400) {
        const QVector<float>& t = bands_[ai].tim;
        int n = timeBuf_.size(), k = t.size();
        for (int i = 0; i + k < n; ++i) timeBuf_[i] = timeBuf_[i + k];
        for (int i = 0; i < k; ++i) timeBuf_[n - k + i] = t[i];
    }
    // carrier markers from bands active in the last 2.5 s
    QVector<Carrier> carriers;
    for (const BandState& b : bands_) {
        if (b.chan.isEmpty() || now - b.tIq > 2500) continue;
        int n = b.chan.size();
        QVector<float> s(b.chan);
        std::sort(s.begin(), s.end());
        double med = s[n / 2], bw = b.chanBwHz / 1000.0;
        for (int i = 0; i < n; ++i) carriers.push_back({b.freq + (i <= n / 2 ? i : i - n) * bw / 1000.0, i, b.chan[i] - med, b.occ.contains(i)});
    }
    spec_->setData(bins_, carriers);
    spec_->setReadout(QString("span %1 MHz").arg(spec_->viewMax() - spec_->viewMin(), 0, 'f', 3));
    wf_->setRows(&wfRows_, 160);
    wf_->setReadout("last 30s");
    time_->setData(timeBuf_, ai >= 0 ? bands_[ai].color : QColor("#1a5f8a"), ai >= 0 ? activeBand_ : QString());
    updateSignalTables();
    for (BandState& b : bands_) {
        MeterBar* m = meters_.value(b.key);
        bool fresh = now - b.tAud < 600;
        if (m) m->setLevel(fresh ? std::min(100.0, std::max(0.0, (b.audRms + 90) * 2.2)) : 0, fresh && b.audActive);
    }
    long long lost = 0;
    for (auto v : topicLost_) lost += v;
    QString st = QString("%1 messages received").arg(sub_->messages.load());
    st += lost ? QString(" · %1 lost (sequence gaps)").arg(lost) : QString(" · no sequence gaps");
    if (lastStatus_.contains("iq_connected")) {
        int c = 0, t = 0;
        for (auto v : lastStatus_.value("iq_connected").toObject()) { ++t; c += v.toBool(); }
        st += QString(" · IQ arriving on %1/%2 bands").arg(c).arg(t);
    }
    lblStatus_->setText(st);
}

void MainWindow::updateSignalTables() {
    qint64 now = clock_.elapsed();
    signalsT_->setRowCount(bands_.size());
    QString decodingBand, actKey;
    for (int r = 0; r < bands_.size(); ++r) {
        BandState& b = bands_[r];
        bool rf = b.active && now - b.tIq < 400, dec = decoderRunning_ && b.audActive && now - b.tAud < 700;
        if (dec) decodingBand = b.key;
        if (rf) ++b.ticksRf;
        if (dec) ++b.ticksDecoding;
        QString state = dec ? "DECODING" : rf ? "ACTIVE" : "idle";
        QColor col = dec ? QColor("#9a6a08") : rf ? QColor("#1f7a4d") : QColor("#7b838e");
        bool has = !b.spec.isEmpty();
        setCell(signalsT_, r, 0, b.name, b.color, true);
        setCell(signalsT_, r, 1, QString::number(b.freq, 'f', 5));
        setCell(signalsT_, r, 2, state, col, dec);
        setCell(signalsT_, r, 3, dec ? QString::number(b.audRms, 'f', 1) : has ? QString::number(b.rmsDb, 'f', 1) : "—");
    }
    for (const BandState& b : bands_) if (b.active && now - b.tIq < 500) { actKey = b.key; break; }
    // per-channel activity of the band that is active right now: the 6 strongest channels
    int ci = bandIndex(actKey);
    if (ci >= 0 && !bands_[ci].chan.isEmpty()) {
        const BandState& b = bands_[ci];
        int n = b.chan.size();
        QVector<float> s(b.chan);
        std::sort(s.begin(), s.end());
        double med = s[n / 2], bw = b.chanBwHz / 1000.0;
        QVector<int> order(n);
        for (int i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int x, int y) { return b.chan[x] > b.chan[y]; });
        int rows = std::min(6, n);
        chanT_->clearSpans();
        chanT_->setRowCount(rows);
        for (int k = 0; k < rows; ++k) {
            int i = order[k];
            bool occ = b.occ.contains(i);
            double off = (i <= n / 2 ? i : i - n) * bw, rel = b.chan[i] - med;
            QColor c = occ ? QColor("#1f7a4d") : QColor("#7b838e");
            setCell(chanT_, k, 0, QString::number(i) + (occ ? " ●" : ""), c);
            setCell(chanT_, k, 1, (off >= 0 ? "+" : "") + QString::number(off, 'f', 2));
            setCell(chanT_, k, 2, (rel > 0 ? "+" : "") + QString::number(rel, 'f', 1), c);
        }
    } else {
        chanT_->setRowCount(1);
        chanT_->setSpan(0, 0, 1, 3);
        setCell(chanT_, 0, 0, "no band active right now", QColor("#7b838e"));
    }
    // decoded field: TETRA call log if recent, else what is known about the active/decoding band
    decodeT_->clearSpans();
    if (!decoderRunning_) {
        decodeT_->setRowCount(1);
        decodeT_->setSpan(0, 0, 1, 2);
        setCell(decodeT_, 0, 0, "decoder not running — use Start decoder in the header", QColor("#7b838e"));
    } else if (!lastTetra_.isEmpty() && now - lastTetraAt_ < 8000) {
        decodeT_->setRowCount(lastTetra_.size());
        int r = 0;
        for (auto it = lastTetra_.begin(); it != lastTetra_.end(); ++it, ++r) {
            QJsonValue v = it.value();
            setCell(decodeT_, r, 0, it.key());
            setCell(decodeT_, r, 1, v.isString() ? v.toString() : v.isBool() ? (v.toBool() ? "True" : "False") : v.isDouble() ? QString::number(v.toDouble(), 'g', 12) : QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)));
        }
    } else if (!decodingBand.isEmpty() || !actKey.isEmpty()) {
        QString k = !decodingBand.isEmpty() ? decodingBand : actKey;
        const BandState& b = bands_[bandIndex(k)];
        int n = std::max(1, (int)b.chan.size());
        double bw = b.chanBwHz / 1000.0;
        int ch = b.best;
        double off = ch >= 0 ? (ch <= n / 2 ? ch : ch - n) * bw : 0;
        bool isDec = decodingBand == k;
        QString occ;
        for (int o : b.occ) occ += (occ.isEmpty() ? "" : ", ") + QString::number(o);
        QList<QPair<QString, QString>> rows = {
            {"Protocol", b.name}, {"Band centre", QString::number(b.freq, 'f', 5) + " MHz"},
            {"Channel", ch >= 0 ? QString("%1 of %2").arg(ch).arg(n) : "—"},
            {"Ch offset", ch >= 0 ? (off >= 0 ? "+" : "") + QString::number(off, 'f', 2) + " kHz" : "—"},
            {"Ch frequency", ch >= 0 ? QString::number(b.freq + off / 1000, 'f', 5) + " MHz" : "—"},
            {"Ch SNR", ch >= 0 ? "+" + QString::number(b.bestDb, 'f', 1) + " dB" : "—"},
            {"Occupied ch", occ.isEmpty() ? "—" : occ}, {"RF level", QString::number(b.rmsDb, 'f', 1) + " dBFS"},
            {"Audio", isDec ? "DECODING (" + QString::number(b.audRms, 'f', 1) + " dBFS)" : "no decoded audio"},
            {"Talkgroup/SSI", k == "tetra" ? "via TETRA call_log" : "unavailable — dsd_block exposes no metadata port"}};
        decodeT_->setRowCount(rows.size());
        for (int r = 0; r < rows.size(); ++r) {
            setCell(decodeT_, r, 0, rows[r].first);
            setCell(decodeT_, r, 1, rows[r].second, r == 0 ? b.color : (r == 8 && isDec) ? QColor("#1f7a4d") : QColor("#1f2329"));
        }
    } else {
        decodeT_->setRowCount(1);
        decodeT_->setSpan(0, 0, 1, 2);
        setCell(decodeT_, 0, 0, "no active decode", QColor("#7b838e"));
    }
}

// ---------------------------------------------------------------- recordings
void MainWindow::fetchRecording(const QString& file, std::function<void(const QByteArray&)> done) {
    auto data = std::make_shared<QByteArray>();
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, file, data, step, done]() {
        command({{"cmd", "get_recording"}, {"file", file}, {"offset", (double)data->size()}, {"length", 262144}},
                [this, file, data, step, done](const QJsonObject& h, const QByteArray& payload) {
                    if (!h.value("ok").toBool()) { logLine("sys", "[GUI] download failed: " + h.value("error").toString()); return; }
                    data->append(payload);
                    if (h.value("eof").toBool() || payload.isEmpty()) done(*data);
                    else (*step)();
                });
    };
    (*step)();
}

void MainWindow::playRecording(const QString& file, QPushButton* btn, QLabel* timeLbl) {
    if (player_ && playingFile_ == file) {                              // toggle pause on the same recording
        player_->pause(!player_->paused());
        btn->setText(player_->paused() ? "▶" : "⏸");
        return;
    }
    if (player_) { player_->stop(); player_->wait(1500); player_->deleteLater(); player_ = nullptr; if (playBtn_) playBtn_->setText("▶"); if (playTime_) playTime_->clear(); }
    playingFile_ = file;
    playBtn_ = btn;
    playTime_ = timeLbl;
    btn->setText("…");
    fetchRecording(file, [this, file, btn, timeLbl](const QByteArray& wav) {
        auto* pl = new WavPlayer(wav, this);
        if (!pl->parse()) { logLine("sys", "[GUI] cannot play " + file + ": not a 16-bit mono WAV"); delete pl; btn->setText("▶"); return; }
        player_ = pl;
        double total = pl->duration();
        auto fmt = [](double s) { int i = (int)s; return QString("%1:%2").arg(i / 60).arg(i % 60, 2, 10, QChar('0')); };
        connect(pl, &WavPlayer::position, this, [timeLbl, fmt, total](double s) { timeLbl->setText(fmt(s) + " / " + fmt(total)); });
        connect(pl, &WavPlayer::finished2, this, [this, btn, timeLbl, pl](const QString& err) {
            btn->setText("▶");
            timeLbl->clear();
            if (!err.isEmpty()) logLine("sys", "[GUI] playback failed: " + err);
            if (player_ == pl) { player_ = nullptr; playingFile_.clear(); }
            pl->deleteLater();
        });
        pl->start();
        btn->setText("⏸");
    });
}

void MainWindow::renderRecordings(const QJsonArray& items) {
    QString sig = QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Compact)) + "|" + playingFile_;
    if (sig == recSig_) return;                                         // unchanged: keep the rows (and a playing one) as they are
    recSig_ = sig;
    recItems_ = items;
    recList_->clear();
    playBtn_ = nullptr;
    playTime_ = nullptr;
    if (items.isEmpty()) { recList_->addItem("no recordings on disk"); return; }
    for (const QJsonValue& v : items) {
        QJsonObject it = v.toObject();
        QString file = it.value("file").toString();
        auto* w = new QWidget;
        auto* l = new QHBoxLayout(w);
        l->setContentsMargins(2, 2, 2, 2);
        l->setSpacing(2);
        auto* meta = new QLabel(QString("<b style='font-size:9px'>%1</b><br><span style='color:#7b838e;font-size:9px'>%2 · %3 · %4 · %5 KB</span>")
                                    .arg(file, QDateTime::fromSecsSinceEpoch((qint64)it.value("mtime").toDouble()).toString("HH:mm:ss"), it.value("band").toString().toUpper(),
                                         it.value("duration").isNull() ? "?" : QString::number(it.value("duration").toDouble()) + "s", QString::number(it.value("bytes").toDouble() / 1024, 'f', 0)));
        meta->setMinimumWidth(0);
        meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        l->addWidget(meta, 1);
        auto* play = new QPushButton(playingFile_ == file && player_ ? "⏸" : "▶");
        auto* tl = new QLabel;
        tl->setStyleSheet("font-family:'DejaVu Sans Mono';font-size:9px;color:#7b838e;");
        auto* dl = new QPushButton("↓");
        auto* del = new QPushButton("✕");
        del->setObjectName("danger");
        for (auto* b : {play, dl, del}) { b->setFixedWidth(22); b->setStyleSheet("padding:2px;"); }
        l->addWidget(play);
        l->addWidget(tl);
        l->addWidget(dl);
        l->addWidget(del);
        if (playingFile_ == file && player_) { playBtn_ = play; playTime_ = tl; }
        connect(play, &QPushButton::clicked, this, [this, file, play, tl] { playRecording(file, play, tl); });
        connect(dl, &QPushButton::clicked, this, [this, file] {
            QString path = QFileDialog::getSaveFileName(this, "Save recording", file);
            if (path.isEmpty()) return;
            fetchRecording(file, [this, path](const QByteArray& d) { QFile f(path); if (f.open(QIODevice::WriteOnly)) { f.write(d); logLine("sys", "[GUI] saved " + path); } });
        });
        connect(del, &QPushButton::clicked, this, [this, file] {
            if (QMessageBox::question(this, "Delete", "Delete " + file + " ?") == QMessageBox::Yes)
                command({{"cmd", "delete_recording"}, {"file", file}}, [this](const QJsonObject&, const QByteArray&) {});
        });
        auto* li = new QListWidgetItem(recList_);
        li->setSizeHint(QSize(0, 40));
        recList_->setItemWidget(li, w);
    }
}

// ---------------------------------------------------------------- test support
QJsonObject MainWindow::summary() const {
    QJsonObject o;
    QJsonObject topics, lost;
    for (auto it = topicCount_.begin(); it != topicCount_.end(); ++it) topics[it.key()] = (double)it.value();
    for (auto it = topicLost_.begin(); it != topicLost_.end(); ++it) lost[it.key()] = (double)it.value();
    o["messages"] = (double)sub_->messages.load();
    o["topics"] = topics;
    o["lost"] = lost;
    o["decoder_running"] = decoderRunning_;
    o["system_label"] = lblSystem_->text();
    QJsonArray bands;
    for (const BandState& b : bands_) {
        QJsonObject x;
        x["key"] = b.key;
        x["name"] = b.name;
        x["freq_mhz"] = b.freq;
        x["rms_db"] = b.rmsDb;
        x["ticks_rf_active"] = (double)b.ticksRf;
        x["ticks_decoding"] = (double)b.ticksDecoding;
        x["audio_msgs"] = (double)b.audMsgs;
        x["audio_voice_seconds"] = b.audVoiceSec;
        bands.push_back(x);
    }
    o["bands"] = bands;
    o["recordings_rows"] = recList_->count();
    o["console_lines"] = console_->document()->blockCount();
    o["tests"] = testResults_;
    return o;
}

QPixmap MainWindow::screenshot() { return grab(); }

// self-test of the zoom interaction on the spectrum: drag-select, wheel scale, double-click reset (events sent like a real mouse)
void MainWindow::runZoomTest() {
    ZoomPlot* z = spec_;
    QJsonObject r;
    r["full"] = QString("%1..%2").arg(z->viewMin()).arg(z->viewMax());
    auto mouse = [&](QEvent::Type t, QPointF pos, Qt::MouseButton b) {
        QMouseEvent e(t, pos, z->mapToGlobal(pos.toPoint()), b, b, Qt::NoModifier);
        QApplication::sendEvent(z, &e);
    };
    double w = z->width();
    mouse(QEvent::MouseButtonPress, QPointF(0.30 * w, 100), Qt::LeftButton);
    mouse(QEvent::MouseMove, QPointF(0.60 * w, 100), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, QPointF(0.60 * w, 100), Qt::LeftButton);
    r["after_drag_30_60_percent"] = QString("%1..%2").arg(z->viewMin(), 0, 'f', 3).arg(z->viewMax(), 0, 'f', 3);
    QWheelEvent we(QPointF(0.5 * w, 100), z->mapToGlobal(QPoint((int)(0.5 * w), 100)), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(z, &we);
    r["after_wheel_in"] = QString("%1..%2").arg(z->viewMin(), 0, 'f', 3).arg(z->viewMax(), 0, 'f', 3);
    QMouseEvent de(QEvent::MouseButtonDblClick, QPointF(0.5 * w, 100), z->mapToGlobal(QPoint((int)(0.5 * w), 100)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(z, &de);
    r["after_double_click"] = QString("%1..%2").arg(z->viewMin()).arg(z->viewMax());
    testResults_["zoom"] = r;
}
void MainWindow::showTab(int i) { tabs_->setCurrentIndex(i); }

void MainWindow::runRecordingsTest(const QString& deleteFile) {
    if (recItems_.isEmpty()) { testResults_["recordings_test"] = "no recordings to test with"; return; }
    QJsonObject first = recItems_.at(recItems_.size() - 1).toObject();       // the oldest: certainly closed
    QString file = first.value("file").toString();
    qint64 bytes = (qint64)first.value("bytes").toDouble();
    fetchRecording(file, [this, file, bytes, deleteFile](const QByteArray& d) {
        WavPlayer pl(d);
        bool wav = pl.parse();
        testResults_["fetch_file"] = file;
        testResults_["fetch_bytes"] = (double)d.size();
        testResults_["fetch_matches_listing"] = (qint64)d.size() == bytes;
        testResults_["fetch_is_wav"] = wav;
        testResults_["fetch_wav_seconds"] = wav ? pl.duration() : 0.0;
        if (deleteFile.isEmpty()) return;
        command({{"cmd", "delete_recording"}, {"file", deleteFile}}, [this](const QJsonObject& h, const QByteArray&) {
            testResults_["delete_ok"] = h.value("ok").toBool() && h.value("deleted").toBool();
        });
    });
}
