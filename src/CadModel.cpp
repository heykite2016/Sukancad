#include "CadModel.h"

#include "dx_iface.h"
#include "LibreDwgReader.h"
#include "PdfDocument.h"

#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Affine2D {
    double m11 = 1.0;
    double m12 = 0.0;
    double m21 = 0.0;
    double m22 = 1.0;
    double dx = 0.0;
    double dy = 0.0;

    QPointF map(const QPointF &point) const
    {
        return QPointF(m11 * point.x() + m21 * point.y() + dx,
                       m12 * point.x() + m22 * point.y() + dy);
    }

    Affine2D then(const Affine2D &outer) const
    {
        Affine2D result;
        result.m11 = outer.m11 * m11 + outer.m21 * m12;
        result.m21 = outer.m11 * m21 + outer.m21 * m22;
        result.dx = outer.m11 * dx + outer.m21 * dy + outer.dx;
        result.m12 = outer.m12 * m11 + outer.m22 * m12;
        result.m22 = outer.m12 * m21 + outer.m22 * m22;
        result.dy = outer.m12 * dx + outer.m22 * dy + outer.dy;
        return result;
    }
};

QPointF coord(const DRW_Coord &value)
{
    return QPointF(value.x, value.y);
}

QColor aciColor(int index, const QColor &fallback = QColor(230, 236, 245))
{
    static const QColor palette[] = {
        QColor("#ffffff"), QColor("#ff5c5c"), QColor("#ffe066"),
        QColor("#65e6a7"), QColor("#66d9ef"), QColor("#6e8bff"),
        QColor("#ff7edb"), QColor("#ffffff"), QColor("#9aa7b5")
    };
    if (index >= 1 && index <= 9) {
        return palette[index - 1];
    }
    if (index >= 10) {
        const int hue = (index * 47) % 360;
        return QColor::fromHsv(hue, 125, 235);
    }
    return fallback;
}

double lineWeightMillimeters(int weight)
{
    static const double weights[] = {
        0.0, 0.05, 0.09, 0.13, 0.15, 0.18, 0.20, 0.25,
        0.30, 0.35, 0.40, 0.50, 0.53, 0.60, 0.70, 0.80,
        0.90, 1.00, 1.06, 1.20, 1.40, 1.58, 2.00, 2.11
    };
    if (weight >= 0 && weight <= 23) {
        return weights[weight];
    }
    return 0.25;
}

QString cleanMText(QString value)
{
    value.replace(QStringLiteral("\\P"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\~"), QStringLiteral(" "));
    value.replace(QRegularExpression(QStringLiteral("\\\\[A-Za-z][^;]*;")),
                  QString());
    value.remove(QLatin1Char('{'));
    value.remove(QLatin1Char('}'));
    return value.trimmed();
}

void appendBulgeArc(QVector<QPointF> &target, const QPointF &from,
                    const QPointF &to, double bulge)
{
    if (qAbs(bulge) < 1e-9) {
        target.append(to);
        return;
    }
    const QPointF delta = to - from;
    const double chord = std::hypot(delta.x(), delta.y());
    if (chord < 1e-12) {
        target.append(to);
        return;
    }
    const double theta = 4.0 * std::atan(bulge);
    const double half = theta * 0.5;
    const QPointF midpoint = (from + to) * 0.5;
    const QPointF leftNormal(-delta.y() / chord, delta.x() / chord);
    const double offset = chord * 0.5 / std::tan(half);
    const QPointF center = midpoint + leftNormal * offset;
    const double radius = std::hypot(from.x() - center.x(),
                                     from.y() - center.y());
    const double start = std::atan2(from.y() - center.y(),
                                    from.x() - center.x());
    const int steps = qBound(4, int(std::ceil(qAbs(theta) / (kPi / 24.0))),
                             256);
    for (int i = 1; i < steps; ++i) {
        const double angle = start + theta * double(i) / double(steps);
        target.append(center + QPointF(std::cos(angle), std::sin(angle))
                                  * radius);
    }
    target.append(to);
}

QVector<QPointF> sampleArc(const QPointF &center, double radius,
                           double start, double end, bool fullCircle = false)
{
    QVector<QPointF> result;
    if (radius <= 0.0) {
        return result;
    }
    if (fullCircle) {
        start = 0.0;
        end = 2.0 * kPi;
    } else {
        while (end <= start) {
            end += 2.0 * kPi;
        }
    }
    const int steps = qBound(fullCircle ? 32 : 8,
                             int(std::ceil(qAbs(end - start) / (kPi / 48.0))),
                             384);
    result.reserve(steps + (fullCircle ? 1 : 0));
    for (int i = 0; i <= steps; ++i) {
        const double angle = start + (end - start) * double(i) / double(steps);
        result.append(center + QPointF(std::cos(angle), std::sin(angle))
                                  * radius);
    }
    if (fullCircle && !result.isEmpty()) {
        result.removeLast();
    }
    return result;
}

struct CapturedLayout {
    int tabOrder = 0;
    QString name;
    std::uint32_t blockRecordHandle = 0;
};

class ModelBuilder {
public:
    explicit ModelBuilder(CadDocument &document) : m_document(document) {}

    void read(dx_data &data, dx_iface &importer, const QString &path,
              const QStringList &layouts,
              const QVector<CapturedLayout> &layoutRecords)
    {
        m_data = &data;
        m_document.path = path;
        m_document.fileName = QFileInfo(path).fileName();
        m_document.sourceFormat = QFileInfo(path).suffix().toUpper();
        m_document.sourceBackend = QStringLiteral("libdxfrw");
        m_document.formatVersion = versionLabel(data.sourceVersion);
        m_document.layouts = layouts;
        readLayers(data);
        readUnits(data);

        for (auto *block : data.blocks) {
            if (block) {
                m_blocks.insert(QString::fromStdString(block->name), block);
            }
        }
        QSet<std::uint32_t> layoutHandles;
        for (const CapturedLayout &layout : layoutRecords) {
            if (layout.blockRecordHandle != 0) {
                layoutHandles.insert(layout.blockRecordHandle);
            }
        }
        for (const CapturedLayout &layout : layoutRecords) {
            QVector<CadEntity> layoutContent;
            if (data.mBlock && layout.blockRecordHandle != 0) {
                m_targetEntities = &layoutContent;
                for (auto *entity : data.mBlock->ent) {
                    if (entity && entity->space == DRW::PaperSpace
                        && entity->parentHandle
                            == layout.blockRecordHandle) {
                        visitEntity(entity, Affine2D{}, 0);
                    }
                }
            }
            if (!layoutContent.isEmpty()) {
                m_document.layoutEntities.insert(layout.name, layoutContent);
                m_document.layoutBounds.insert(
                    layout.name, calculateBounds(layoutContent));
            }
        }
        m_targetEntities = &m_document.entities;
        if (data.mBlock) {
            for (auto *entity : data.mBlock->ent) {
                if (entity && entity->space == DRW::PaperSpace
                    && layoutHandles.contains(entity->parentHandle)) {
                    continue;
                }
                visitEntity(entity, Affine2D{}, 0);
            }
        }
        if (!layoutRecords.isEmpty()
            && !m_document.layoutEntities.contains(
                layoutRecords.first().name)
            && data.mBlock) {
            QVector<CadEntity> fallback;
            m_targetEntities = &fallback;
            for (auto *entity : data.mBlock->ent) {
                if (entity && entity->space == DRW::PaperSpace) {
                    visitEntity(entity, Affine2D{}, 0);
                }
            }
            if (!fallback.isEmpty()) {
                m_document.layoutEntities.insert(
                    layoutRecords.first().name, fallback);
                m_document.layoutBounds.insert(
                    layoutRecords.first().name,
                    calculateBounds(fallback));
            }
            m_targetEntities = &m_document.entities;
        }
        (void)importer;
        finalize();
    }

private:
    void readLayers(dx_data &data)
    {
        for (const auto &source : data.layers) {
            CadLayer layer;
            layer.name = QString::fromStdString(source.name);
            if (layer.name.isEmpty()) {
                layer.name = QStringLiteral("0");
            }
            const int aci = qAbs(source.color);
            layer.color = source.color24 >= 0
                ? QColor::fromRgb(source.color24)
                : aciColor(aci);
            layer.sourceColor = layer.color;
            layer.color = displayColorForDarkCanvas(layer.color);
            layer.visible = source.color >= 0 && (source.flags & 1) == 0;
            layer.locked = (source.flags & 4) != 0;
            layer.lineWeight = static_cast<int>(source.lWeight);
            m_document.layers.append(layer);
            m_layerLookup.insert(layer.name, m_document.layers.size() - 1);
        }
        if (!m_layerLookup.contains(QStringLiteral("0"))) {
            CadLayer layer;
            layer.name = QStringLiteral("0");
            m_document.layers.prepend(layer);
            rebuildLayerLookup();
        }
    }

    void rebuildLayerLookup()
    {
        m_layerLookup.clear();
        for (int i = 0; i < m_document.layers.size(); ++i) {
            m_layerLookup.insert(m_document.layers[i].name, i);
        }
    }

    int ensureLayer(const QString &name)
    {
        const QString normalized = name.isEmpty() ? QStringLiteral("0") : name;
        const auto found = m_layerLookup.constFind(normalized);
        if (found != m_layerLookup.cend()) {
            return found.value();
        }
        CadLayer layer;
        layer.name = normalized;
        layer.color = aciColor((m_document.layers.size() % 8) + 1);
        layer.sourceColor = layer.color;
        m_document.layers.append(layer);
        const int index = m_document.layers.size() - 1;
        m_layerLookup.insert(normalized, index);
        return index;
    }

    void readUnits(dx_data &data)
    {
        int units = 0;
        const auto unitsVar =
            data.headerC.vars.find(std::string("INSUNITS"));
        if (unitsVar != data.headerC.vars.end() && unitsVar->second
            && (unitsVar->second->type() == DRW_Variant::INTEGER
                || unitsVar->second->type() == DRW_Variant::INTEGER64)) {
            units = int(unitsVar->second->i_val());
        }
        m_document.sourceUnit = measuringUnitFromInsUnits(units);
        m_document.metersPerDrawingUnit =
            measuringUnitMeters(m_document.sourceUnit);
        if (!std::isfinite(m_document.metersPerDrawingUnit)
            || m_document.metersPerDrawingUnit <= 0.0) {
            m_document.sourceUnit = MeasuringUnit::Millimeter;
            m_document.metersPerDrawingUnit = 0.001;
        }
        const auto readCoord = [&data](const char *name, QPointF *point) {
            auto found = data.headerC.vars.find(std::string(name));
            if (found == data.headerC.vars.end() || !found->second
                || found->second->type() != DRW_Variant::COORD
                || !found->second->coord()) {
                return false;
            }
            *point = QPointF(found->second->coord()->x,
                             found->second->coord()->y);
            return true;
        };
        QPointF minimum;
        QPointF maximum;
        if (readCoord("EXTMIN", &minimum)
            && readCoord("EXTMAX", &maximum)) {
            const QRectF extents(minimum, maximum);
            if (extents.isValid() && extents.width() > 0.0
                && extents.height() > 0.0) {
                m_document.declaredBounds = extents.normalized();
            }
        }
    }

    CadEntity baseEntity(const DRW_Entity *source, const Affine2D &transform)
    {
        CadEntity output;
        output.layer = QString::fromStdString(source->layer);
        const int layerIndex = ensureLayer(output.layer);
        const CadLayer &cadLayer = m_document.layers[layerIndex];
        output.sourceColor = source->color24 >= 0
            ? QColor::fromRgb(source->color24)
            : (source->color == DRW::ColorByLayer
                   ? (cadLayer.sourceColor.isValid()
                          ? cadLayer.sourceColor : cadLayer.color)
                   : aciColor(source->color));
        output.color = output.sourceColor;
        output.color = displayColorForDarkCanvas(output.color);
        output.lineWeight = source->lWeight == DRW_LW_Conv::widthByLayer
            ? cadLayer.lineWeight
            : static_cast<int>(source->lWeight);
        if (output.lineWeight < 0
            || output.lineWeight > static_cast<int>(
                DRW_LW_Conv::widthByBlock)) {
            output.lineWeight = 31;
        }
        m_document.layers[layerIndex].entityCount++;
        (void)transform;
        return output;
    }

    void appendEntity(CadEntity entity, const Affine2D &transform)
    {
        const double scaleX = std::hypot(transform.m11, transform.m12);
        const double scaleY = std::hypot(transform.m21, transform.m22);
        const double uniformScale = (scaleX + scaleY) * 0.5;
        for (QPointF &point : entity.points) {
            point = transform.map(point);
        }
        for (CadDimensionEdge &edge : entity.dimensionEdges) {
            edge.midpoint = transform.map(edge.midpoint);
            edge.length *= uniformScale;
        }
        entity.center = transform.map(entity.center);
        entity.textPosition = transform.map(entity.textPosition);
        entity.textAlignmentPoint =
            transform.map(entity.textAlignmentPoint);
        entity.radius *= uniformScale;
        entity.textHeight *= uniformScale;
        if (scaleX > 1.0e-12) {
            entity.textWidthScale *= scaleY / scaleX;
        }
        entity.textRotation +=
            qRadiansToDegrees(std::atan2(transform.m12, transform.m11));
        entity.updateBounds();
        if (!entity.bounds.isValid() || entity.points.isEmpty()
            && entity.type != CadEntityType::Text) {
            return;
        }
        m_targetEntities->append(std::move(entity));
    }

    void visitEntity(const DRW_Entity *source, const Affine2D &transform,
                     int depth)
    {
        if (!source || depth > 24 || !source->visible) {
            return;
        }
        switch (source->eType) {
        case DRW::LINE:
        case DRW::THREEDLINE: {
            const auto *line = dynamic_cast<const DRW_Line *>(source);
            if (!line) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Segment;
            out.points = {coord(line->basePoint), coord(line->secPoint)};
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::RAY:
        case DRW::XLINE: {
            // Construction rays have no finite model extent. Rendering them
            // with an arbitrary huge length corrupts zoom-to-extents for real
            // drawings, so they are intentionally omitted from this viewer.
            break;
        }
        case DRW::CIRCLE: {
            const auto *circle = dynamic_cast<const DRW_Circle *>(source);
            if (!circle) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Circle;
            out.center = coord(circle->basePoint);
            out.radius = circle->radious;
            out.points = sampleArc(out.center, out.radius, 0.0, 2.0 * kPi,
                                   true);
            out.closed = true;
            if (out.radius > 0.0) {
                out.dimensionEdges.append(
                    {out.center + QPointF(out.radius, 0.0),
                     2.0 * kPi * out.radius, true});
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::ARC: {
            const auto *arc = dynamic_cast<const DRW_Arc *>(source);
            if (!arc) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Arc;
            out.center = coord(arc->basePoint);
            out.radius = arc->radious;
            out.startAngle = arc->staangle;
            out.endAngle = arc->endangle;
            out.points = sampleArc(out.center, out.radius, out.startAngle,
                                   out.endAngle);
            double sweep = out.endAngle - out.startAngle;
            while (sweep <= 0.0) {
                sweep += 2.0 * kPi;
            }
            if (out.radius > 0.0) {
                const double middle =
                    out.startAngle + sweep * 0.5;
                out.dimensionEdges.append(
                    {out.center
                         + QPointF(std::cos(middle), std::sin(middle))
                             * out.radius,
                     out.radius * sweep, true});
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::ELLIPSE: {
            const auto *ellipse = dynamic_cast<const DRW_Ellipse *>(source);
            if (!ellipse) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Ellipse;
            const QPointF center = coord(ellipse->basePoint);
            const QPointF major = coord(ellipse->secPoint);
            const QPointF minor(-major.y() * ellipse->ratio,
                                major.x() * ellipse->ratio);
            double start = ellipse->staparam;
            double end = ellipse->endparam;
            if (qAbs(end - start) < 1e-10) {
                start = 0.0;
                end = 2.0 * kPi;
                out.closed = true;
            } else {
                while (end <= start) {
                    end += 2.0 * kPi;
                }
            }
            const int steps = qBound(16, int(std::ceil(
                qAbs(end - start) / (kPi / 48.0))), 384);
            for (int i = 0; i <= steps; ++i) {
                const double t = start + (end - start) * double(i) / steps;
                out.points.append(center + major * std::cos(t)
                                  + minor * std::sin(t));
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::LWPOLYLINE: {
            const auto *poly = dynamic_cast<const DRW_LWPolyline *>(source);
            if (!poly || poly->vertlist.empty()) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Polyline;
            out.closed = (poly->flags & 1) != 0;
            QPointF previous;
            bool havePrevious = false;
            double previousBulge = 0.0;
            for (const auto &vertex : poly->vertlist) {
                if (!vertex) {
                    continue;
                }
                const QPointF current(vertex->x, vertex->y + poly->elevation);
                if (!havePrevious) {
                    out.points.append(current);
                } else {
                    if (qAbs(previousBulge) > 1e-9) {
                        const QPointF delta = current - previous;
                        const double chord =
                            std::hypot(delta.x(), delta.y());
                        const double theta = 4.0 * std::atan(previousBulge);
                        const QPointF midpoint = (previous + current) * 0.5;
                        const QPointF normal(-delta.y() / chord,
                                             delta.x() / chord);
                        const QPointF center =
                            midpoint
                            + normal * (chord * 0.5
                                        / std::tan(theta * 0.5));
                        const double radius =
                            std::hypot(previous.x() - center.x(),
                                       previous.y() - center.y());
                        const double start = std::atan2(
                            previous.y() - center.y(),
                            previous.x() - center.x());
                        const double middle = start + theta * 0.5;
                        out.dimensionEdges.append(
                            {center
                                 + QPointF(std::cos(middle),
                                           std::sin(middle))
                                     * radius,
                             radius * qAbs(theta), true});
                    } else {
                        out.dimensionEdges.append(
                            {(previous + current) * 0.5,
                             QLineF(previous, current).length(), false});
                    }
                    appendBulgeArc(out.points, previous, current,
                                   previousBulge);
                }
                previous = current;
                previousBulge = vertex->bulge;
                havePrevious = true;
            }
            if (out.closed && out.points.size() > 2) {
                const double closingBulge =
                    poly->vertlist.back() ? poly->vertlist.back()->bulge : 0.0;
                if (!out.points.isEmpty()) {
                    if (qAbs(closingBulge) > 1e-9) {
                        const QPointF delta =
                            out.points.first() - out.points.last();
                        const double chord =
                            std::hypot(delta.x(), delta.y());
                        const double theta =
                            4.0 * std::atan(closingBulge);
                        const QPointF midpoint =
                            (out.points.last() + out.points.first())
                            * 0.5;
                        const QPointF normal(-delta.y() / chord,
                                             delta.x() / chord);
                        const QPointF center =
                            midpoint
                            + normal * (chord * 0.5
                                        / std::tan(theta * 0.5));
                        const double radius =
                            std::hypot(out.points.last().x() - center.x(),
                                       out.points.last().y() - center.y());
                        const double start = std::atan2(
                            out.points.last().y() - center.y(),
                            out.points.last().x() - center.x());
                        const double middle = start + theta * 0.5;
                        out.dimensionEdges.append(
                            {center
                                 + QPointF(std::cos(middle),
                                           std::sin(middle))
                                     * radius,
                             radius * qAbs(theta), true});
                    } else {
                        out.dimensionEdges.append(
                            {(out.points.last() + out.points.first())
                                 * 0.5,
                             QLineF(out.points.last(), out.points.first())
                                 .length(),
                             false});
                    }
                }
                if (qAbs(closingBulge) > 1e-9) {
                    appendBulgeArc(out.points, out.points.back(),
                                   out.points.first(), closingBulge);
                }
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::POLYLINE: {
            const auto *poly = dynamic_cast<const DRW_Polyline *>(source);
            if (!poly || poly->vertlist.empty()) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Polyline;
            out.closed = (poly->flags & 1) != 0;
            QPointF previous;
            bool havePrevious = false;
            for (const auto &vertex : poly->vertlist) {
                if (!vertex) {
                    continue;
                }
                const QPointF current = coord(vertex->basePoint);
                if (!havePrevious) {
                    out.points.append(current);
                } else {
                    appendBulgeArc(out.points, previous, current,
                                   vertex->bulge);
                }
                previous = current;
                havePrevious = true;
            }
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::SPLINE: {
            const auto *spline = dynamic_cast<const DRW_Spline *>(source);
            if (!spline) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Polyline;
            if (!spline->fitlist.empty()) {
                for (const auto &point : spline->fitlist) {
                    if (point) {
                        out.points.append(coord(*point));
                    }
                }
            } else {
                for (const auto &point : spline->controllist) {
                    if (point) {
                        out.points.append(coord(*point));
                    }
                }
            }
            out.closed = (spline->flags & 1) != 0;
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::POINT: {
            // POINT is commonly a dimension definition or block control
            // point. It is not drawing geometry unless the viewer implements
            // PDMODE/PDSIZE display semantics.
            break;
        }
        case DRW::TEXT: {
            const auto *text = dynamic_cast<const DRW_Text *>(source);
            if (!text) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Text;
            out.text = QString::fromStdString(text->text);
            out.textPosition = coord(text->basePoint);
            out.points = {out.textPosition};
            out.textHeight = qMax(0.1, text->height);
            out.textRotation = text->angle;
            out.textWidthScale = qMax(0.05, text->widthscale);
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::MTEXT: {
            const auto *text = dynamic_cast<const DRW_MText *>(source);
            if (!text) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Text;
            out.text = cleanMText(QString::fromStdString(text->text));
            out.textPosition = coord(text->basePoint);
            out.points = {out.textPosition};
            out.textHeight = qMax(0.1, text->height);
            out.textRotation = text->angle;
            out.textWidthScale = qMax(0.05, text->widthscale);
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::SOLID:
        case DRW::E3DFACE:
        case DRW::DXF_TRACE: {
            const auto *face = dynamic_cast<const DRW_Trace *>(source);
            if (!face) {
                break;
            }
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Solid;
            out.filled = true;
            out.closed = true;
            out.points = {
                coord(face->basePoint), coord(face->secPoint),
                coord(face->fourPoint), coord(face->thirdPoint)
            };
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::HATCH:
        case DRW::MPOLYGON: {
            const auto *hatch = dynamic_cast<const DRW_Hatch *>(source);
            if (!hatch) {
                break;
            }
            QVector<QVector<QPointF>> loops;
            for (const auto &loop : hatch->looplist) {
                if (!loop) {
                    continue;
                }
                QVector<QPointF> loopPoints;
                for (const auto &edge : loop->objlist) {
                    if (!edge) {
                        continue;
                    }
                    if (const auto *line =
                            dynamic_cast<const DRW_Line *>(edge.get())) {
                        if (loopPoints.isEmpty()
                            || QLineF(loopPoints.last(),
                                      coord(line->basePoint)).length()
                                > 1.0e-9) {
                            loopPoints.append(coord(line->basePoint));
                        }
                        loopPoints.append(coord(line->secPoint));
                    } else if (const auto *arc =
                                   dynamic_cast<const DRW_Arc *>(edge.get())) {
                        const QPointF center = coord(arc->basePoint);
                        const QVector<QPointF> sampled =
                            sampleArc(center, arc->radious, arc->staangle,
                                      arc->endangle);
                        for (const QPointF &point : sampled) {
                            if (loopPoints.isEmpty()
                                || QLineF(loopPoints.last(), point).length()
                                    > 1.0e-9) {
                                loopPoints.append(point);
                            }
                        }
                    } else if (const auto *ellipse =
                                   dynamic_cast<const DRW_Ellipse *>(
                                       edge.get())) {
                        const QPointF center = coord(ellipse->basePoint);
                        const QPointF major = coord(ellipse->secPoint);
                        const QPointF minor(
                            -major.y() * ellipse->ratio,
                            major.x() * ellipse->ratio);
                        for (int i = 0; i <= 96; ++i) {
                            const double t =
                                2.0 * kPi * double(i) / 96.0;
                            loopPoints.append(center + major * std::cos(t)
                                              + minor * std::sin(t));
                        }
                    } else if (const auto *polyline =
                                   dynamic_cast<const DRW_LWPolyline *>(
                                       edge.get())) {
                        for (const auto &vertex : polyline->vertlist) {
                            if (vertex) {
                                loopPoints.append(
                                    QPointF(vertex->x, vertex->y));
                            }
                        }
                    }
                }
                if (loopPoints.size() > 2) {
                    if (QLineF(loopPoints.first(), loopPoints.last()).length()
                        < 1.0e-9) {
                        loopPoints.removeLast();
                    }
                    loops.append(loopPoints);
                }
            }
            if (loops.isEmpty()) {
                break;
            }
            std::sort(loops.begin(), loops.end(),
                      [](const QVector<QPointF> &a,
                         const QVector<QPointF> &b) {
                          QRectF ba;
                          QRectF bb;
                          for (const QPointF &point : a) {
                              ba = ba.united(QRectF(point, point));
                          }
                          for (const QPointF &point : b) {
                              bb = bb.united(QRectF(point, point));
                          }
                          return ba.width() * ba.height()
                              > bb.width() * bb.height();
                      });
            CadEntity out = baseEntity(source, transform);
            out.type = CadEntityType::Solid;
            out.closed = true;
            out.filled = true;
            out.points = loops.first();
            appendEntity(std::move(out), transform);
            break;
        }
        case DRW::INSERT: {
            const auto *insert = dynamic_cast<const DRW_Insert *>(source);
            if (!insert) {
                break;
            }
            const auto blockIt =
                m_blocks.constFind(QString::fromStdString(insert->name));
            if (blockIt == m_blocks.cend() || !blockIt.value()) {
                break;
            }
            const auto *block = blockIt.value();
            const double cosine = std::cos(insert->angle);
            const double sine = std::sin(insert->angle);
            Affine2D local;
            local.m11 = cosine * insert->xscale;
            local.m12 = sine * insert->xscale;
            local.m21 = -sine * insert->yscale;
            local.m22 = cosine * insert->yscale;
            local.dx = insert->basePoint.x
                - local.m11 * block->basePoint.x
                - local.m21 * block->basePoint.y;
            local.dy = insert->basePoint.y
                - local.m12 * block->basePoint.x
                - local.m22 * block->basePoint.y;
            const Affine2D combined = local.then(transform);
            const int columns = qBound(1, insert->colcount, 256);
            const int rows = qBound(1, insert->rowcount, 256);
            for (int row = 0; row < rows; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const double localX = column * insert->colspace;
                    const double localY = row * insert->rowspace;
                    Affine2D arrayOffset;
                    arrayOffset.dx = cosine * localX - sine * localY;
                    arrayOffset.dy = sine * localX + cosine * localY;
                    const Affine2D arrayTransform = arrayOffset.then(combined);
                    for (auto *child : block->ent) {
                        visitEntity(child, arrayTransform, depth + 1);
                    }
                }
            }
            break;
        }
        default:
            break;
        }
    }

    void finalize()
    {
        m_document.sanitizeRenderableEntities();
        QRectF bounds = m_document.declaredBounds.isValid()
            ? m_document.declaredBounds
            : calculateBounds(m_document.entities);
        if (!bounds.isValid() || bounds.width() < 1e-9
            || bounds.height() < 1e-9) {
            const double side = qMax(1.0, bounds.width());
            bounds = QRectF(bounds.center().x() - side,
                            bounds.center().y() - side, side * 2.0,
                            side * 2.0);
        }
        m_document.bounds = bounds;
        m_document.recountLayers();
    }

    static QRectF calculateBounds(const QVector<CadEntity> &entities)
    {
        QRectF bounds;
        for (const CadEntity &entity : entities) {
            if (!entity.bounds.isValid()) {
                continue;
            }
            bounds = bounds.isNull() ? entity.bounds
                                     : bounds.united(entity.bounds);
        }
        if (!bounds.isValid()) {
            return bounds;
        }
        return bounds.adjusted(-bounds.width() * 0.03,
                               -bounds.height() * 0.03,
                               bounds.width() * 0.03,
                               bounds.height() * 0.03);
    }

    static QString versionLabel(DRW::Version version)
    {
        for (const auto &entry : DRW::dwgVersionStrings) {
            if (entry.second == version && entry.first) {
                return QString::fromLatin1(entry.first);
            }
        }
        return QStringLiteral("AutoCAD");
    }

    CadDocument &m_document;
    QVector<CadEntity> *m_targetEntities = &m_document.entities;
    dx_data *m_data = nullptr;
    QHash<QString, dx_ifaceBlock *> m_blocks;
    QHash<QString, int> m_layerLookup;
};

class LayoutAwareInterface : public dx_iface {
public:
    void addBlock(const DRW_Block &block) override
    {
        dx_iface::addBlock(block);
        if (currentBlock) {
            blocksByHandle.insert(static_cast<int>(block.handle),
                                  currentBlock);
        }
    }

    void setBlock(const int handle) override
    {
        const auto found = blocksByHandle.constFind(handle);
        if (found != blocksByHandle.cend()) {
            currentBlock = found.value();
        } else {
            currentBlock = cData ? cData->mBlock : nullptr;
        }
    }

    void addLayout(const DRW_Layout &layout) override
    {
        const QString name = QString::fromStdString(layout.name).trimmed();
        if (!name.isEmpty() && name.compare(QStringLiteral("Model"),
                                           Qt::CaseInsensitive)
                != 0
            && std::none_of(layoutRecords.cbegin(), layoutRecords.cend(),
                            [&name](const CapturedLayout &record) {
                                return record.name == name;
                            })) {
            layoutRecords.append(
                {layout.tabOrder, name, layout.paperSpaceBlockRecordHandle.ref});
        }
    }

    void finishLayouts()
    {
        std::sort(layoutRecords.begin(), layoutRecords.end(),
                  [](const auto &a, const auto &b) {
                      if (a.tabOrder != b.tabOrder) {
                          return a.tabOrder < b.tabOrder;
                      }
                      return a.name.size() < b.name.size();
                  });
        layouts.clear();
        for (const auto &record : layoutRecords) {
            layouts.append(record.name);
        }
    }

    QStringList layouts;
    QVector<CapturedLayout> layoutRecords;
    QHash<int, dx_ifaceBlock *> blocksByHandle;
};

} // namespace

QColor displayColorForDarkCanvas(const QColor &source)
{
    if (!source.isValid()) {
        return QColor(QStringLiteral("#f2f4f5"));
    }
    const auto linear = [](double value) {
        value /= 255.0;
        return value <= 0.04045
            ? value / 12.92
            : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const auto luminance = [&linear](const QColor &color) {
        return 0.2126 * linear(color.red())
            + 0.7152 * linear(color.green())
            + 0.0722 * linear(color.blue());
    };
    if (luminance(source) >= 0.12) {
        return source;
    }
    QColor adjusted = source;
    for (double amount = 0.2; amount <= 0.75; amount += 0.05) {
        adjusted.setRed(qRound(source.red() * (1.0 - amount) + 255 * amount));
        adjusted.setGreen(
            qRound(source.green() * (1.0 - amount) + 255 * amount));
        adjusted.setBlue(
            qRound(source.blue() * (1.0 - amount) + 255 * amount));
        if (luminance(adjusted) >= 0.18) {
            adjusted.setAlpha(source.alpha());
            return adjusted;
        }
    }
    adjusted.setAlpha(source.alpha());
    return adjusted;
}

void CadEntity::updateBounds()
{
    segmentCacheValid = false;
    bounds = QRectF();
    if (type == CadEntityType::Text) {
        const int lines = qMax(1, text.count(QLatin1Char('\n')) + 1);
        double width = qMax(1, text.size()) * textHeight * 0.62
            * textWidthScale;
        if (textFit) {
            width = qMax(
                width,
                QLineF(textPosition, textAlignmentPoint).length());
        }
        const double height = textHeight * lines * 1.2;
        double left = textPosition.x();
        double top = textPosition.y() - height * 0.2;
        if (textAttachment >= 1 && textAttachment <= 9) {
            const int column = (textAttachment - 1) % 3;
            const int row = (textAttachment - 1) / 3;
            left -= column == 1 ? width * 0.5
                    : column == 2 ? width : 0.0;
            top = row == 0 ? textPosition.y() - height
                  : row == 1 ? textPosition.y() - height * 0.5
                             : textPosition.y();
        } else {
            left -= textHAlign == 1 || textHAlign == 4
                ? width * 0.5
                : textHAlign == 2 ? width : 0.0;
            top = textHAlign == 4 || textVAlign == 2
                ? textPosition.y() - height * 0.5
                : textVAlign == 3 ? textPosition.y() - height
                : textVAlign == 1 ? textPosition.y()
                                  : textPosition.y() - height * 0.2;
        }
        bounds = QRectF(left, top, width, height);
        return;
    }
    if (points.isEmpty()) {
        return;
    }
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (const QPointF &point : points) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            continue;
        }
        minX = qMin(minX, point.x());
        minY = qMin(minY, point.y());
        maxX = qMax(maxX, point.x());
        maxY = qMax(maxY, point.y());
    }
    if (minX <= maxX && minY <= maxY) {
        bounds = QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
        const double epsilon =
            qMax(1.0e-9, qMax(bounds.width(), bounds.height()) * 1.0e-9);
        if (bounds.width() < epsilon) {
            bounds.setLeft(bounds.left() - epsilon);
            bounds.setRight(bounds.right() + epsilon);
        }
        if (bounds.height() < epsilon) {
            bounds.setTop(bounds.top() - epsilon);
            bounds.setBottom(bounds.bottom() + epsilon);
        }
    }
}

const QVector<QLineF> &CadEntity::segments() const
{
    if (segmentCacheValid) {
        return segmentCache;
    }
    segmentCache.clear();
    segmentCacheValid = true;
    if (points.size() < 2) {
        return segmentCache;
    }
    if (type == CadEntityType::Text || type == CadEntityType::Point) {
        return segmentCache;
    }
    segmentCache.reserve(points.size() + (closed ? 1 : 0));
    for (int i = 1; i < points.size(); ++i) {
        segmentCache.append(QLineF(points[i - 1], points[i]));
    }
    if (closed) {
        segmentCache.append(QLineF(points.last(), points.first()));
    }
    return segmentCache;
}

QRectF cadInitialViewBounds(const QVector<CadEntity> &entities,
                           const QRectF &drawingBounds)
{
    // This is only a camera hint. The complete drawing bounds remain intact
    // so show-all continues to include every renderable entity.
    const QRectF fullBounds = drawingBounds.normalized();
    constexpr int gridSize = 32;
    constexpr int minimumEntityCount = 64;
    constexpr int cellCount = gridSize * gridSize;
    if (!fullBounds.isValid() || fullBounds.width() <= 0.0
        || fullBounds.height() <= 0.0
        || entities.size() < minimumEntityCount) {
        return fullBounds;
    }

    QVector<quint32> cellCounts(cellCount, 0);
    int validEntityCount = 0;
    for (const CadEntity &entity : entities) {
        if (!entity.bounds.isValid()) {
            continue;
        }
        const QPointF center = entity.bounds.center();
        if (!std::isfinite(center.x()) || !std::isfinite(center.y())) {
            continue;
        }
        if (!fullBounds.contains(center)) {
            continue;
        }
        const int column = qBound(
            0, int(qBound(0.0,
                          (center.x() - fullBounds.left())
                              / fullBounds.width(),
                          1.0)
                       * gridSize),
            gridSize - 1);
        const int row = qBound(
            0, int(qBound(0.0,
                          (center.y() - fullBounds.top())
                              / fullBounds.height(),
                          1.0)
                       * gridSize),
            gridSize - 1);
        ++cellCounts[row * gridSize + column];
        ++validEntityCount;
    }
    if (validEntityCount < minimumEntityCount) {
        return fullBounds;
    }

    QVector<quint8> visited(cellCount, 0);
    qint64 bestCount = 0;
    int bestMinimumColumn = 0;
    int bestMaximumColumn = 0;
    int bestMinimumRow = 0;
    int bestMaximumRow = 0;
    static constexpr int neighborOffsets[8][2] = {
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
        {1, 0}, {-1, 1}, {0, 1}, {1, 1}
    };
    for (int start = 0; start < cellCount; ++start) {
        if (cellCounts[start] == 0 || visited[start]) {
            continue;
        }
        QVector<int> pending = {start};
        visited[start] = 1;
        qint64 componentCount = 0;
        int minimumColumn = gridSize;
        int maximumColumn = -1;
        int minimumRow = gridSize;
        int maximumRow = -1;
        for (qsizetype index = 0; index < pending.size(); ++index) {
            const int cell = pending[index];
            const int column = cell % gridSize;
            const int row = cell / gridSize;
            componentCount += cellCounts[cell];
            minimumColumn = qMin(minimumColumn, column);
            maximumColumn = qMax(maximumColumn, column);
            minimumRow = qMin(minimumRow, row);
            maximumRow = qMax(maximumRow, row);
            for (const auto &offset : neighborOffsets) {
                const int nextColumn = column + offset[0];
                const int nextRow = row + offset[1];
                if (nextColumn < 0 || nextColumn >= gridSize
                    || nextRow < 0 || nextRow >= gridSize) {
                    continue;
                }
                const int next = nextRow * gridSize + nextColumn;
                if (!visited[next] && cellCounts[next] > 0) {
                    visited[next] = 1;
                    pending.append(next);
                }
            }
        }
        if (componentCount > bestCount) {
            bestCount = componentCount;
            bestMinimumColumn = minimumColumn;
            bestMaximumColumn = maximumColumn;
            bestMinimumRow = minimumRow;
            bestMaximumRow = maximumRow;
        }
    }
    if (bestCount <= 0) {
        return fullBounds;
    }

    const double cellWidth = fullBounds.width() / gridSize;
    const double cellHeight = fullBounds.height() / gridSize;
    QRectF focusBounds(
        fullBounds.left() + bestMinimumColumn * cellWidth,
        fullBounds.top() + bestMinimumRow * cellHeight,
        (bestMaximumColumn - bestMinimumColumn + 1) * cellWidth,
        (bestMaximumRow - bestMinimumRow + 1) * cellHeight);
    const double horizontalMargin = focusBounds.width() * 0.06;
    const double verticalMargin = focusBounds.height() * 0.06;
    focusBounds.adjust(-horizontalMargin, -verticalMargin,
                       horizontalMargin, verticalMargin);
    return focusBounds.intersected(fullBounds);
}

namespace {

constexpr quint32 kParsedCacheMagic = 0x4341444cU;
constexpr quint32 kParsedCacheVersion = 8;

QDataStream &operator<<(QDataStream &stream,
                        const CadDimensionEdge &edge);
QDataStream &operator>>(QDataStream &stream, CadDimensionEdge &edge);
QDataStream &operator<<(QDataStream &stream,
                        const CadHatchPatternLine &line);
QDataStream &operator>>(QDataStream &stream, CadHatchPatternLine &line);
QDataStream &operator<<(QDataStream &stream, const CadLayer &layer);
QDataStream &operator>>(QDataStream &stream, CadLayer &layer);
QDataStream &operator<<(QDataStream &stream, const CadEntity &entity);
QDataStream &operator>>(QDataStream &stream, CadEntity &entity);
QDataStream &operator<<(QDataStream &stream,
                        const CadViewportDefinition &viewport);
QDataStream &operator>>(QDataStream &stream,
                        CadViewportDefinition &viewport);
QDataStream &operator<<(QDataStream &stream,
                        const CadExternalReference &reference);
QDataStream &operator>>(QDataStream &stream,
                        CadExternalReference &reference);

template <typename T>
void writeVector(QDataStream &stream, const QVector<T> &values)
{
    stream << quint32(values.size());
    for (const T &value : values) {
        stream << value;
    }
}

template <typename T>
bool readVector(QDataStream &stream, QVector<T> &values)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > 10000000U) {
        return false;
    }
    values.clear();
    values.reserve(qsizetype(count));
    for (quint32 i = 0; i < count; ++i) {
        T value;
        stream >> value;
        if (stream.status() != QDataStream::Ok) {
            return false;
        }
        values.append(std::move(value));
    }
    return true;
}

template <typename T>
void writeStringVectorMap(
    QDataStream &stream,
    const QHash<QString, QVector<T>> &values)
{
    stream << quint32(values.size());
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        stream << it.key();
        writeVector(stream, it.value());
    }
}

template <typename T>
bool readStringVectorMap(
    QDataStream &stream,
    QHash<QString, QVector<T>> &values)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > 100000U) {
        return false;
    }
    values.clear();
    for (quint32 i = 0; i < count; ++i) {
        QString key;
        QVector<T> value;
        stream >> key;
        if (!readVector(stream, value)) {
            return false;
        }
        values.insert(std::move(key), std::move(value));
    }
    return true;
}

template <typename T>
void writeStringMap(QDataStream &stream,
                    const QHash<QString, T> &values)
{
    stream << quint32(values.size());
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        stream << it.key() << it.value();
    }
}

template <typename T>
bool readStringMap(QDataStream &stream, QHash<QString, T> &values)
{
    quint32 count = 0;
    stream >> count;
    if (stream.status() != QDataStream::Ok || count > 100000U) {
        return false;
    }
    values.clear();
    for (quint32 i = 0; i < count; ++i) {
        QString key;
        T value;
        stream >> key >> value;
        if (stream.status() != QDataStream::Ok) {
            return false;
        }
        values.insert(std::move(key), std::move(value));
    }
    return true;
}

QString parsedCachePath(const QString &drawingPath)
{
    const QFileInfo info(drawingPath);
    const QString stablePath =
        info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    const QByteArray fingerprint =
        QFile::encodeName(stablePath)
        + '|' + QByteArray::number(info.size())
        + '|' + QByteArray::number(
            info.lastModified().toUTC().toSecsSinceEpoch())
        + '|' + QByteArray::number(qint64(kParsedCacheVersion));
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(fingerprint,
                                 QCryptographicHash::Sha256)
            .toHex());
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (directory.isEmpty()) {
        directory = QDir::tempPath() + QStringLiteral("/CadLens");
    }
    return directory + QStringLiteral("/parsed/") + hash
        + QStringLiteral(".cadcache");
}

QDataStream &operator<<(QDataStream &stream,
                        const CadDimensionEdge &edge)
{
    return stream << edge.midpoint << edge.length << edge.arc;
}

QDataStream &operator>>(QDataStream &stream, CadDimensionEdge &edge)
{
    return stream >> edge.midpoint >> edge.length >> edge.arc;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadHatchPatternLine &line)
{
    return stream << line.origin << line.direction << line.offset
                  << line.dashes;
}

QDataStream &operator>>(QDataStream &stream, CadHatchPatternLine &line)
{
    return stream >> line.origin >> line.direction >> line.offset
                  >> line.dashes;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadHatchPattern &pattern)
{
    stream << pattern.name << pattern.solid << pattern.gradient;
    writeVector(stream, pattern.lines);
    return stream;
}

QDataStream &operator>>(QDataStream &stream, CadHatchPattern &pattern)
{
    stream >> pattern.name >> pattern.solid >> pattern.gradient;
    readVector(stream, pattern.lines);
    return stream;
}

QDataStream &operator<<(QDataStream &stream, const CadLayer &layer)
{
    return stream << layer.name << layer.color << layer.sourceColor
                  << layer.visible
                  << layer.locked << layer.lineWeight
                  << layer.entityCount;
}

QDataStream &operator>>(QDataStream &stream, CadLayer &layer)
{
    return stream >> layer.name >> layer.color >> layer.sourceColor
                  >> layer.visible
                  >> layer.locked >> layer.lineWeight
                  >> layer.entityCount;
}

QDataStream &operator<<(QDataStream &stream, const CadEntity &entity)
{
    stream << qint32(entity.type) << quint64(entity.sourceHandle)
           << entity.layer << entity.color << entity.sourceColor
           << entity.lineWeight
           << entity.closed << entity.filled
           << entity.center << entity.radius << entity.startAngle
           << entity.endAngle << entity.text << entity.textPosition
           << entity.textAlignmentPoint << entity.textFit
           << entity.textHeight << entity.textRotation
           << entity.textWidthScale << entity.textHAlign
           << entity.textVAlign << entity.textAttachment
           << entity.textMirrored << entity.hatchGroup
           << bool(entity.hatchPattern);
    writeVector(stream, entity.points);
    if (entity.hatchPattern) {
        stream << *entity.hatchPattern;
    }
    writeVector(stream, entity.dimensionEdges);
    return stream << entity.clipRect;
}

QDataStream &operator>>(QDataStream &stream, CadEntity &entity)
{
    qint32 type = 0;
    quint64 handle = 0;
    bool hasPattern = false;
    stream >> type >> handle >> entity.layer >> entity.color
        >> entity.sourceColor >> entity.lineWeight >> entity.closed
        >> entity.filled >> entity.center >> entity.radius
        >> entity.startAngle >> entity.endAngle >> entity.text
        >> entity.textPosition >> entity.textAlignmentPoint
        >> entity.textFit >> entity.textHeight >> entity.textRotation
        >> entity.textWidthScale >> entity.textHAlign
        >> entity.textVAlign >> entity.textAttachment
        >> entity.textMirrored >> entity.hatchGroup >> hasPattern;
    readVector(stream, entity.points);
    entity.type = CadEntityType(qBound(0, int(type), 7));
    entity.sourceHandle = handle;
    if (hasPattern) {
        entity.hatchPattern = std::make_shared<CadHatchPattern>();
        stream >> *entity.hatchPattern;
    }
    readVector(stream, entity.dimensionEdges);
    stream >> entity.clipRect;
    entity.updateBounds();
    return stream;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadViewportDefinition &viewport)
{
    stream << viewport.center << viewport.windowSize
           << viewport.modelCenter << viewport.viewHeight
           << viewport.twist << viewport.revealsModel
           << qint32(viewport.frozenLayers.size());
    for (const QString &layer : viewport.frozenLayers) {
        stream << layer;
    }
    return stream;
}

QDataStream &operator>>(QDataStream &stream,
                        CadViewportDefinition &viewport)
{
    qint32 frozenCount = 0;
    stream >> viewport.center >> viewport.windowSize
        >> viewport.modelCenter >> viewport.viewHeight
        >> viewport.twist >> viewport.revealsModel >> frozenCount;
    frozenCount = qBound(0, int(frozenCount), 100000);
    for (int i = 0; i < frozenCount; ++i) {
        QString layer;
        stream >> layer;
        viewport.frozenLayers.insert(layer);
    }
    return stream;
}

QDataStream &operator<<(QDataStream &stream,
                        const CadExternalReference &reference)
{
    stream << reference.name << reference.savedPath
           << reference.resolvedPath << reference.fileFound
           << reference.loaded;
    return stream;
}

QDataStream &operator>>(QDataStream &stream,
                        CadExternalReference &reference)
{
    stream >> reference.name >> reference.savedPath
        >> reference.resolvedPath >> reference.fileFound
        >> reference.loaded;
    return stream;
}

void writeParsedDocumentCache(const CadDocument &document)
{
    const QString cachePath = parsedCachePath(document.path);
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_8);
    stream << kParsedCacheMagic << kParsedCacheVersion
           << document.path << document.fileName
           << document.formatVersion << document.sourceFormat
           << document.sourceBackend;
    writeVector(stream, document.entities);
    writeVector(stream, document.layers);
    writeVector(stream, document.layouts);
    writeStringVectorMap(stream, document.layoutEntities);
    writeStringVectorMap(stream, document.layoutViewports);
    writeStringMap(stream, document.layoutBounds);
    writeVector(stream, document.externalReferences);
    stream << document.computedBounds
           << document.declaredBounds << document.bounds
           << document.savedViewCenter << document.savedViewHeight
           << document.savedViewWidth << document.savedViewTwist
           << document.hasSavedView << qint32(document.attributeMode)
           << document.sourceAttributeCount
           << document.sourceVisibleAttributeCount
           << document.sourceHiddenAttributeCount
           << document.renderedAttributeVisits
           << document.suppressedAttributeVisits
           << document.overriddenHiddenAttributeVisits
           << qint32(document.sourceUnit)
           << document.metersPerDrawingUnit;
    if (stream.status() == QDataStream::Ok) {
        file.commit();
    }
}

bool readParsedDocumentCache(const QString &drawingPath,
                             CadDocument &document)
{
    QFile file(parsedCachePath(drawingPath));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_8);
    quint32 magic = 0;
    quint32 version = 0;
    stream >> magic >> version;
    if (magic != kParsedCacheMagic || version != kParsedCacheVersion) {
        return false;
    }
    CadDocument cached;
    qint32 attributeMode = -1;
    qint32 sourceUnit = int(MeasuringUnit::Millimeter);
    stream >> cached.path >> cached.fileName >> cached.formatVersion
        >> cached.sourceFormat >> cached.sourceBackend;
    if (!readVector(stream, cached.entities)
        || !readVector(stream, cached.layers)
        || !readVector(stream, cached.layouts)
        || !readStringVectorMap(stream, cached.layoutEntities)
        || !readStringVectorMap(stream, cached.layoutViewports)
        || !readStringMap(stream, cached.layoutBounds)
        || !readVector(stream, cached.externalReferences)) {
        return false;
    }
    stream >> cached.computedBounds
        >> cached.declaredBounds >> cached.bounds
        >> cached.savedViewCenter >> cached.savedViewHeight
        >> cached.savedViewWidth >> cached.savedViewTwist
        >> cached.hasSavedView >> attributeMode
        >> cached.sourceAttributeCount
        >> cached.sourceVisibleAttributeCount
        >> cached.sourceHiddenAttributeCount
        >> cached.renderedAttributeVisits
        >> cached.suppressedAttributeVisits
        >> cached.overriddenHiddenAttributeVisits
        >> sourceUnit >> cached.metersPerDrawingUnit;
    if (stream.status() != QDataStream::Ok
        || cached.entities.isEmpty() || cached.layers.isEmpty()
        || !cached.bounds.isValid()) {
        return false;
    }
    cached.attributeMode = attributeMode;
    cached.sourceUnit = MeasuringUnit(sourceUnit);
    cached.calibrated = false;
    cached.sanitizeRenderableEntities();
    document = std::move(cached);
    return true;
}

} // namespace

QString drawingParsedCachePath(const QString &drawingPath)
{
    return parsedCachePath(drawingPath);
}

namespace {

bool finiteCadPoint(const QPointF &point)
{
    constexpr double maximumCoordinate = 1.0e15;
    return std::isfinite(point.x()) && std::isfinite(point.y())
        && qAbs(point.x()) <= maximumCoordinate
        && qAbs(point.y()) <= maximumCoordinate;
}

void sanitizeCadEntity(CadEntity &entity)
{
    entity.points.erase(
        std::remove_if(
            entity.points.begin(), entity.points.end(),
            [](const QPointF &point) {
                return !finiteCadPoint(point);
            }),
        entity.points.end());
    if (!entity.clipRect.isNull() && !entity.clipRect.isValid()) {
        entity.clipRect = QRectF();
    }
    if (entity.type == CadEntityType::Text
        && entity.text.trimmed().isEmpty()) {
        entity.text.clear();
    }
    entity.updateBounds();
}

} // namespace

bool isRenderableCadEntity(const CadEntity &entity)
{
    if (entity.type == CadEntityType::Text) {
        return !entity.text.trimmed().isEmpty()
            && std::isfinite(entity.textHeight)
            && entity.textHeight > 1.0e-9
            && entity.textHeight < 1.0e9
            && finiteCadPoint(entity.textPosition);
    }
    if (entity.points.size() < 2) {
        return false;
    }
    if (!std::all_of(
            entity.points.cbegin(), entity.points.cend(),
            finiteCadPoint)) {
        return false;
    }
    if ((entity.type == CadEntityType::Circle
         || entity.type == CadEntityType::Arc)
        && (!std::isfinite(entity.radius)
            || entity.radius <= 1.0e-12
            || !finiteCadPoint(entity.center))) {
        return false;
    }
    if (!entity.bounds.isValid()
        || !std::isfinite(entity.bounds.width())
        || !std::isfinite(entity.bounds.height())
        || entity.bounds.width() > 1.0e15
        || entity.bounds.height() > 1.0e15) {
        return false;
    }
    if (entity.type == CadEntityType::Segment) {
        return QLineF(entity.points.first(), entity.points.last()).length()
            > 1.0e-12;
    }
    if (entity.type == CadEntityType::Solid) {
        return entity.points.size() >= 3;
    }
    return true;
}

void CadDocument::sanitizeRenderableEntities()
{
    const auto sanitizeVector = [](QVector<CadEntity> &values) {
        QVector<CadEntity> clean;
        clean.reserve(values.size());
        for (CadEntity &entity : values) {
            sanitizeCadEntity(entity);
            if (isRenderableCadEntity(entity)) {
                clean.append(std::move(entity));
            }
        }
        values = std::move(clean);
    };
    sanitizeVector(entities);
    for (auto iterator = layoutEntities.begin();
         iterator != layoutEntities.end(); ++iterator) {
        sanitizeVector(iterator.value());
    }
    recountLayers();
}

bool CadDocument::load(const QString &filePath, QString *errorMessage,
                       const QHash<QString, QString> &externalReferencePaths)
{
    QElapsedTimer timer;
    timer.start();
    entities.clear();
    layers.clear();
    layerIndices.clear();
    layouts.clear();
    layoutEntities.clear();
    layoutViewports.clear();
    layoutBounds.clear();
    pdfPages.clear();
    declaredBounds = QRectF();
    computedBounds = QRectF();
    bounds = QRectF();
    savedViewCenter = QPointF();
    savedViewHeight = 0.0;
    savedViewWidth = 0.0;
    savedViewTwist = 0.0;
    hasSavedView = false;
    attributeMode = -1;
    sourceAttributeCount = 0;
    sourceVisibleAttributeCount = 0;
    sourceHiddenAttributeCount = 0;
    renderedAttributeVisits = 0;
    suppressedAttributeVisits = 0;
    overriddenHiddenAttributeVisits = 0;
    calibrated = false;

    const QFileInfo info(filePath);
    if (!info.exists() || !info.isFile()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("文件不存在：%1").arg(filePath);
        }
        return false;
    }
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("pdf")) {
        if (!loadPdfDocument(filePath, *this, errorMessage)) {
            return false;
        }
        loadMilliseconds = timer.elapsed();
        return !pdfPages.isEmpty();
    }
    if (suffix != QStringLiteral("dwg") && suffix != QStringLiteral("dxf")) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("仅支持 DWG、DXF 与 PDF 图纸。");
        }
        return false;
    }

    if (suffix == QStringLiteral("dwg")
        && !qEnvironmentVariableIsSet("CADLENS_FORCE_LIBDXFRW")
        && !qEnvironmentVariableIsSet("CADLENS_DISABLE_PARSE_CACHE")
        && externalReferencePaths.isEmpty()
        && readParsedDocumentCache(filePath, *this)) {
        loadMilliseconds = timer.elapsed();
        if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_DWG")) {
            std::fprintf(stderr, "parsed-cache hit %lldms\n",
                         static_cast<long long>(loadMilliseconds));
        }
        return !entities.isEmpty();
    }

    if (suffix == QStringLiteral("dwg")
        && !qEnvironmentVariableIsSet("CADLENS_FORCE_LIBDXFRW")) {
        QString libreError;
        if (loadDwgWithLibreDwg(filePath, *this, &libreError,
                                externalReferencePaths)) {
            if (externalReferencePaths.isEmpty()
                && !qEnvironmentVariableIsSet(
                    "CADLENS_DISABLE_PARSE_CACHE")) {
                writeParsedDocumentCache(*this);
            }
            loadMilliseconds = timer.elapsed();
            return !entities.isEmpty();
        }
        // Keep the older direct reader as a compatibility fallback for DWG
        // revisions which the primary backend cannot complete.
        entities.clear();
        layers.clear();
        layouts.clear();
        layoutEntities.clear();
        layoutBounds.clear();
        declaredBounds = QRectF();
    }

    try {
        dx_data data;
        LayoutAwareInterface importer;
        const bool success = importer.fileImport(
            filePath.toStdString(), &data,
            qEnvironmentVariableIsSet("CADLENS_DXF_DEBUG"));
        if (!success) {
            if (errorMessage) {
                *errorMessage = QStringLiteral(
                    "图纸解析失败。文件可能损坏、加密，或使用了当前开源内核"
                    "尚未覆盖的 DWG 版本。");
            }
            return false;
        }
        entities.reserve(1024);
        importer.finishLayouts();
        ModelBuilder builder(*this);
        builder.read(data, importer, filePath, importer.layouts,
                     importer.layoutRecords);
    } catch (const std::exception &exception) {
        if (errorMessage) {
            *errorMessage = QString::fromUtf8(exception.what());
        }
        return false;
    } catch (...) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("解析图纸时发生未知错误。");
        }
        return false;
    }
    loadMilliseconds = timer.elapsed();
    return !entities.isEmpty();
}

const CadPdfPage *CadDocument::pdfPage(const QString &name) const
{
    for (const CadPdfPage &page : pdfPages) {
        if (page.name == name) {
            return &page;
        }
    }
    return nullptr;
}

CadPdfPage *CadDocument::pdfPage(const QString &name)
{
    for (CadPdfPage &page : pdfPages) {
        if (page.name == name) {
            return &page;
        }
    }
    return nullptr;
}

int CadDocument::layerIndex(const QString &name) const
{
    const auto found = layerIndices.constFind(name);
    if (found != layerIndices.cend()) {
        return found.value();
    }
    for (int i = 0; i < layers.size(); ++i) {
        if (layers[i].name == name) {
            return i;
        }
    }
    return -1;
}

const CadLayer *CadDocument::layer(const QString &name) const
{
    const int index = layerIndex(name);
    return index >= 0 ? &layers[index] : nullptr;
}

CadLayer *CadDocument::layer(const QString &name)
{
    const int index = layerIndex(name);
    return index >= 0 ? &layers[index] : nullptr;
}

void CadDocument::recountLayers()
{
    layerIndices.clear();
    layerIndices.reserve(layers.size());
    for (int i = 0; i < layers.size(); ++i) {
        layers[i].entityCount = 0;
        layerIndices.insert(layers[i].name, i);
    }
    for (const CadEntity &entity : entities) {
        if (CadLayer *item = layer(entity.layer)) {
            ++item->entityCount;
        }
    }
}

QString measuringUnitLabel(MeasuringUnit unit)
{
    switch (unit) {
    case MeasuringUnit::Millimeter:
        return QStringLiteral("毫米 (mm)");
    case MeasuringUnit::Centimeter:
        return QStringLiteral("厘米 (cm)");
    case MeasuringUnit::Meter:
        return QStringLiteral("米 (m)");
    case MeasuringUnit::Kilometer:
        return QStringLiteral("千米 (km)");
    case MeasuringUnit::Inch:
        return QStringLiteral("英寸 (in)");
    case MeasuringUnit::Foot:
        return QStringLiteral("英尺 (ft)");
    case MeasuringUnit::None:
        break;
    }
    return QStringLiteral("图纸单位");
}

QString measuringUnitSuffix(MeasuringUnit unit, bool squared)
{
    QString suffix;
    switch (unit) {
    case MeasuringUnit::Millimeter:
        suffix = QStringLiteral("mm");
        break;
    case MeasuringUnit::Centimeter:
        suffix = QStringLiteral("cm");
        break;
    case MeasuringUnit::Meter:
        suffix = QStringLiteral("m");
        break;
    case MeasuringUnit::Kilometer:
        suffix = QStringLiteral("km");
        break;
    case MeasuringUnit::Inch:
        suffix = QStringLiteral("in");
        break;
    case MeasuringUnit::Foot:
        suffix = QStringLiteral("ft");
        break;
    case MeasuringUnit::None:
        suffix = QStringLiteral("u");
        break;
    }
    return squared ? suffix + QStringLiteral("²") : suffix;
}

double measuringUnitMeters(MeasuringUnit unit)
{
    switch (unit) {
    case MeasuringUnit::Millimeter:
        return 0.001;
    case MeasuringUnit::Centimeter:
        return 0.01;
    case MeasuringUnit::Meter:
        return 1.0;
    case MeasuringUnit::Kilometer:
        return 1000.0;
    case MeasuringUnit::Inch:
        return 0.0254;
    case MeasuringUnit::Foot:
        return 0.3048;
    case MeasuringUnit::None:
        return 1.0;
    }
    return 1.0;
}

MeasuringUnit measuringUnitFromInsUnits(int insUnits)
{
    switch (insUnits) {
    case 1:
        return MeasuringUnit::Inch;
    case 2:
        return MeasuringUnit::Foot;
    case 4:
        return MeasuringUnit::Millimeter;
    case 5:
        return MeasuringUnit::Centimeter;
    case 6:
        return MeasuringUnit::Meter;
    case 7:
        return MeasuringUnit::Kilometer;
    default:
        return MeasuringUnit::Millimeter;
    }
}

QString formatMeasuredLength(double drawingLength, const CadDocument &document,
                             MeasuringUnit displayUnit, int precision)
{
    const double value = drawingLength * document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QStringLiteral("%1 %2")
        .arg(value, 0, 'f', precision)
        .arg(measuringUnitSuffix(displayUnit));
}

QString formatDimensionValue(double drawingLength,
                             const CadDocument &document,
                             MeasuringUnit displayUnit, int precision)
{
    const double value = drawingLength * document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QString::number(value, 'f', qBound(0, precision, 6));
}

QString formatMeasuredArea(double drawingArea, const CadDocument &document,
                           MeasuringUnit displayUnit, int precision)
{
    const double linearFactor = document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QStringLiteral("%1 %2")
        .arg(drawingArea * linearFactor * linearFactor, 0, 'f', precision)
        .arg(measuringUnitSuffix(displayUnit, true));
}

QString formatAreaValue(double drawingArea, const CadDocument &document,
                        MeasuringUnit displayUnit, int precision)
{
    const double linearFactor = document.metersPerDrawingUnit
        / measuringUnitMeters(displayUnit);
    return QString::number(drawingArea * linearFactor * linearFactor,
                           'f', qBound(0, precision, 6));
}

void appendConnectedPolylinePiece(
    QVector<QVector<QPointF>> &paths, QVector<QPointF> piece,
    double tolerance)
{
    if (piece.isEmpty()) {
        return;
    }
    const auto pointsMatch = [tolerance](const QPointF &a,
                                         const QPointF &b) {
        return QLineF(a, b).length() <= tolerance;
    };
    if (paths.isEmpty()) {
        paths.append(std::move(piece));
        return;
    }

    QVector<QPointF> &path = paths.last();
    if (!path.isEmpty() && pointsMatch(path.last(), piece.first())) {
        for (qsizetype i = 1; i < piece.size(); ++i) {
            path.append(piece[i]);
        }
        return;
    }
    if (!path.isEmpty() && pointsMatch(path.last(), piece.last())) {
        for (qsizetype i = piece.size() - 2; i >= 0; --i) {
            path.append(piece[i]);
        }
        return;
    }
    paths.append(std::move(piece));
}
