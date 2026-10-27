#ifndef CADLENS_SNAPENGINE_H
#define CADLENS_SNAPENGINE_H

#include "SpatialIndex.h"

#include <QHash>
#include <QLineF>
#include <QPointF>
#include <QString>

enum class SnapKind {
    None,
    Endpoint,
    Midpoint,
    Intersection,
    Perpendicular,
    Center,
    Quadrant,
    Nearest
};

struct SnapModes {
    bool endpoint = true;
    bool midpoint = true;
    bool intersection = true;
    bool perpendicular = true;
    bool center = true;
    bool nearest = true;
};

struct SnapResult {
    bool valid = false;
    QPointF point;
    SnapKind kind = SnapKind::None;
    QString label;
};

QString snapKindLabel(SnapKind kind);

class SnapEngine {
public:
    void setDocument(const CadDocument *document, const SpatialIndex *index);
    SnapResult findSnap(const QPointF &cursor, double radius,
                        const SnapModes &modes) const;

private:
    struct Candidate {
        QPointF point;
        SnapKind kind = SnapKind::None;
        double distance = 0.0;
    };

    void consider(QVector<Candidate> &candidates, const QPointF &cursor,
                  const QPointF &point, SnapKind kind, double radius) const;

    const CadDocument *m_document = nullptr;
    const SpatialIndex *m_index = nullptr;
    QHash<QString, bool> m_layerVisibility;
};

#endif
