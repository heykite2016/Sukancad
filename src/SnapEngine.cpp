#include "SnapEngine.h"

#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

int priority(SnapKind kind)
{
    switch (kind) {
    case SnapKind::Endpoint:
        return 100;
    case SnapKind::Intersection:
        return 95;
    case SnapKind::Center:
        return 90;
    case SnapKind::Midpoint:
        return 85;
    case SnapKind::Quadrant:
        return 80;
    case SnapKind::Perpendicular:
        return 70;
    case SnapKind::Nearest:
        return 20;
    case SnapKind::None:
        break;
    }
    return 0;
}

bool segmentIntersection(const QLineF &a, const QLineF &b, QPointF *point)
{
    const QPointF p(a.p1());
    const QPointF r(a.p2() - a.p1());
    const QPointF q(b.p1());
    const QPointF s(b.p2() - b.p1());
    const double cross = r.x() * s.y() - r.y() * s.x();
    if (qAbs(cross) < 1.0e-10) {
        return false;
    }
    const QPointF qp = q - p;
    const double t = (qp.x() * s.y() - qp.y() * s.x()) / cross;
    const double u = (qp.x() * r.y() - qp.y() * r.x()) / cross;
    if (t < -1.0e-8 || t > 1.0 + 1.0e-8 || u < -1.0e-8
        || u > 1.0 + 1.0e-8) {
        return false;
    }
    *point = p + r * t;
    return true;
}

QPointF closestPointOnSegment(const QLineF &line, const QPointF &cursor)
{
    const QPointF delta = line.p2() - line.p1();
    const double lengthSquared = QPointF::dotProduct(delta, delta);
    if (lengthSquared <= 1.0e-16) {
        return line.p1();
    }
    const double t = qBound(0.0, QPointF::dotProduct(cursor - line.p1(),
                                                     delta)
                                     / lengthSquared,
                            1.0);
    return line.p1() + delta * t;
}

} // namespace

QString snapKindLabel(SnapKind kind)
{
    switch (kind) {
    case SnapKind::Endpoint:
        return QStringLiteral("端点");
    case SnapKind::Midpoint:
        return QStringLiteral("中点");
    case SnapKind::Intersection:
        return QStringLiteral("交点");
    case SnapKind::Perpendicular:
        return QStringLiteral("垂足");
    case SnapKind::Center:
        return QStringLiteral("圆心");
    case SnapKind::Quadrant:
        return QStringLiteral("象限点");
    case SnapKind::Nearest:
        return QStringLiteral("最近点");
    case SnapKind::None:
        break;
    }
    return QString();
}

void SnapEngine::setDocument(const CadDocument *document,
                             const SpatialIndex *index)
{
    m_document = document;
    m_index = index;
    m_layerVisibility.clear();
    if (m_document) {
        for (const CadLayer &layer : m_document->layers) {
            m_layerVisibility.insert(layer.name, layer.visible);
        }
    }
}

void SnapEngine::consider(QVector<Candidate> &candidates,
                          const QPointF &cursor, const QPointF &point,
                          SnapKind kind, double radius) const
{
    const double dx = point.x() - cursor.x();
    const double dy = point.y() - cursor.y();
    const double distance = std::hypot(dx, dy);
    if (distance <= radius) {
        candidates.append({point, kind, distance});
    }
}

SnapResult SnapEngine::findSnap(const QPointF &cursor, double radius,
                                const SnapModes &modes) const
{
    SnapResult result;
    if (!m_document || !m_index || radius <= 0.0) {
        return result;
    }
    const QRectF searchArea(cursor.x() - radius, cursor.y() - radius,
                            radius * 2.0, radius * 2.0);
    const QVector<int> indices = m_index->query(searchArea, 4000);
    QVector<Candidate> candidates;
    candidates.reserve(256);

    struct LocalSegment {
        QLineF line;
        int entityIndex;
    };
    QVector<LocalSegment> localSegments;
    localSegments.reserve(256);

    for (int index : indices) {
        if (index < 0 || index >= m_document->entities.size()) {
            continue;
        }
        const CadEntity &entity = m_document->entities[index];
        if (!m_layerVisibility.value(entity.layer, true)) {
            continue;
        }
        if (!isRenderableCadEntity(entity)) {
            continue;
        }
        if (entity.type == CadEntityType::Point && !entity.points.isEmpty()) {
            consider(candidates, cursor, entity.points.first(),
                     SnapKind::Endpoint, radius);
            continue;
        }
        if (entity.type == CadEntityType::Text) {
            continue;
        }
        if (modes.endpoint) {
            for (const QPointF &point : entity.points) {
                consider(candidates, cursor, point, SnapKind::Endpoint,
                         radius);
            }
        }
        if (modes.center
            && (entity.type == CadEntityType::Circle
                || entity.type == CadEntityType::Arc)) {
            consider(candidates, cursor, entity.center, SnapKind::Center,
                     radius);
        }
        if (modes.center
            && (entity.type == CadEntityType::Circle
                || entity.type == CadEntityType::Arc)) {
            const QPointF quadrants[] = {
                entity.center + QPointF(entity.radius, 0.0),
                entity.center + QPointF(0.0, entity.radius),
                entity.center - QPointF(entity.radius, 0.0),
                entity.center - QPointF(0.0, entity.radius)
            };
            for (const QPointF &point : quadrants) {
                consider(candidates, cursor, point, SnapKind::Quadrant,
                         radius);
            }
        }

        const QVector<QLineF> &segments = entity.segments();
        for (const QLineF &segment : segments) {
            const double segmentLength = segment.length();
            if (segmentLength < 1.0e-12) {
                continue;
            }
            QRectF segmentBox(segment.p1(), segment.p2());
            segmentBox = segmentBox.normalized();
            if (segmentBox.adjusted(-radius, -radius, radius, radius)
                    .contains(cursor)) {
                localSegments.append({segment, index});
            }
            if (modes.midpoint
                && (entity.type == CadEntityType::Segment
                    || segmentLength > radius * 2.0)) {
                consider(candidates, cursor,
                         (segment.p1() + segment.p2()) * 0.5,
                         SnapKind::Midpoint, radius);
            }
            if (modes.perpendicular) {
                const QPointF foot = closestPointOnSegment(segment, cursor);
                const double t = QLineF(segment.p1(), foot).length()
                    / segmentLength;
                if (t > 1.0e-5 && t < 1.0 - 1.0e-5) {
                    consider(candidates, cursor, foot,
                             SnapKind::Perpendicular, radius);
                }
            }
            if (modes.nearest) {
                consider(candidates, cursor,
                         closestPointOnSegment(segment, cursor),
                         SnapKind::Nearest, radius);
            }
        }
    }

    if (modes.intersection && localSegments.size() > 1) {
        const int cap = qMin(localSegments.size(), 700);
        for (int a = 0; a < cap; ++a) {
            for (int b = a + 1; b < cap; ++b) {
                if (localSegments[a].entityIndex
                    == localSegments[b].entityIndex) {
                    continue;
                }
                QPointF intersection;
                if (segmentIntersection(localSegments[a].line,
                                        localSegments[b].line,
                                        &intersection)) {
                    consider(candidates, cursor, intersection,
                             SnapKind::Intersection, radius);
                }
            }
        }
    }

    if (candidates.isEmpty()) {
        return result;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  const int aPriority = priority(a.kind);
                  const int bPriority = priority(b.kind);
                  if (aPriority != bPriority) {
                      return aPriority > bPriority;
                  }
                  return a.distance < b.distance;
              });
    result.valid = true;
    result.point = candidates.first().point;
    result.kind = candidates.first().kind;
    result.label = snapKindLabel(result.kind);
    return result;
}
