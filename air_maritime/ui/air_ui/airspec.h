// RF spectrum of the air & maritime bands, one panel per band (continuous IQ: both are always shown):
// yellow = running mean over N updates, cyan = peak per bin within the latest 0.1 s (shows the bursts), orange = max hold.
#pragma once
#include <QMap>
#include <QString>
#include <QVector>
#include <QWidget>

struct SpecBand {
    QString key, name;
    double freqMhz = 0, spanMhz = 0;
    QVector<double> lin;               // running mean, linear power
    QVector<float> avg, peak, hold;    // dB
    bool active = false;
    qint64 tMs = 0;
    long long frames = 0;
};

class AirSpectrum : public QWidget {
    Q_OBJECT
public:
    explicit AirSpectrum(QWidget* p = nullptr) : QWidget(p) { setMinimumHeight(150); }
    void setBands(const QVector<SpecBand>& b);
    void update2(const QString& key, const QVector<float>& mean, const QVector<float>& peak, bool active, qint64 nowMs);
    void setAverage(int n) { avgN_ = std::max(1, n); }
    void setMaxHold(bool on);
    void autoScale();
    void setNow(qint64 ms) { nowMs_ = ms; }
    const QVector<SpecBand>& bands() const { return bands_; }
    double yMin() const { return yMin_; }
    double yMax() const { return yMax_; }
    static float peakDb(const SpecBand& b);
    static float floorDb(const SpecBand& b);             // median of the average trace
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QVector<SpecBand> bands_;
    int avgN_ = 10;
    bool hold_ = false;
    double yMin_ = -110, yMax_ = -10;
    qint64 nowMs_ = 0;
    bool scaled_ = false;
};
