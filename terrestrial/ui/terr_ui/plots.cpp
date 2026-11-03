#include "plots.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

QColor heatColor(double t) {
    static const double S[6][4] = {{0, 10, 16, 70}, {0.22, 10, 70, 200}, {0.42, 0, 190, 200}, {0.6, 40, 210, 55}, {0.8, 240, 225, 25}, {1, 235, 35, 25}};
    t = std::min(1.0, std::max(0.0, t));
    for (int s = 1; s < 6; ++s) {
        if (t <= S[s][0]) {
            double k = (t - S[s - 1][0]) / ((S[s][0] - S[s - 1][0]) ? (S[s][0] - S[s - 1][0]) : 1);
            return QColor((int)std::lround(S[s - 1][1] + (S[s][1] - S[s - 1][1]) * k), (int)std::lround(S[s - 1][2] + (S[s][2] - S[s - 1][2]) * k),
                          (int)std::lround(S[s - 1][3] + (S[s][3] - S[s - 1][3]) * k));
        }
    }
    return QColor(235, 35, 25);
}

ZoomPlot::ZoomPlot(const QString& label, double lo, double hi, QWidget* parent)
    : QWidget(parent), fmin_(lo), fmax_(hi), vmin_(lo), vmax_(hi), label_(label) {
    setMouseTracking(false);
    setMinimumHeight(60);
}

void ZoomPlot::setFullRange(double lo, double hi) {
    bool full = vmin_ == fmin_ && vmax_ == fmax_;
    fmin_ = lo;
    fmax_ = hi;
    if (full) { vmin_ = lo; vmax_ = hi; }
    update();
}

void ZoomPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    QRectF r(0, 0, width(), height());
    paintPlot(p, r);
    if (dragA_ >= 0) { p.fillRect(QRectF(std::min(dragA_, dragB_) * width(), 0, std::fabs(dragB_ - dragA_) * width(), height()), QColor(242, 226, 0, 46)); }
    p.setPen(QColor("#9aa3af"));
    p.setFont(QFont("DejaVu Sans Mono", 7));
    p.drawText(QRectF(6, 3, width() - 12, 14), Qt::AlignLeft, label_);
    p.drawText(QRectF(6, 3, width() - 12 - (vmin_ != fmin_ || vmax_ != fmax_ ? 70 : 6), 14), Qt::AlignRight, readout_);
    if (vmin_ != fmin_ || vmax_ != fmax_) p.drawText(QRectF(6, 3, width() - 12, 14), Qt::AlignRight, "reset zoom");
    p.setPen(QColor("#d3d7dd"));
    p.drawLine(0, height() - 1, width(), height() - 1);
}

void ZoomPlot::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (e->pos().y() < 18 && e->pos().x() > width() - 80 && (vmin_ != fmin_ || vmax_ != fmax_)) { resetZoom(); return; }
    dragA_ = dragB_ = std::min(1.0, std::max(0.0, e->pos().x() / (double)width()));
}
void ZoomPlot::mouseMoveEvent(QMouseEvent* e) {
    if (dragA_ < 0) return;
    dragB_ = std::min(1.0, std::max(0.0, e->pos().x() / (double)width()));
    update();
}
void ZoomPlot::mouseReleaseEvent(QMouseEvent*) {
    if (dragA_ < 0) return;
    if (std::fabs(dragB_ - dragA_) > 0.015) {
        double span = vmax_ - vmin_, a = vmin_ + std::min(dragA_, dragB_) * span, b = vmin_ + std::max(dragA_, dragB_) * span;
        vmin_ = a;
        vmax_ = b;
    }
    dragA_ = dragB_ = -1;
    update();
}
void ZoomPlot::wheelEvent(QWheelEvent* e) {
    double px = e->position().x() / width(), span = vmax_ - vmin_, centre = vmin_ + px * span;
    double factor = e->angleDelta().y() < 0 ? 1.15 : 0.87;
    double full = fmax_ - fmin_, ns = std::max(full * 0.01, std::min(full, span * factor));
    double a = centre - px * ns, b = centre + (1 - px) * ns;
    if (a < fmin_) { b += fmin_ - a; a = fmin_; }
    if (b > fmax_) { a -= b - fmax_; b = fmax_; }
    vmin_ = std::max(fmin_, a);
    vmax_ = std::min(fmax_, b);
    update();
}

// ---------------------------------------------------------------- spectrum
void SpectrumPlot::paintPlot(QPainter& p, const QRectF& r) {
    p.fillRect(r, Qt::black);
    auto y4db = [&](double db) { return r.height() - ((db + 110) / 100.0) * r.height(); };
    p.setFont(QFont("DejaVu Sans Mono", 7));
    for (int db = -100; db <= -20; db += 20) {
        double y = y4db(db);
        p.setPen(QColor(120, 130, 145, 56));
        p.drawLine(QPointF(0, y), QPointF(r.width(), y));
        p.setPen(QColor("#9aa3af"));
        p.drawText(QPointF(3, y - 2), QString::number(db));
        p.drawText(QPointF(r.width() - 22, y - 2), QString::number(db));
    }
    double perMhz = r.width() / (vmax_ - vmin_);                    // label every 1/2/5/10/... MHz, at least ~90 px apart
    double step = 1;
    for (double c : {1.0, 2.0, 5.0, 10.0, 20.0}) { step = c; if (c * perMhz >= 90) break; }
    if (perMhz > 400) step = 0.1 * std::ceil(90 / perMhz / 0.1);       // deep zoom: sub-MHz steps
    for (double f = std::ceil(vmin_ / step) * step; f <= vmax_; f += step) {
        double x = (f - vmin_) / (vmax_ - vmin_) * r.width();
        p.setPen(QColor(120, 130, 145, 41));
        p.drawLine(QPointF(x, 0), QPointF(x, r.height()));
        p.setPen(QColor("#9aa3af"));
        p.drawText(QPointF(x + 2, r.height() - 3), QString::number(f, 'f', step < 1 ? 1 : 0) + " MHz");
    }
    for (const Carrier& c : carriers_) {                          // grey band per channel, level and number on occupied ones
        if (c.f_mhz < vmin_ || c.f_mhz > vmax_) continue;
        double x = (c.f_mhz - vmin_) / (vmax_ - vmin_) * r.width();
        p.fillRect(QRectF(x - 1.5, 0, 3, r.height()), c.occupied ? QColor(180, 190, 205, 56) : QColor(150, 160, 175, 26));
        if (c.occupied) {
            p.setPen(QColor("#d8dde4"));
            p.drawText(QRectF(x - 20, 14, 40, 12), Qt::AlignHCenter, (c.rel_db > 0 ? "+" : "") + QString::number(c.rel_db, 'f', 0));
            p.drawText(QRectF(x - 20, r.height() - 26, 40, 12), Qt::AlignHCenter, QString::number(c.ch));
        }
    }
    if (!bins_.isEmpty()) {                                       // the single yellow trace across the span
        p.setPen(QPen(QColor("#f2e200"), 1.2));
        QPolygonF poly;
        int n = bins_.size();
        double full = fmax_ - fmin_;
        for (int i = 0; i < n; ++i) {
            double f = fmin_ + (double)i / n * full;
            if (f < vmin_ || f > vmax_) continue;
            poly << QPointF((f - vmin_) / (vmax_ - vmin_) * r.width(), y4db(bins_[i]));
        }
        if (poly.size() > 1) p.drawPolyline(poly);
    }
    double x = 8;                                                  // legend, bottom left above the axis labels
    p.setFont(QFont("DejaVu Sans", 7));
    for (auto& l : legend_) {
        p.fillRect(QRectF(x, r.height() - 52, 9, 9), l.second);
        p.setPen(QColor("#d8dde4"));
        p.drawText(QPointF(x + 12, r.height() - 44), l.first);
        x += 22 + 6.2 * l.first.size();
    }
}

// ---------------------------------------------------------------- waterfall
void WaterfallPlot::paintPlot(QPainter& p, const QRectF& r) {
    const double BAR = 34, DB_LO = -100, DB_HI = -30;
    double plotW = r.width() - BAR;
    p.fillRect(QRectF(0, 0, plotW, r.height()), heatColor(0));
    if (rows_ && !rows_->isEmpty()) {
        double rowH = r.height() / maxRows_;
        int n = rows_->at(0).size();
        double full = fmax_ - fmin_, colW = std::max(1.2, plotW / n * 2);
        for (int ri = 0; ri < rows_->size(); ++ri) {
            const QVector<float>& row = rows_->at(ri);
            for (int i = 0; i < n; i += 2) {
                double f = fmin_ + (double)i / n * full;
                if (f < vmin_ || f > vmax_) continue;
                p.fillRect(QRectF((f - vmin_) / (vmax_ - vmin_) * plotW, ri * rowH, colW, rowH + 0.6), heatColor((row[i] - DB_LO) / (DB_HI - DB_LO)));
            }
        }
    }
    double bx = plotW + 6, bw = BAR - 12;                          // colour bar with a dB scale
    p.fillRect(QRectF(plotW, 0, BAR, r.height()), Qt::black);
    for (int y = 0; y < r.height(); ++y) p.fillRect(QRectF(bx, y, bw, 1), heatColor(1 - y / r.height()));
    p.setFont(QFont("DejaVu Sans Mono", 7));
    p.setPen(QColor("#c9ced6"));
    for (int k = 0; k <= 4; ++k) {
        int db = (int)std::lround(DB_HI - k * (DB_HI - DB_LO) / 4);
        p.drawText(QPointF(bx, std::min(r.height() - 2, std::max(8.0, k / 4.0 * r.height() + 3))), QString::number(db));
    }
}

// ---------------------------------------------------------------- time
void TimePlot::paintPlot(QPainter& p, const QRectF& r) {
    p.fillRect(r, Qt::white);
    p.setPen(QColor("#e4e7eb"));
    for (int i = 1; i < 4; ++i) p.drawLine(QPointF(0, r.height() * i / 4), QPointF(r.width(), r.height() * i / 4));
    int n = buf_.size();
    if (n < 2) return;
    int i0 = (int)std::floor(vmin_ * n), i1 = std::min(n, (int)std::ceil(vmax_ * n));
    p.setPen(QPen(color_, 1.2));
    float mx = 1e-4f;                                                // autoscale: the IQ is tiny (-45 dBFS) and would be a flat line at full scale
    for (int i = i0; i < i1; ++i) mx = std::max(mx, std::fabs(buf_[i]));
    QPolygonF poly;
    for (int i = i0; i < i1; ++i) poly << QPointF(((double)i / n - vmin_) / (vmax_ - vmin_) * r.width(), r.height() / 2 - buf_[i] / mx * r.height() * 0.42);
    if (poly.size() > 1) p.drawPolyline(poly);
}
