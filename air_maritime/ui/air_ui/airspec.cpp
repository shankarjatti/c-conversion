#include "airspec.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

void AirSpectrum::setBands(const QVector<SpecBand>& b) {
    QVector<SpecBand> out;
    for (SpecBand nb : b) {                                  // keep the data of bands that are still there
        for (const SpecBand& o : bands_) if (o.key == nb.key) { nb.lin = o.lin; nb.avg = o.avg; nb.peak = o.peak; nb.hold = o.hold; nb.active = o.active; nb.tMs = o.tMs; nb.frames = o.frames; }
        out.push_back(nb);
    }
    bands_ = out;
    update();
}

void AirSpectrum::update2(const QString& key, const QVector<float>& mean, const QVector<float>& peak, bool active, qint64 nowMs) {
    for (SpecBand& b : bands_) {
        if (b.key != key) continue;
        b.active = active;
        b.tMs = nowMs;
        ++b.frames;
        if (!scaled_ && !mean.isEmpty() && b.frames >= 12) { scaled_ = true; QMetaObject::invokeMethod(this, [this] { autoScale(); }, Qt::QueuedConnection); }   // once; the button stays
        if (mean.isEmpty()) return;                           // idle frame (zero-fill / nothing): keeps the last trace
        QVector<double> l(mean.size());
        for (int i = 0; i < mean.size(); ++i) l[i] = std::pow(10.0, mean[i] / 10.0);
        if (b.lin.size() != l.size() || avgN_ == 1) b.lin = l;
        else for (int i = 0; i < l.size(); ++i) b.lin[i] += (l[i] - b.lin[i]) / avgN_;
        b.avg.resize(l.size());
        for (int i = 0; i < l.size(); ++i) b.avg[i] = (float)(10 * std::log10(b.lin[i]));
        b.peak = peak;
        if (hold_) {
            if (b.hold.size() != peak.size()) b.hold = peak;
            else for (int i = 0; i < peak.size(); ++i) b.hold[i] = std::max(b.hold[i], peak[i]);
        }
        return;
    }
}

void AirSpectrum::setMaxHold(bool on) {
    hold_ = on;
    if (!on) for (SpecBand& b : bands_) b.hold.clear();
}

float AirSpectrum::peakDb(const SpecBand& b) {
    float pk = -999;
    for (float v : b.peak) pk = std::max(pk, v);
    return pk;
}
float AirSpectrum::floorDb(const SpecBand& b) {
    if (b.avg.isEmpty()) return -999;
    QVector<float> s(b.avg);
    std::sort(s.begin(), s.end());
    return s[s.size() / 2];
}

void AirSpectrum::autoScale() {                               // scale from the data shown: noise floor (5th percentile) up to the highest peak
    float lo = 1e9, hi = -1e9;
    for (const SpecBand& b : bands_) {
        if (b.avg.isEmpty()) continue;
        QVector<float> s(b.avg);
        std::sort(s.begin(), s.end());
        lo = std::min(lo, s[(int)(s.size() * 0.05)]);
        for (float v : (b.hold.isEmpty() ? b.peak : b.hold)) hi = std::max(hi, v);
    }
    if (lo > hi) return;
    yMin_ = std::floor((lo - 10) / 10) * 10;
    yMax_ = std::ceil((hi + 5) / 10) * 10;
    update();
}

void AirSpectrum::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#f4f5f7"));
    int n = bands_.size();
    if (!n) { p.setPen(QColor("#7b838e")); p.drawText(rect(), Qt::AlignCenter, "waiting for the band list"); return; }
    int gap = 4, h = (height() - gap * (n - 1)) / n;
    for (int bi = 0; bi < n; ++bi) {
        const SpecBand& b = bands_[bi];
        QRectF r(0, bi * (h + gap), width(), h);
        p.fillRect(r, Qt::black);
        auto yOf = [&](double db) { return r.bottom() - (db - yMin_) / (yMax_ - yMin_) * r.height(); };
        p.setFont(QFont("DejaVu Sans Mono", 7));
        int gstep = (yMax_ - yMin_) > 60 ? 20 : 10;
        for (int db = (int)std::ceil(yMin_ / 10) * 10; db <= yMax_; db += 10) {
            double y = yOf(db);
            p.setPen(QColor("#303030"));
            p.drawLine(QPointF(30, y), QPointF(r.width() - 30, y));
            if (db % gstep == 0 && y > r.top() + 24 && y < r.bottom() - 16) { p.setPen(Qt::white); p.drawText(QPointF(3, y - 2), QString::number(db)); p.drawText(QPointF(r.width() - 24, y - 2), QString::number(db)); }
        }
        p.setPen(Qt::white);
        p.drawText(QPointF(3, r.bottom() - 3), "dB");
        double lo = b.freqMhz - b.spanMhz / 2, hi = b.freqMhz + b.spanMhz / 2, step = b.spanMhz > 1 ? 0.5 : 0.05;
        for (double f = std::ceil(lo / step) * step; f <= hi + 1e-9; f += step) {
            double x = (f - lo) / (hi - lo) * r.width();
            p.setPen(QColor("#303030"));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(Qt::white);
            p.drawText(QPointF(x + 2, r.bottom() - 3), QString::number(f, 'f', 3) + " MHz");
        }
        bool fresh = b.frames && nowMs_ - b.tMs < 1500;
        p.fillRect(r, fresh ? QColor(60, 220, 60, 56) : QColor(255, 255, 255, 26));
        p.setPen(fresh ? QColor("#7dff7d") : QColor(255, 255, 255, 153));
        p.setFont(QFont("DejaVu Sans Mono", 8));
        p.drawText(QRectF(r.left(), r.top() + 12, r.width(), 14), Qt::AlignHCenter,
                   QString("%1  •  %2").arg(b.name, fresh ? (b.active ? "IQ arriving — signal above -60 dB" : "IQ arriving") : "no IQ"));
        auto trace = [&](const QVector<float>& a, const QColor& c, double w) {
            if (a.size() < 2) return;
            p.setPen(QPen(c, w));
            QPolygonF poly;
            for (int i = 0; i < a.size(); ++i) poly << QPointF((double)i / a.size() * r.width(), std::max(r.top(), std::min(r.bottom(), yOf(a[i]))));
            p.drawPolyline(poly);
        };
        if (hold_) trace(b.hold, QColor("#ff8c42"), 1);
        trace(b.peak, QColor("#4fd1ff"), 1);
        trace(b.avg, QColor("#ffe800"), 1.3);
    }
}
