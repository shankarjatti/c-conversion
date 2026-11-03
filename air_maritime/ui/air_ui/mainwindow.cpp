#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

static QString dotStyle(const char* c) { return QString("background:%1;border-radius:4px;min-width:8px;max-width:8px;min-height:8px;max-height:8px;").arg(c); }

static QTableWidget* makeTable(const QStringList& heads, QWidget* parent) {
    auto* t = new QTableWidget(0, heads.size(), parent);
    t->setHorizontalHeaderLabels(heads);
    t->verticalHeader()->setVisible(false);
    t->verticalHeader()->setDefaultSectionSize(22);
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::SingleSelection);
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

QString MainWindow::num(const QJsonValue& v, int dec) {
    if (!v.isDouble()) return "—";
    double d = v.toDouble();
    return d == std::floor(d) ? QString::number((long long)d) : QString::number(d, 'f', dec);
}

MainWindow::MainWindow(const QString& subAddr, const QString& cmdAddr, const QString& mbtiles) : subAddr_(subAddr), cmdAddr_(cmdAddr) {
    qRegisterMetaType<Frame>("Frame");
    qRegisterMetaType<QJsonObject>("QJsonObject");
    clock_.start();
    tiles_ = new TileStore(mbtiles);
    buildUi();
    sub_ = new SubThread(subAddr_, "air/", this);
    connect(sub_, &SubThread::frame, this, &MainWindow::onFrame, Qt::QueuedConnection);
    connect(sub_, &SubThread::gap, this, &MainWindow::onGap, Qt::QueuedConnection);
    sub_->start();
    cmd_ = new CommandClient(cmdAddr_);
    cmd_->moveToThread(&cmdThread_);
    connect(cmd_, &CommandClient::reply, this, &MainWindow::onReply, Qt::QueuedConnection);
    cmdThread_.start();
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::tick);
    timer_->start(100);
    requestState();
    logLine("sys", "subscribing to " + subAddr_ + ", commands on " + cmdAddr_ + (tiles_->ok() ? "" : "; map tiles unavailable: " + tiles_->error()));
}

MainWindow::~MainWindow() {
    sub_->stop();
    sub_->wait(2000);
    cmdThread_.quit();
    cmdThread_.wait(9000);
    delete cmd_;
    delete tiles_;
}

void MainWindow::buildUi() {
    setWindowTitle("SecureCOM — Air & Maritime Console (ZeroMQ)");
    resize(1400, 860);
    setStyleSheet(
        "QMainWindow,QWidget#root{background:#f4f5f7;} QLabel{color:#1f2329;} "
        "QPushButton{background:#ffffff;border:1px solid #d3d7dd;border-radius:3px;padding:3px 9px;font-size:11px;} QPushButton:hover{background:#eceef1;} "
        "QPushButton#primary{background:#1a5f8a;color:white;border-color:#1a5f8a;} QPushButton#danger{color:#b02a2a;} "
        "QTabBar::tab{background:#ffffff;border:1px solid #d3d7dd;padding:5px 14px;color:#4d545e;} "
        "QTabBar::tab:selected{background:#eceef1;color:#1f2329;border-bottom:2px solid #1a5f8a;} QTabWidget::pane{border:1px solid #d3d7dd;background:#ffffff;} "
        "QTableWidget{background:#ffffff;border:none;} QHeaderView::section{background:#ffffff;color:#7b838e;border:none;border-bottom:1px solid #d3d7dd;padding:3px;font-size:10px;} "
        "QPlainTextEdit{background:#ffffff;border:none;}");
    auto* root = new QWidget;
    root->setObjectName("root");
    setCentralWidget(root);
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* head = new QWidget;
    head->setObjectName("head");
    head->setStyleSheet("QWidget#head{background:#ffffff;border-bottom:1px solid #d3d7dd;}");
    head->setFixedHeight(44);
    auto* h = new QHBoxLayout(head);
    h->setContentsMargins(14, 0, 14, 0);
    h->setSpacing(12);
    auto* tcol = new QVBoxLayout;
    tcol->setSpacing(0);
    auto* title = new QLabel("SECURECOM — AIR & MARITIME CONSOLE");
    title->setStyleSheet("font-weight:600;font-size:13px;");
    auto* sub = new QLabel("AIS 162 MHz · ADS-B 1090 MHz · ZeroMQ");
    sub->setStyleSheet("color:#4d545e;font-size:10px;");
    tcol->addWidget(title);
    tcol->addWidget(sub);
    h->addLayout(tcol);
    h->addStretch(1);
    auto badge = [&](QLabel*& dot, QLabel*& lbl, const QString& text, bool withDot) {
        auto* w = new QWidget;
        w->setObjectName("badge");
        w->setStyleSheet("QWidget#badge{border:1px solid #d3d7dd;border-radius:3px;}");
        auto* l = new QHBoxLayout(w);
        l->setContentsMargins(8, 3, 8, 3);
        l->setSpacing(6);
        dot = nullptr;
        if (withDot) { dot = new QLabel; dot->setStyleSheet(dotStyle("#9a6a08")); l->addWidget(dot); }
        lbl = new QLabel(text);
        lbl->setStyleSheet("font-family:'DejaVu Sans Mono';font-size:10px;");
        l->addWidget(lbl);
        h->addWidget(w);
    };
    QLabel* unused;
    badge(dotSystem_, lblSystem_, "NOT CONNECTED", true);
    badge(dotDec_, lblDec_, "Decoder · unknown", true);
    badge(unused, lblShips_, "🚢 0", false);
    badge(unused, lblPlanes_, "✈ 0", false);
    auto* bStart = new QPushButton("Start decoder");
    bStart->setObjectName("primary");
    auto* bStop = new QPushButton("Stop decoder");
    bStop->setObjectName("danger");
    h->addWidget(bStart);
    h->addWidget(bStop);
    lblClock_ = new QLabel("--:--:-- UTC");
    lblClock_->setStyleSheet("font-family:'DejaVu Sans Mono';color:#4d545e;font-size:11px;");
    h->addWidget(lblClock_);
    v->addWidget(head);

    // left: spectrum, map, footer strip
    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(6, 6, 6, 0);
    lv->setSpacing(4);
    auto* specTitle = new QLabel("📶 RF SPECTRUM — AIS / ADS-B");
    specTitle->setStyleSheet("font-weight:600;font-size:10px;color:#4d545e;");
    spec_ = new AirSpectrum;
    spec_->setFixedHeight(230);
    lv->addWidget(specTitle);
    lv->addWidget(spec_);
    auto* mapHead = new QWidget;
    auto* mh = new QHBoxLayout(mapHead);
    mh->setContentsMargins(0, 0, 0, 0);
    auto* mapTitle = new QLabel("🌐 UNIFIED AIR & MARITIME TACTICAL MAP");
    mapTitle->setStyleSheet("font-weight:600;font-size:10px;color:#4d545e;");
    chkShips_ = new QCheckBox("Show Ships (AIS 162M)");
    chkShips_->setChecked(true);
    chkShips_->setStyleSheet("color:#2563eb;font-size:10px;");
    chkPlanes_ = new QCheckBox("Show Aircraft (ADS-B 1090M)");
    chkPlanes_->setChecked(true);
    chkPlanes_->setStyleSheet("color:#b45309;font-size:10px;");
    mh->addWidget(mapTitle);
    mh->addStretch(1);
    mh->addWidget(chkShips_);
    mh->addWidget(chkPlanes_);
    lv->addWidget(mapHead);
    map_ = new MapWidget(tiles_);
    lv->addWidget(map_, 1);
    auto* foot = new QWidget;
    auto* fh = new QHBoxLayout(foot);
    fh->setContentsMargins(0, 0, 0, 2);
    fh->setSpacing(6);
    auto cl = [](const QString& s) { auto* l = new QLabel(s); l->setStyleSheet("color:#7b838e;font-size:10px;"); return l; };
    fh->addWidget(cl("Avg:"));
    spinAvg_ = new QSpinBox;
    spinAvg_->setRange(1, 200);
    spinAvg_->setValue(10);
    spinAvg_->setToolTip("average trace: running mean over this many 0.1 s updates");
    fh->addWidget(spinAvg_);
    lblPeak_ = new QLabel("Peak / noise floor: —");
    lblPeak_->setStyleSheet("font-family:'DejaVu Sans Mono';font-size:10px;");
    fh->addWidget(lblPeak_);
    auto* bAuto = new QPushButton("Auto Scale");
    chkHold_ = new QCheckBox("MaxHold");
    chkHold_->setToolTip("hold the highest peak seen per bin (orange); untick to clear");
    fh->addWidget(bAuto);
    fh->addWidget(chkHold_);
    fh->addWidget(cl("━ avg  ━ peak/0.1 s  ━ max hold"));
    fh->addStretch(1);
    lv->addWidget(foot);

    // right: tabs
    tabs_ = new QTabWidget;
    tabs_->setMinimumWidth(360);
    auto* sig = new QWidget;
    auto* sl = new QVBoxLayout(sig);
    sl->setContentsMargins(8, 8, 8, 8);
    auto cap = [](const QString& s) { auto* l = new QLabel(s); l->setStyleSheet("color:#7b838e;font-size:10px;margin-top:6px;"); return l; };
    signalsT_ = makeTable({"Band", "Freq MHz", "Last decode", "Targets"}, sig);
    signalsT_->setMaximumHeight(80);
    targetsT_ = makeTable({"ID / MMSI", "Symbol & Type", "Speed", "Heading"}, sig);
    sl->addWidget(cap("Protocol Band Signals"));
    sl->addWidget(signalsT_);
    sl->addWidget(cap("Live Tracked Targets (click a row to centre the map on it)"));
    sl->addWidget(targetsT_, 1);
    tabs_->addTab(sig, "Signals");
    auto* js = new QWidget;
    auto* jl = new QVBoxLayout(js);
    jl->setContentsMargins(8, 8, 8, 8);
    auto* jb = new QHBoxLayout;
    auto* bPause = new QPushButton("Pause Feed");
    bPause->setObjectName("primary");
    auto* bClear = new QPushButton("Clear Buffer");
    auto* bCopy = new QPushButton("Copy JSON");
    jb->addWidget(bPause);
    jb->addWidget(bClear);
    jb->addWidget(bCopy);
    jb->addStretch(1);
    jl->addLayout(jb);
    json_ = new QPlainTextEdit;
    json_->setReadOnly(true);
    json_->setFont(QFont("DejaVu Sans Mono", 7));
    json_->setMaximumBlockCount(1500);
    json_->setPlainText("// Waiting for live ZeroMQ JSON data...");
    jl->addWidget(json_, 1);
    tabs_->addTab(js, "ZMQ JSON Feed");

    auto* main = new QSplitter(Qt::Horizontal);
    main->addWidget(left);
    main->addWidget(tabs_);
    main->setSizes({1000, 400});
    main->setChildrenCollapsible(false);
    console_ = new QPlainTextEdit;
    console_->setReadOnly(true);
    console_->setMaximumBlockCount(300);
    console_->setFont(QFont("DejaVu Sans Mono", 8));
    auto* vs = new QSplitter(Qt::Vertical);
    vs->addWidget(main);
    vs->addWidget(console_);
    vs->setSizes({690, 120});
    vs->setChildrenCollapsible(false);
    v->addWidget(vs, 1);
    lblStatus_ = new QLabel;
    lblStatus_->setStyleSheet("color:#7b838e;font-size:10px;padding:2px 10px;background:#ffffff;border-top:1px solid #d3d7dd;");
    v->addWidget(lblStatus_);

    connect(bStart, &QPushButton::clicked, this, [this] { command({{"cmd", "start_decoder"}}, [this](const QJsonObject& r, const QByteArray&) { logLine("sys", r.value("ok").toBool() ? "decoder start requested" : "start failed: " + r.value("error").toString()); }); });
    connect(bStop, &QPushButton::clicked, this, [this] { command({{"cmd", "stop_decoder"}}, [this](const QJsonObject&, const QByteArray&) { logLine("sys", "decoder stop requested"); }); });
    connect(chkShips_, &QCheckBox::toggled, this, [this] { map_->setShow(chkShips_->isChecked(), chkPlanes_->isChecked()); });
    connect(chkPlanes_, &QCheckBox::toggled, this, [this] { map_->setShow(chkShips_->isChecked(), chkPlanes_->isChecked()); });
    connect(spinAvg_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int n) { spec_->setAverage(n); });
    connect(chkHold_, &QCheckBox::toggled, this, [this](bool on) { spec_->setMaxHold(on); });
    connect(bAuto, &QPushButton::clicked, this, [this] { spec_->autoScale(); });
    connect(bPause, &QPushButton::clicked, this, [this, bPause] { jsonPaused_ = !jsonPaused_; bPause->setText(jsonPaused_ ? "Resume Feed" : "Pause Feed"); logLine("sys", jsonPaused_ ? "ZMQ JSON Feed paused" : "ZMQ JSON Feed resumed"); });
    connect(bClear, &QPushButton::clicked, this, [this] { json_->setPlainText("// Buffer cleared. Waiting for live ZeroMQ JSON data..."); });
    connect(bCopy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(json_->toPlainText()); logLine("sys", "Copied ZMQ JSON feed to clipboard"); });
    connect(targetsT_, &QTableWidget::cellClicked, this, [this](int r, int) {
        QString id = targetsT_->item(r, 0) ? targetsT_->item(r, 0)->text() : QString();
        const QJsonObject* t = ais_.contains(id) ? &ais_[id] : adsb_.contains(id) ? &adsb_[id] : nullptr;
        if (t && hasPos(*t)) { map_->setView(t->value("lat").toDouble(), t->value("lon").toDouble(), 12); map_->openPopup(id); }
    });
}

void MainWindow::logLine(const QString& type, const QString& text) {
    QColor c = type == "ais" ? QColor("#2563eb") : type == "adsb" ? QColor("#b45309") : QColor("#4d545e");
    console_->appendHtml(QString("<span style=\"color:#7b838e\">[%1]</span> <span style=\"color:%2;white-space:pre\">%3</span>")
                             .arg(QDateTime::currentDateTimeUtc().toString("HH:mm:ss"), c.name(), text.toHtmlEscaped()));
}

void MainWindow::setDecoderBadge(int state) {
    dotDec_->setStyleSheet(dotStyle(state == 1 ? "#1f7a4d" : state == 2 ? "#b02a2a" : "#9a6a08"));
    lblDec_->setText(state == 1 ? "Decoder · RUNNING" : state == 2 ? "Decoder · stopped" : "Decoder · unknown");
}

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
void MainWindow::onGap(const QString& topic, int lost) { topicLost_[topic.section('/', 0, 2)] += lost; }

// a late joiner asks for everything now instead of waiting for the next periodic re-publish
void MainWindow::requestState() {
    lastStateReq_ = clock_.elapsed();
    command({{"cmd", "get_state"}}, [this](const QJsonObject& h, const QByteArray&) {
        if (!h.value("ok").toBool()) return;
        setBands(h.value("bands").toArray());
        QJsonObject st = h.value("status").toObject();
        if (!st.isEmpty()) { lastStatus_ = st; decoderRunning_ = st.value("decoder_running").toBool(); setDecoderBadge(decoderRunning_ ? 1 : 2); }
        replaceAll(true, h.value("ais_vessels").toArray());
        replaceAll(false, h.value("adsb_aircraft").toArray());
    });
}

void MainWindow::setBands(const QJsonArray& arr) {
    QString sig = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    if (sig == bandsSig_) return;
    bandsSig_ = sig;
    QVector<SpecBand> bs;
    for (const QJsonValue& v : arr) {
        QJsonObject o = v.toObject();
        SpecBand b;
        b.key = o.value("key").toString();
        b.name = o.value("name").toString();
        b.freqMhz = o.value("freq_mhz").toDouble();
        b.spanMhz = o.value("samp_rate_hz").toDouble() / 1e6;
        bs.push_back(b);
    }
    spec_->setBands(bs);
}

void MainWindow::upsert(bool ship, const QJsonObject& t) {
    QString id = ship ? t.value("mmsi").toString() : t.value("icao").toString();
    if (id.isEmpty()) return;
    (ship ? ais_ : adsb_)[id] = t;
    if (!framed_ && hasPos(t)) { framed_ = true; map_->frameFirst(t.value("lat").toDouble(), t.value("lon").toDouble()); }
}
void MainWindow::remove(bool ship, const QString& id) { (ship ? ais_ : adsb_).remove(id); }

void MainWindow::replaceAll(bool ship, const QJsonArray& list) {
    QSet<QString> keep;
    for (const QJsonValue& v : list) {
        QJsonObject t = v.toObject();
        keep.insert(ship ? t.value("mmsi").toString() : t.value("icao").toString());
        upsert(ship, t);
    }
    auto& m = ship ? ais_ : adsb_;
    for (auto it = m.begin(); it != m.end();) it = keep.contains(it.key()) ? std::next(it) : m.erase(it);
}

void MainWindow::jsonFeed(const QString& topic, const QJsonObject& payload) {
    if (jsonPaused_) return;
    QJsonObject e{{"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"zmq_topic", topic}, {"payload", payload}};
    QString s = QString::fromUtf8(QJsonDocument(e).toJson(QJsonDocument::Indented));
    if (json_->toPlainText().startsWith("// ")) json_->clear();
    json_->appendPlainText(s);
}

void MainWindow::onFrame(const Frame& f) {
    qint64 now = clock_.elapsed();
    lastFrameAt_ = now;
    const QStringList p = f.topic.split('/');                  // topics are compared exactly: "air/ais/target" is not "air/ais/targets"
    if (p.size() < 2 || p[0] != "air") return;
    topicCount_[p.size() >= 3 ? p[0] + "/" + p[1] + (p[1] == "spectrum" ? "" : "/" + p[2]) : f.topic]++;
    if (p.size() == 3 && p[1] == "spectrum") {
        int ns = f.hdr.value("n_spec").toInt(), np = f.hdr.value("n_peak").toInt();
        QVector<float> mean, peak;
        if (ns && f.payload.size() == 4 * (ns + np)) {
            const float* d = (const float*)f.payload.constData();
            mean = QVector<float>(d, d + ns);
            peak = QVector<float>(d + ns, d + ns + np);
        }
        spec_->update2(p[2], mean, peak, f.hdr.value("active").toBool(), now);
        ++specFrames_;
    } else if (f.topic == "air/ais/target" || f.topic == "air/adsb/target") {
        bool ship = f.topic == "air/ais/target";
        QJsonObject t = f.hdr.value("target").toObject();
        upsert(ship, t);
        lastMsg_[ship ? "ais" : "adsb"] = now;
        jsonFeed(ship ? "AIS_VESSEL" : "ADSB_FLIGHT", t);
        logLine(ship ? "ais" : "adsb", ship ? QString("AIS Target MMSI %1 (%2) updated").arg(t.value("mmsi").toString(), t.value("name").toString("SHIP"))
                                            : QString("ADS-B Flight ICAO %1 (%2) updated").arg(t.value("icao").toString(), t.value("callsign").toString("FLT")));
    } else if (f.topic == "air/ais/remove" || f.topic == "air/adsb/remove") {
        bool ship = f.topic == "air/ais/remove";
        remove(ship, f.hdr.value(ship ? "mmsi" : "icao").toString());   // the backend stopped hearing it (ADS-B 60 s, AIS 10 min)
    } else if (f.topic == "air/ais/targets") {
        replaceAll(true, f.hdr.value("targets").toArray());
    } else if (f.topic == "air/adsb/targets") {
        replaceAll(false, f.hdr.value("targets").toArray());
    } else if (f.topic == "air/bands") {
        setBands(f.hdr.value("bands").toArray());
    } else if (f.topic == "air/status") {
        lastStatus_ = f.hdr;
        decoderRunning_ = f.hdr.value("decoder_running").toBool();
        setDecoderBadge(decoderRunning_ ? 1 : 2);
    }
}

void MainWindow::refreshMap() {
    QVector<MapTarget> mt;
    for (auto it = ais_.begin(); it != ais_.end(); ++it) {
        const QJsonObject& v = it.value();
        if (!hasPos(v)) continue;                              // a target with no real position yet is never drawn at an invented place
        MapTarget t;
        t.id = it.key();
        t.ship = true;
        t.lat = v.value("lat").toDouble();
        t.lon = v.value("lon").toDouble();
        t.heading = v.value("heading").isDouble() ? v.value("heading").toDouble() : v.value("course").isDouble() ? v.value("course").toDouble() : -1;
        t.label = v.value("name").toString();
        t.popupHtml = QString("<b style='color:#60a5fa'>Ship %1</b><br><b>MMSI:</b> %2<br><b>Callsign:</b> %3<br><b>Speed:</b> %4 kn<br><b>Course:</b> %5°")
                          .arg(v.value("name").toString("VESSEL"), t.id, v.value("callsign").toString("N/A"), num(v.value("speed")), num(v.value("course")));
        mt.push_back(t);
    }
    for (auto it = adsb_.begin(); it != adsb_.end(); ++it) {
        const QJsonObject& a = it.value();
        if (!hasPos(a)) continue;
        MapTarget t;
        t.id = it.key();
        t.ship = false;
        t.lat = a.value("lat").toDouble();
        t.lon = a.value("lon").toDouble();
        t.heading = a.value("heading").isDouble() ? a.value("heading").toDouble() : -1;
        t.label = a.value("callsign").toString();
        t.popupHtml = QString("<b style='color:#fbbf24'>Flight %1</b><br><b>ICAO Hex:</b> %2<br><b>Altitude:</b> %3 ft<br><b>Speed:</b> %4 kts<br><b>Heading:</b> %5°")
                          .arg(a.value("callsign").toString("FLIGHT"), t.id, num(a.value("alt")), num(a.value("speed")), num(a.value("heading")));
        mt.push_back(t);
    }
    map_->setTargets(mt);
}

void MainWindow::renderTables() {
    qint64 now = clock_.elapsed();
    auto ago = [&](const QString& k) { qint64 t = lastMsg_.value(k, 0); return t ? QString("%1 s ago").arg((now - t) / 1000) : QString("none yet"); };
    signalsT_->setRowCount(2);
    setCell(signalsT_, 0, 0, "AIS A+B", QColor("#2563eb"), true);
    setCell(signalsT_, 0, 1, "162.000");
    setCell(signalsT_, 0, 2, ago("ais"), QColor("#7b838e"));
    setCell(signalsT_, 0, 3, QString::number(ais_.size()));
    setCell(signalsT_, 1, 0, "ADS-B", QColor("#d97706"), true);
    setCell(signalsT_, 1, 1, "1090.000");
    setCell(signalsT_, 1, 2, ago("adsb"), QColor("#7b838e"));
    setCell(signalsT_, 1, 3, QString::number(adsb_.size()));
    int total = ais_.size() + adsb_.size();
    // rebuild the target rows only when something changed (keeps a selected row and the scroll position)
    QString sig;
    for (auto it = ais_.begin(); it != ais_.end(); ++it) sig += it.key() + num(it.value().value("speed")) + num(it.value().value("course")) + ";";
    for (auto it = adsb_.begin(); it != adsb_.end(); ++it) sig += it.key() + num(it.value().value("speed")) + num(it.value().value("heading")) + ";";
    if (sig == mapSig_) return;
    mapSig_ = sig;
    targetsT_->clearSpans();
    if (!total) {
        targetsT_->setRowCount(1);
        targetsT_->setSpan(0, 0, 1, 4);
        setCell(targetsT_, 0, 0, "No active targets decoded yet", QColor("#7b838e"));
        return;
    }
    targetsT_->setRowCount(total);
    int r = 0;
    for (auto it = ais_.begin(); it != ais_.end(); ++it, ++r) {
        const QJsonObject& v = it.value();
        setCell(targetsT_, r, 0, it.key(), QColor("#2563eb"), true);
        setCell(targetsT_, r, 1, "⚓ AIS Marine Vessel");
        setCell(targetsT_, r, 2, v.value("speed").isDouble() ? num(v.value("speed")) + " kn" : "—");
        setCell(targetsT_, r, 3, v.value("course").isDouble() ? num(v.value("course")) + "°" : "—");
    }
    for (auto it = adsb_.begin(); it != adsb_.end(); ++it, ++r) {
        const QJsonObject& a = it.value();
        setCell(targetsT_, r, 0, it.key(), QColor("#d97706"), true);
        setCell(targetsT_, r, 1, "✈ ADS-B Aircraft");
        setCell(targetsT_, r, 2, a.value("speed").isDouble() ? num(a.value("speed")) + " kts" : "—");
        setCell(targetsT_, r, 3, a.value("heading").isDouble() ? num(a.value("heading")) + "°" : "—");
    }
}

void MainWindow::tick() {
    qint64 now = clock_.elapsed();
    lblClock_->setText(QDateTime::currentDateTimeUtc().toString("HH:mm:ss") + " UTC");
    bool live = lastFrameAt_ && now - lastFrameAt_ < 5000;
    QString lab = live ? "LIVE — real IQ + real decode" : "NOT CONNECTED";
    if (lab != lblSystem_->text() && qEnvironmentVariableIsSet("SECURECOM_UI_TRACE")) { std::printf("[ui %6.1fs] %s\n", now / 1000.0, lab.toUtf8().constData()); std::fflush(stdout); }
    lblSystem_->setText(lab);
    dotSystem_->setStyleSheet(dotStyle(live ? "#1f7a4d" : "#9a6a08"));
    if (!live) setDecoderBadge(0);
    if (!live && now - lastStateReq_ > 3000) requestState();
    if (live && spec_->bands().isEmpty() && now - lastStateReq_ > 2000) requestState();
    spec_->setNow(now);
    spec_->update();
    lblShips_->setText(QString("🚢 %1").arg(ais_.size()));
    lblPlanes_->setText(QString("✈ %1").arg(adsb_.size()));
    refreshMap();
    renderTables();
    QString pk;
    for (const SpecBand& b : spec_->bands()) {
        float p = AirSpectrum::peakDb(b), f = AirSpectrum::floorDb(b);
        pk += QString("%1 %2 / %3 dB   ").arg(b.name, p > -900 ? QString::number(p, 'f', 0) : "—", f > -900 ? QString::number(f, 'f', 0) : "—");
    }
    lblPeak_->setText("Peak / floor: " + (pk.isEmpty() ? QString("—") : pk));
    long long lost = 0;
    for (auto v : topicLost_) lost += v;
    QString st = QString("%1 messages received").arg(sub_->messages.load()) + (lost ? QString(" · %1 lost (sequence gaps)").arg(lost) : QString(" · no sequence gaps"));
    if (lastStatus_.contains("iq_connected")) {
        int c = 0, t = 0;
        for (auto v : lastStatus_.value("iq_connected").toObject()) { ++t; c += v.toBool(); }
        st += QString(" · IQ arriving on %1/%2 bands").arg(c).arg(t);
    }
    lblStatus_->setText(live ? st : QString("waiting for %1").arg(subAddr_));
}

QPixmap MainWindow::screenshot() { return grab(); }
void MainWindow::showTab(int i) { tabs_->setCurrentIndex(i); }

// self-test of the map: frame, pan by dragging, wheel zoom, click a marker, hide ships
void MainWindow::runMapTest() {
    QJsonObject r;
    const QJsonObject* first = nullptr;
    for (auto it = adsb_.begin(); it != adsb_.end(); ++it) if (hasPos(it.value())) { first = &it.value(); break; }
    if (!first) for (auto it = ais_.begin(); it != ais_.end(); ++it) if (hasPos(it.value())) { first = &it.value(); break; }
    if (!first) { testResults_["map"] = "no positioned target to test with"; return; }
    QString id = first->value(first->contains("icao") ? "icao" : "mmsi").toString();
    double lat = first->value("lat").toDouble(), lon = first->value("lon").toDouble();
    map_->setView(lat, lon, 8);
    QApplication::processEvents();
    r["target"] = id;
    r["tiles_drawn_zoom8"] = map_->tilesDrawn();
    r["markers_visible_zoom8"] = map_->markersVisible();
    auto mouse = [&](QEvent::Type t, QPointF pos) { QMouseEvent e(t, pos, map_->mapToGlobal(pos.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(map_, &e); };
    QPointF c(map_->width() / 2.0, map_->height() / 2.0);
    double lon0 = map_->centerLon();
    mouse(QEvent::MouseButtonPress, c);
    mouse(QEvent::MouseMove, c + QPointF(120, 0));
    mouse(QEvent::MouseButtonRelease, c + QPointF(120, 0));
    r["pan_drag_120px_east_lon_change_deg"] = map_->centerLon() - lon0;          // dragging the map east moves the centre west (negative)
    QWheelEvent we(c, map_->mapToGlobal(c.toPoint()), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    int z0 = map_->zoomLevel();
    QApplication::sendEvent(map_, &we);
    r["wheel_in_zoom_before_after"] = QString("%1 -> %2").arg(z0).arg(map_->zoomLevel());
    map_->setView(lat, lon, 8);
    QApplication::processEvents();
    QPointF mp = map_->project(lat, lon);
    mouse(QEvent::MouseButtonPress, mp);
    mouse(QEvent::MouseButtonRelease, mp);
    r["click_on_marker_opens_popup_for"] = map_->popupId();
    r["click_matches_target"] = map_->popupId() == id;
    int before = map_->markersVisible();
    chkShips_->setChecked(false);
    chkPlanes_->setChecked(false);
    QApplication::processEvents();
    r["markers_visible_all_hidden"] = QString("%1 -> %2").arg(before).arg(map_->markersVisible());
    chkShips_->setChecked(true);
    chkPlanes_->setChecked(true);
    testResults_["map"] = r;
}

QJsonObject MainWindow::summary() const {
    QJsonObject o, topics, lost;
    for (auto it = topicCount_.begin(); it != topicCount_.end(); ++it) topics[it.key()] = (double)it.value();
    for (auto it = topicLost_.begin(); it != topicLost_.end(); ++it) lost[it.key()] = (double)it.value();
    o["messages"] = (double)sub_->messages.load();
    o["topics"] = topics;
    o["lost"] = lost;
    o["system_label"] = lblSystem_->text();
    o["decoder_running"] = decoderRunning_;
    o["ais_targets"] = ais_.size();
    o["adsb_targets"] = adsb_.size();
    int ap = 0, sp = 0;
    for (auto& v : ais_) sp += hasPos(v);
    for (auto& v : adsb_) ap += hasPos(v);
    o["ais_with_position"] = sp;
    o["adsb_with_position"] = ap;
    QJsonArray bands;
    for (const SpecBand& b : spec_->bands()) {
        QJsonObject x;
        x["key"] = b.key;
        x["frames"] = (double)b.frames;
        x["peak_db"] = AirSpectrum::peakDb(b);
        x["floor_db"] = AirSpectrum::floorDb(b);
        bands.push_back(x);
    }
    o["spectrum"] = bands;
    o["map_zoom"] = map_->zoomLevel();
    o["map_center"] = QString("%1, %2").arg(map_->centerLat(), 0, 'f', 3).arg(map_->centerLon(), 0, 'f', 3);
    o["map_tiles_drawn"] = map_->tilesDrawn();
    o["map_markers_visible"] = map_->markersVisible();
    o["tiles_ok"] = tiles_->ok();
    o["tests"] = testResults_;
    return o;
}
