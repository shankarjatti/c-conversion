#include "mapwidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QTextDocument>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

MapWidget::MapWidget(TileStore* tiles, QWidget* parent) : QWidget(parent), tiles_(tiles) { setMinimumHeight(120); }

double MapWidget::latToY(double lat, int z) {
    lat = std::max(-85.0511, std::min(85.0511, lat));
    double r = lat * M_PI / 180;
    return (1 - std::asinh(std::tan(r)) / M_PI) / 2 * 256.0 * (1 << z);
}
double MapWidget::yToLat(double y, int z) {
    double n = M_PI - 2 * M_PI * y / (256.0 * (1 << z));
    return std::atan(std::sinh(n)) * 180 / M_PI;
}

QPointF MapWidget::project(double lat, double lon) const {
    return QPointF(lonToX(lon, z_) - lonToX(lon_, z_) + width() / 2.0, latToY(lat, z_) - latToY(lat_, z_) + height() / 2.0);
}

void MapWidget::setView(double lat, double lon, int zoom) {
    lat_ = std::max(-85.0, std::min(85.0, lat));
    lon_ = lon;
    z_ = std::max(0, std::min(tiles_ ? tiles_->maxZoom() : 12, zoom));
    update();
    emit viewChanged();
}

void MapWidget::frameFirst(double lat, double lon) {
    int zoom = 0;
    for (int z = std::min(11, tiles_ ? tiles_->maxZoom() : 11); z > 0; --z) {
        int n = 1 << z;
        int x = (int)std::floor((lon + 180) / 360 * n), y = (int)std::floor((1 - std::asinh(std::tan(lat * M_PI / 180)) / M_PI) / 2 * n);
        if (tiles_ && tiles_->has(z, x, y)) { zoom = z; break; }
    }
    setView(lat, lon, zoom);
}

void MapWidget::zoomAt(const QPointF& px, int dz) {
    int nz = std::max(0, std::min(tiles_ ? tiles_->maxZoom() : 12, z_ + dz));
    if (nz == z_) return;
    // keep the geographic point under the cursor fixed
    double cx = lonToX(lon_, z_) + (px.x() - width() / 2.0), cy = latToY(lat_, z_) + (px.y() - height() / 2.0);
    double lon = xToLon(cx, z_), lat = yToLat(cy, z_);
    z_ = nz;
    double ncx = lonToX(lon, z_) - (px.x() - width() / 2.0), ncy = latToY(lat, z_) - (px.y() - height() / 2.0);
    lon_ = xToLon(ncx, z_);
    lat_ = std::max(-85.0, std::min(85.0, yToLat(ncy, z_)));
    update();
    emit viewChanged();
}

static QPolygonF shipShape() { return QPolygonF({{0, -11}, {6, -3}, {6, 9}, {-6, 9}, {-6, -3}}); }
static QPolygonF planeShape() {
    return QPolygonF({{0, -12}, {2, -4}, {11, 2}, {11, 5}, {2, 3}, {2, 8}, {5, 11}, {5, 13}, {0, 12}, {-5, 13}, {-5, 11}, {-2, 8}, {-2, 3}, {-11, 5}, {-11, 2}, {-2, -4}});
}

void MapWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#0b0f19"));
    tilesDrawn_ = 0;
    if (tiles_ && tiles_->ok()) {
        double ox = lonToX(lon_, z_) - width() / 2.0, oy = latToY(lat_, z_) - height() / 2.0;   // world pixel of the widget's top-left
        int n = 1 << z_;
        int x0 = (int)std::floor(ox / 256), x1 = (int)std::floor((ox + width()) / 256), y0 = (int)std::floor(oy / 256), y1 = (int)std::floor((oy + height()) / 256);
        for (int ty = std::max(0, y0); ty <= std::min(n - 1, y1); ++ty)
            for (int tx = std::max(0, x0); tx <= std::min(n - 1, x1); ++tx) {
                QPixmap t = tiles_->tile(z_, tx, ty);
                if (t.isNull()) continue;                                   // a tile the database does not hold stays empty
                p.drawPixmap(QPointF(tx * 256 - ox, ty * 256 - oy), t);
                ++tilesDrawn_;
            }
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    markersVisible_ = 0;
    p.setFont(QFont("DejaVu Sans", 7));
    for (const MapTarget& t : targets_) {
        if ((t.ship && !showShips_) || (!t.ship && !showPlanes_)) continue;
        QPointF c = project(t.lat, t.lon);
        if (c.x() < -20 || c.y() < -20 || c.x() > width() + 20 || c.y() > height() + 20) continue;
        ++markersVisible_;
        p.save();
        p.translate(c);
        if (t.heading >= 0) p.rotate(t.heading);
        p.setPen(QPen(Qt::white, 1.2));
        p.setBrush(t.ship ? QColor("#60a5fa") : QColor("#fbbf24"));
        p.drawPolygon(t.ship ? shipShape() : planeShape());
        p.restore();
        if (popupId_ == t.id) { p.setPen(QPen(QColor("#ffffff"), 1.5)); p.setBrush(Qt::NoBrush); p.drawEllipse(c, 16, 16); }
        if (z_ >= 5 && !t.label.isEmpty()) { p.setPen(QColor("#e5e7eb")); p.drawText(c + QPointF(13, 4), t.label); }
    }
    for (const MapTarget& t : targets_) {                                  // popup of the selected target
        if (t.id != popupId_ || (t.ship && !showShips_) || (!t.ship && !showPlanes_)) continue;
        QTextDocument doc;
        doc.setDefaultStyleSheet("body{color:#e5e7eb;font-size:9pt}");
        doc.setHtml("<body>" + t.popupHtml + "</body>");
        doc.setTextWidth(190);
        QSizeF sz = doc.size();
        QPointF c = project(t.lat, t.lon);
        QRectF box(c.x() - sz.width() / 2 - 8, c.y() - sz.height() - 30, sz.width() + 16, sz.height() + 12);
        box.moveLeft(std::max(4.0, std::min(box.left(), width() - box.width() - 4)));
        box.moveTop(std::max(4.0, box.top()));
        p.setPen(QPen(QColor("#374151")));
        p.setBrush(QColor("#1a2234"));
        p.drawRoundedRect(box, 6, 6);
        p.save();
        p.translate(box.topLeft() + QPointF(8, 6));
        doc.drawContents(&p);
        p.restore();
    }
    // zoom buttons (top left) and attribution (bottom right)
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QColor("#9ca3af"));
    for (int i = 0; i < 2; ++i) {
        QRectF b(10, 10 + i * 28, 26, 26);
        p.setBrush(QColor("#1f2937"));
        p.drawRect(b);
        p.setPen(QColor("#e5e7eb"));
        p.setFont(QFont("DejaVu Sans", 12, QFont::Bold));
        p.drawText(b, Qt::AlignCenter, i == 0 ? "+" : "−");
        p.setPen(QColor("#9ca3af"));
    }
    p.setFont(QFont("DejaVu Sans", 7));
    p.drawText(QRectF(0, height() - 16, width() - 6, 14), Qt::AlignRight, QString("Offline MBTiles (stored tiles only, zoom 0-12) · zoom %1").arg(z_));
    if (!tiles_ || !tiles_->ok()) { p.setPen(QColor("#ef4444")); p.drawText(rect(), Qt::AlignCenter, "map tiles not available: " + (tiles_ ? tiles_->error() : QString("no database"))); }
}

int MapWidget::targetAt(const QPointF& pt) const {
    int best = -1;
    double bd = 14 * 14;
    for (int i = 0; i < targets_.size(); ++i) {
        const MapTarget& t = targets_[i];
        if ((t.ship && !showShips_) || (!t.ship && !showPlanes_)) continue;
        QPointF d = project(t.lat, t.lon) - pt;
        double dd = d.x() * d.x() + d.y() * d.y();
        if (dd <= bd) { bd = dd; best = i; }
    }
    return best;
}

void MapWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    pressPos_ = lastPos_ = QPointF(e->pos());
    dragging_ = true;
}
void MapWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    QPointF d = QPointF(e->pos()) - lastPos_;
    lastPos_ = QPointF(e->pos());
    double cx = lonToX(lon_, z_) - d.x(), cy = latToY(lat_, z_) - d.y();
    lon_ = xToLon(cx, z_);
    lat_ = std::max(-85.0, std::min(85.0, yToLat(cy, z_)));
    update();
}
void MapWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (!dragging_) return;
    dragging_ = false;
    QPointF d = QPointF(e->pos()) - pressPos_;
    if (std::abs(d.x()) + std::abs(d.y()) > 4) { emit viewChanged(); return; }          // it was a drag, not a click
    QPointF pt = QPointF(e->pos());
    if (QRectF(10, 10, 26, 26).contains(pt)) { zoomAt(QPointF(width() / 2.0, height() / 2.0), +1); return; }
    if (QRectF(10, 38, 26, 26).contains(pt)) { zoomAt(QPointF(width() / 2.0, height() / 2.0), -1); return; }
    int i = targetAt(pt);
    if (i >= 0) { popupId_ = targets_[i].id; update(); emit targetClicked(popupId_); }
    else if (!popupId_.isEmpty()) { popupId_.clear(); update(); }
}
void MapWidget::mouseDoubleClickEvent(QMouseEvent* e) { zoomAt(QPointF(e->pos()), +1); }
void MapWidget::wheelEvent(QWheelEvent* e) { zoomAt(QPointF(e->pos()), e->angleDelta().y() > 0 ? +1 : -1); }
