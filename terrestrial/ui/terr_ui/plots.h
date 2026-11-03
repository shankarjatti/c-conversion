// The three graphs of the console: composite spectrum, waterfall, time domain. Same interaction as the HTML page:
// drag = zoom to the selection, wheel = scale around the cursor, double-click or "reset zoom" = full span.
#pragma once
#include <QColor>
#include <QVector>
#include <QWidget>

QColor heatColor(double t);                          // deep blue (noise floor) -> blue -> cyan -> green -> yellow -> red

struct Carrier { double f_mhz; int ch; double rel_db; bool occupied; };

class ZoomPlot : public QWidget {
    Q_OBJECT
public:
    ZoomPlot(const QString& label, double fullMin, double fullMax, QWidget* parent = nullptr);
    void setFullRange(double lo, double hi);
    void setReadout(const QString& s) { readout_ = s; update(); }
    double viewMin() const { return vmin_; }
    double viewMax() const { return vmax_; }
    void resetZoom() { vmin_ = fmin_; vmax_ = fmax_; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override { resetZoom(); }
    void wheelEvent(QWheelEvent*) override;
    virtual void paintPlot(QPainter& p, const QRectF& r) = 0;    // the data area; r = the whole widget
    double fmin_, fmax_, vmin_, vmax_;
    double dragA_ = -1, dragB_ = -1;                              // 0..1 across the widget while dragging, else -1
    QString label_, readout_;
};

class SpectrumPlot : public ZoomPlot {
    Q_OBJECT
public:
    explicit SpectrumPlot(QWidget* p = nullptr) : ZoomPlot("FREQUENCY  —  drag to zoom, wheel to scale, dbl-click to reset", 419, 454, p) {}
    void setData(const QVector<float>& bins, const QVector<Carrier>& carriers) { bins_ = bins; carriers_ = carriers; update(); }
    void setLegend(const QVector<QPair<QString, QColor>>& l) { legend_ = l; }
protected:
    void paintPlot(QPainter& p, const QRectF& r) override;
private:
    QVector<float> bins_;
    QVector<Carrier> carriers_;
    QVector<QPair<QString, QColor>> legend_;
};

class WaterfallPlot : public ZoomPlot {
    Q_OBJECT
public:
    explicit WaterfallPlot(QWidget* p = nullptr) : ZoomPlot("WATERFALL", 419, 454, p) {}
    void setRows(const QVector<QVector<float>>* rows, int maxRows) { rows_ = rows; maxRows_ = maxRows; update(); }
protected:
    void paintPlot(QPainter& p, const QRectF& r) override;
private:
    const QVector<QVector<float>>* rows_ = nullptr;     // newest first, owned by the window
    int maxRows_ = 160;
};

class TimePlot : public ZoomPlot {
    Q_OBJECT
public:
    explicit TimePlot(QWidget* p = nullptr) : ZoomPlot("TIME", 0, 1, p) {}
    void setData(const QVector<float>& buf, const QColor& c, const QString& band) { buf_ = buf; color_ = c; setReadout("active: " + (band.isEmpty() ? QString("—") : band.toUpper())); }
protected:
    void paintPlot(QPainter& p, const QRectF& r) override;
private:
    QVector<float> buf_;
    QColor color_{26, 95, 138};
};
