#include "tiles.h"

#include <sqlite3.h>

TileStore::TileStore(const QString& path) {
    if (sqlite3_open_v2(path.toUtf8().constData(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        err_ = QString::fromUtf8(sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT max(zoom_level) FROM tiles", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) maxZoom_ = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
}

TileStore::~TileStore() { if (db_) sqlite3_close(db_); }

static quint64 key(int z, int x, int y) { return ((quint64)z << 56) | ((quint64)x << 28) | (quint64)y; }

QPixmap TileStore::tile(int z, int x, int y) {
    if (!db_ || z < 0 || x < 0 || y < 0 || x >= (1 << z) || y >= (1 << z)) return QPixmap();
    QMutexLocker l(&mu_);
    quint64 k = key(z, x, y);
    if (QPixmap* p = cache_.object(k)) return *p;
    sqlite3_stmt* st = nullptr;
    QPixmap out;
    if (sqlite3_prepare_v2(db_, "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, z);
        sqlite3_bind_int(st, 2, x);
        sqlite3_bind_int(st, 3, (1 << z) - 1 - y);                   // MBTiles rows are TMS (y up)
        if (sqlite3_step(st) == SQLITE_ROW) {
            const void* b = sqlite3_column_blob(st, 0);
            int n = sqlite3_column_bytes(st, 0);
            if (b && n > 0) out.loadFromData((const uchar*)b, (uint)n);
        }
    }
    sqlite3_finalize(st);
    cache_.insert(k, new QPixmap(out));                              // also caches "no such tile" (null pixmap)
    return out;
}

bool TileStore::has(int z, int x, int y) { return !tile(z, x, y).isNull(); }
