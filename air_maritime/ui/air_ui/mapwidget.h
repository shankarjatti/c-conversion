// Slippy map over offline MBTiles: pan (drag), zoom (wheel, double-click, +/- buttons), ship and aircraft markers, click popups.
#pragma once
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

#include "tiles.h"

struct MapTarget {
    QString id;
    bool ship = true;
    double lat = 0, lon = 0;
    double heading = -1;           // compass degrees, -1 = unknown (drawn unrotated)
    QString label, popupHtml;
};

class MapWidget : public QWidget {
    Q_OBJECT
public:
    MapWidget(TileStore* tiles, QWidget* parent = nullptr);
    void setTargets(const QVector<MapTarget>& t) { targets_ = t; update(); }
    void setShow(bool ships, bool planes) { showShips_ = ships; showPlanes_ = planes; update(); }
    void setView(double lat, double lon, int zoom);
    void frameFirst(double lat, double lon);                 // deepest zoom (<= 11) at which the database holds the tile for this spot
    void openPopup(const QString& id) { popupId_ = id; update(); }
    // test / status access
    int zoomLevel() const { return z_; }
    double centerLat() const { return lat_; }
    double centerLon() const { return lon_; }
    int tilesDrawn() const { return tilesDrawn_; }
    int markersVisible() const { return markersVisible_; }
    QString popupId() const { return popupId_; }
    QPointF project(double lat, double lon) const;           // widget coordinates
signals:
    void targetClicked(const QString& id);
    void viewChanged();
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
private:
    void zoomAt(const QPointF& px, int dz);
    static double lonToX(double lon, int z) { return (lon + 180.0) / 360.0 * 256.0 * (1 << z); }
    static double latToY(double lat, int z);
    static double xToLon(double x, int z) { return x / (256.0 * (1 << z)) * 360.0 - 180.0; }
    static double yToLat(double y, int z);
    int targetAt(const QPointF& p) const;
    TileStore* tiles_;
    QVector<MapTarget> targets_;
    double lat_ = 20, lon_ = 0;
    int z_ = 2;
    bool showShips_ = true, showPlanes_ = true;
    QString popupId_;
    bool dragging_ = false;
    QPointF pressPos_, lastPos_;
    int tilesDrawn_ = 0, markersVisible_ = 0;
};
