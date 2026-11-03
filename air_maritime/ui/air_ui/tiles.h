// Offline map tiles straight from an MBTiles (SQLite) file: only tiles the database really holds are ever drawn.
#pragma once
#include <QCache>
#include <QMutex>
#include <QPixmap>
#include <QString>

struct sqlite3;

class TileStore {
public:
    explicit TileStore(const QString& path);
    ~TileStore();
    bool ok() const { return db_ != nullptr; }
    QString error() const { return err_; }
    int maxZoom() const { return maxZoom_; }
    // XYZ scheme (y down), converted to MBTiles TMS rows internally. Returns a null pixmap when the database has no such tile.
    QPixmap tile(int z, int x, int y);
    bool has(int z, int x, int y);
private:
    sqlite3* db_ = nullptr;
    QString err_;
    int maxZoom_ = 12;
    QCache<quint64, QPixmap> cache_{512};
    QMutex mu_;
};
