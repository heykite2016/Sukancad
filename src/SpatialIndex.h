#ifndef CADLENS_SPATIALINDEX_H
#define CADLENS_SPATIALINDEX_H

#include "CadModel.h"

#include <QHash>
#include <QRectF>
#include <QVector>

class SpatialIndex {
public:
    void rebuild(const CadDocument &document);
    QVector<int> query(const QRectF &area, int limit = 8000) const;
    bool isEmpty() const { return m_boxes.isEmpty(); }

private:
    quint64 cellKey(int x, int y) const;
    int cellX(double worldX) const;
    int cellY(double worldY) const;

    QRectF m_bounds;
    QVector<QRectF> m_boxes;
    QVector<int> m_oversized;
    QHash<quint64, QVector<int>> m_cells;
    QSize m_gridSize;
    int m_columns = 1;
    int m_rows = 1;
    mutable QVector<quint32> m_seen;
    mutable quint32 m_queryStamp = 0;
};

#endif
