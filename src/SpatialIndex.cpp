#include "SpatialIndex.h"

#include <QtMath>

#include <algorithm>

quint64 SpatialIndex::cellKey(int x, int y) const
{
    return (quint64(quint32(y)) << 32) | quint32(x);
}

int SpatialIndex::cellX(double worldX) const
{
    if (m_bounds.width() <= 0.0) {
        return 0;
    }
    return qBound(0, int((worldX - m_bounds.left())
                         / m_bounds.width() * m_columns),
                  m_columns - 1);
}

int SpatialIndex::cellY(double worldY) const
{
    if (m_bounds.height() <= 0.0) {
        return 0;
    }
    return qBound(0, int((worldY - m_bounds.top())
                         / m_bounds.height() * m_rows),
                  m_rows - 1);
}

void SpatialIndex::rebuild(const CadDocument &document)
{
    m_cells.clear();
    m_boxes.clear();
    m_oversized.clear();
    m_boxes.reserve(document.entities.size());
    m_bounds = document.bounds.adjusted(-1.0, -1.0, 1.0, 1.0);
    const int count = document.entities.size();
    m_columns = qBound(8, int(std::ceil(std::sqrt(qMax(1, count)))), 256);
    m_rows = m_columns;
    m_gridSize = QSize(m_columns, m_rows);
    for (int i = 0; i < count; ++i) {
        const QRectF box = document.entities[i].bounds;
        m_boxes.append(box);
        if (!box.isValid()) {
            continue;
        }
        const int left = cellX(box.left());
        const int right = cellX(box.right());
        const int top = cellY(box.top());
        const int bottom = cellY(box.bottom());
        const int cellCount = (right - left + 1) * (bottom - top + 1);
        if (cellCount > 1024) {
            m_oversized.append(i);
            continue;
        }
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                m_cells[cellKey(x, y)].append(i);
            }
        }
    }
    m_seen.fill(0, count);
    m_queryStamp = 0;
}

QVector<int> SpatialIndex::query(const QRectF &area, int limit) const
{
    QVector<int> result;
    if (m_boxes.isEmpty() || limit <= 0) {
        return result;
    }
    result.reserve(qMin(limit, 256));
    ++m_queryStamp;
    if (m_queryStamp == 0) {
        m_seen.fill(0);
        m_queryStamp = 1;
    }
    const QRectF clipped = area.intersected(m_bounds);
    if (!clipped.isValid()) {
        return result;
    }
    const int left = cellX(clipped.left());
    const int right = cellX(clipped.right());
    const int top = cellY(clipped.top());
    const int bottom = cellY(clipped.bottom());
    const auto appendUnique = [&](int index) {
        if (index < 0 || index >= m_boxes.size()
            || m_seen[index] == m_queryStamp) {
            return false;
        }
        m_seen[index] = m_queryStamp;
        if (!m_boxes[index].intersects(clipped)) {
            return false;
        }
        result.append(index);
        return result.size() >= limit;
    };
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const auto found = m_cells.constFind(cellKey(x, y));
            if (found == m_cells.cend()) {
                continue;
            }
            for (int index : found.value()) {
                if (appendUnique(index)) {
                    return result;
                }
            }
        }
    }
    for (int index : m_oversized) {
        if (appendUnique(index)) {
            break;
        }
    }
    return result;
}
