#include "CadView.h"

#include "PdfDocument.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QPainterPath>
#include <QPrinter>
#include <QPolygonF>
#include <QSurfaceFormat>
#include <QSvgGenerator>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinScreenSegment = 1.0e-6;
constexpr double kMinHatchSpacing = 0.1;
constexpr double kMinTextPixelHeight = 1.0;
constexpr double kDefaultDimensionTextPixels = 25.0;
constexpr double kMinimumReadableDimensionTextPixels = 25.0;

bool isDimensionMeasurement(MeasurementType type)
{
    switch (type) {
    case MeasurementType::Aligned:
    case MeasurementType::Linear:
    case MeasurementType::Continuous:
    case MeasurementType::Batch:
    case MeasurementType::Radius:
    case MeasurementType::Angle:
    case MeasurementType::ArcLength:
        return true;
    default:
        return false;
    }
}

// 标注线宽：随图纸缩放一起变粗，但保底为基准宽度，避免缩小后看不见。
inline double annotationStroke(double base, double geometryScale)
{
    return qMax(base, base * geometryScale);
}

struct LineBatchKey {
    QRgb color = 0;
    int width = 0;
    qint64 left = 0;
    qint64 top = 0;
    qint64 right = 0;
    qint64 bottom = 0;

    bool operator==(const LineBatchKey &other) const = default;
};

struct ClipKey {
    qint64 left = 0;
    qint64 top = 0;
    qint64 right = 0;
    qint64 bottom = 0;

    bool operator==(const ClipKey &other) const = default;
};

qint64 doubleBits(double value)
{
    qint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::size_t qHash(const LineBatchKey &key, std::size_t seed = 0) noexcept
{
    seed ^= std::size_t(key.color) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.width) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.left) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.top) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.right) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.bottom) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    return seed;
}

std::size_t qHash(const ClipKey &key, std::size_t seed = 0) noexcept
{
    seed ^= std::size_t(key.left) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.top) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.right) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    seed ^= std::size_t(key.bottom) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    return seed;
}

double lineWidthPixels(int cadWeight, bool showWeights)
{
    if (!showWeights || cadWeight < 0 || cadWeight > 23) {
        return 1.25;
    }
    static const double millimeters[] = {
        0.0, 0.05, 0.09, 0.13, 0.15, 0.18, 0.20, 0.25,
        0.30, 0.35, 0.40, 0.50, 0.53, 0.60, 0.70, 0.80,
        0.90, 1.00, 1.06, 1.20, 1.40, 1.58, 2.00, 2.11
    };
    return 0.55 + millimeters[cadWeight] * 1.35;
}

 QPainterPath polygonPath(const QPainter &painter,
                         const QVector<QPointF> &screenPoints, bool close)
{
    QPainterPath path;
    Q_UNUSED(painter);
    if (screenPoints.isEmpty()) {
        return path;
    }
    path.moveTo(screenPoints.first());
    for (int i = 1; i < screenPoints.size(); ++i) {
        path.lineTo(screenPoints[i]);
    }
    if (close) {
        path.closeSubpath();
    }
    return path;
}

double distanceSquaredToScreenSegment(const QPointF &point,
                                      const QPointF &first,
                                      const QPointF &second)
{
    const QPointF delta = second - first;
    const double lengthSquared = QPointF::dotProduct(delta, delta);
    if (lengthSquared <= 1.0e-18) {
        const QPointF offset = point - first;
        return QPointF::dotProduct(offset, offset);
    }
    const double projection = qBound(
        0.0,
        QPointF::dotProduct(point - first, delta) / lengthSquared,
        1.0);
    const QPointF offset = point - (first + delta * projection);
    return QPointF::dotProduct(offset, offset);
}

QVector<QPointF> simplifyScreenPolyline(
    const QVector<QPointF> &points,
    double tolerance)
{
    if (points.size() <= 2 || tolerance <= 1.0e-9) {
        return points;
    }
    const double squaredTolerance = tolerance * tolerance;
    QVector<bool> keep(points.size(), false);
    keep[0] = true;
    keep[points.size() - 1] = true;
    std::vector<std::pair<int, int>> ranges;
    ranges.emplace_back(0, points.size() - 1);
    while (!ranges.empty()) {
        const auto [firstIndex, lastIndex] = ranges.back();
        ranges.pop_back();
        if (lastIndex <= firstIndex + 1) {
            continue;
        }
        int farthestIndex = firstIndex;
        double farthestDistance = squaredTolerance;
        for (int index = firstIndex + 1; index < lastIndex; ++index) {
            const double distance = distanceSquaredToScreenSegment(
                points[index], points[firstIndex], points[lastIndex]);
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthestIndex = index;
            }
        }
        if (farthestIndex != firstIndex) {
            keep[farthestIndex] = true;
            ranges.emplace_back(firstIndex, farthestIndex);
            ranges.emplace_back(farthestIndex, lastIndex);
        }
    }
    QVector<QPointF> result;
    result.reserve(points.size());
    for (int index = 0; index < points.size(); ++index) {
        if (keep[index]) {
            result.append(points[index]);
        }
    }
    return result;
}

void appendScreenPolylineSegments(
    QVector<QLineF> &segments,
    const QVector<QPointF> &points,
    bool closed,
    double minimumEdge)
{
    if (points.size() < 2) {
        return;
    }
    QPointF previous = points.first();
    for (qsizetype index = 1; index < points.size(); ++index) {
        const QPointF current = points[index];
        if (QLineF(previous, current).length() >= minimumEdge) {
            segments.append(QLineF(previous, current));
        }
        previous = current;
    }
    if (closed) {
        const QPointF first = points.first();
        if (QLineF(previous, first).length() >= minimumEdge) {
            segments.append(QLineF(previous, first));
        }
    }
}

void appendHatchPatternLine(QVector<QLineF> &result,
                            const QPointF &origin,
                            const QPointF &direction,
                            const QPointF &offset,
                            const QRectF &bounds,
                            const QVector<double> &dashes,
                            double minimumScreenSpacing,
                            double minimumSegmentLength,
                            qsizetype maximumSegments)
{
    if (result.size() >= maximumSegments) {
        return;
    }
    const double directionLength =
        std::hypot(direction.x(), direction.y());
    if (directionLength < 1.0e-9) {
        return;
    }
    const QPointF unit = direction / directionLength;
    const QPointF normal(-unit.y(), unit.x());
    const double offsetNormal =
        offset.x() * normal.x() + offset.y() * normal.y();
    if (qAbs(offsetNormal) < kMinHatchSpacing
        || qAbs(offsetNormal) < minimumScreenSpacing) {
        return;
    }

    const QPointF corners[] = {
        bounds.topLeft(), bounds.topRight(),
        bounds.bottomRight(), bounds.bottomLeft()
    };
    double minimumNormal = std::numeric_limits<double>::max();
    double maximumNormal = std::numeric_limits<double>::lowest();
    double minimumAlong = std::numeric_limits<double>::max();
    double maximumAlong = std::numeric_limits<double>::lowest();
    const double originNormal =
        origin.x() * normal.x() + origin.y() * normal.y();
    for (const QPointF &corner : corners) {
        const double normalPosition =
            corner.x() * normal.x() + corner.y() * normal.y();
        const double alongPosition =
            corner.x() * unit.x() + corner.y() * unit.y();
        minimumNormal = qMin(minimumNormal, normalPosition);
        maximumNormal = qMax(maximumNormal, normalPosition);
        minimumAlong = qMin(minimumAlong, alongPosition);
        maximumAlong = qMax(maximumAlong, alongPosition);
    }

    double firstLineValue =
        (minimumNormal - originNormal) / offsetNormal;
    double lastLineValue =
        (maximumNormal - originNormal) / offsetNormal;
    if (firstLineValue > lastLineValue) {
        std::swap(firstLineValue, lastLineValue);
    }
    const int firstLine = int(std::floor(firstLineValue)) - 1;
    const int lastLine = int(std::ceil(lastLineValue)) + 1;
    if (lastLine < firstLine || lastLine - firstLine > 4096) {
        return;
    }
    double period = 0.0;
    for (double dash : dashes) {
        period += qAbs(dash) * directionLength;
    }
    for (int lineIndex = firstLine; lineIndex <= lastLine; ++lineIndex) {
        const QPointF base = origin + offset * double(lineIndex);
        const double baseAlong =
            base.x() * unit.x() + base.y() * unit.y();
        const double start =
            minimumAlong - baseAlong - 1.0;
        const double end =
            maximumAlong - baseAlong + 1.0;
        const auto pointAt = [&](double distance) {
            return base + unit * distance;
        };
        if (dashes.isEmpty() || period <= 1.0e-9) {
            const QLineF line(pointAt(start), pointAt(end));
            if (line.length() >= minimumSegmentLength) {
                if (result.size() >= maximumSegments) {
                    return;
                }
                result.append(line);
            }
            continue;
        }
        const int cycles =
            int(std::ceil((end - start) / period)) + 1;
        if (cycles > 4096) {
            continue;
        }
        double cycleStart =
            std::floor(start / period) * period;
        for (int cycle = 0; cycle < cycles; ++cycle) {
            double cursor = cycleStart;
            for (double dash : dashes) {
                const double length = qAbs(dash) * directionLength;
                if (qAbs(dash) < 1.0e-9) {
                    if (cursor >= start && cursor <= end) {
                        if (result.size() >= maximumSegments) {
                            return;
                        }
                        // A zero-length pattern element is a dot. Preserve it
                        // as a degenerate world-space line; the renderer turns
                        // it into a round screen dot. Expanding it to an
                        // epsilon-length world segment made it disappear once
                        // the segment was transformed to screen coordinates.
                        result.append(
                            QLineF(pointAt(cursor), pointAt(cursor)));
                    }
                } else if (dash > 0.0) {
                    const double dashStart =
                        qMax(start, cursor);
                    const double dashEnd =
                        qMin(end, cursor + length);
                    if (dashEnd > dashStart) {
                        const QLineF line(pointAt(dashStart),
                                          pointAt(dashEnd));
                        if (line.length() >= minimumSegmentLength) {
                            if (result.size() >= maximumSegments) {
                                return;
                            }
                            result.append(line);
                        }
                    }
                }
                cursor += length;
            }
            cycleStart += period;
        }
    }
}

} // namespace

const QVector<QLineF> &CadView::hatchPatternLinesForGroup(
    int groupId, const QVector<int> &group,
    qsizetype generationLimit, quint64 cacheSignature)
{
    QHash<int, QVector<QLineF>> &spaceCache =
        m_hatchLineCache[m_activeSpace];
    QHash<int, quint64> &signatureCache =
        m_hatchLineCacheSignatures[m_activeSpace];
    const auto cached = spaceCache.constFind(groupId);
    const auto cachedSignature =
        signatureCache.constFind(groupId);
    if (cached != spaceCache.cend()
        && cachedSignature != signatureCache.cend()
        && cachedSignature.value() == cacheSignature) {
        return cached.value();
    }
    signatureCache.insert(groupId, cacheSignature);

    QVector<QLineF> generated;
    if (!m_document || group.isEmpty()) {
        return spaceCache.insert(groupId, std::move(generated)).value();
    }

    QRectF bounds;
    std::shared_ptr<CadHatchPattern> pattern;
    for (int index : group) {
        if (index < 0 || index >= m_document->entities.size()) {
            continue;
        }
        const CadEntity &entity = m_document->entities[index];
        if (entity.bounds.isValid()) {
            bounds = bounds.isValid() ? bounds.united(entity.bounds)
                                      : entity.bounds;
        }
        if (!pattern && entity.hatchPattern) {
            pattern = entity.hatchPattern;
        }
    }
    if (!bounds.isValid() || !pattern || pattern->solid
        || pattern->lines.isEmpty()) {
        return spaceCache.insert(groupId, std::move(generated)).value();
    }

    // CAD HATCH patterns are defined in world coordinates. Generate them
    // once and reuse the same segments for static, pan, zoom and cached
    // interaction frames. Regenerating from screen-space in only some
    // modes causes density/colour jumps during gestures.
    const qsizetype maximumSegments = qMin<qsizetype>(
        qMax<qsizetype>(1, generationLimit),
        qBound<qsizetype>(
            1,
            qsizetype(pattern->maximumPatternSegments),
            qsizetype(10000000)));
    const double tolerance = qMax(
        1.0e-12,
        qMax(bounds.width() + bounds.height(), 1.0)
            * pattern->toleranceFactor / 3.0);
    const qsizetype families =
        qMax<qsizetype>(1, pattern->lines.size());
    const qsizetype perFamilyCap = qMax<qsizetype>(
        16, maximumSegments / families);
    generated.reserve(qMin<qsizetype>(
        maximumSegments, qMax<qsizetype>(64, families * 16)));
    const QRectF patternBounds =
        bounds.adjusted(-tolerance, -tolerance,
                        tolerance, tolerance);
    for (const CadHatchPatternLine &definition : pattern->lines) {
        if (generated.size() >= maximumSegments) {
            break;
        }
        const qsizetype familyCap = qMin<qsizetype>(
            maximumSegments,
            generated.size() + perFamilyCap);
        appendHatchPatternLine(
            generated, definition.origin, definition.direction,
            definition.offset, patternBounds, definition.dashes,
            tolerance, tolerance, familyCap);
    }
    return spaceCache.insert(groupId, std::move(generated)).value();
}

const QPainterPath &CadView::hatchPathForGroup(
    int groupId, const QVector<int> &group)
{
    QHash<int, QPainterPath> &spaceCache =
        m_hatchPathCache[m_activeSpace];
    const auto cached = spaceCache.constFind(groupId);
    if (cached != spaceCache.cend()) {
        return cached.value();
    }
    QPainterPath path;
    if (m_document) {
        for (int index : group) {
            if (index < 0 || index >= m_document->entities.size()) {
                continue;
            }
            const CadEntity &entity = m_document->entities[index];
            if (entity.points.size() >= 3) {
                path.addPolygon(QPolygonF(entity.points));
            }
        }
    }
    return spaceCache.insert(groupId, std::move(path)).value();
}

namespace {

QColor annotationColor(const Measurement &measurement)
{
    return measurement.color.isValid() ? measurement.color
                                       : QColor(QStringLiteral("#37d67a"));
}

double distanceToSegment(const QLineF &line, const QPointF &point)
{
    const QPointF delta = line.p2() - line.p1();
    const double lengthSquared = QPointF::dotProduct(delta, delta);
    if (lengthSquared < 1.0e-12) {
        return QLineF(line.p1(), point).length();
    }
    const double t = qBound(
        0.0, QPointF::dotProduct(point - line.p1(), delta) / lengthSquared,
        1.0);
    return QLineF(line.p1() + delta * t, point).length();
}

std::optional<QPointF> circleCenterFromThreePoints(
    const QPointF &a, const QPointF &b, const QPointF &c)
{
    const double determinant =
        2.0 * (a.x() * (b.y() - c.y())
               + b.x() * (c.y() - a.y())
               + c.x() * (a.y() - b.y()));
    const double scale = std::max(
        {QLineF(a, b).length(), QLineF(b, c).length(),
         QLineF(c, a).length(), 1.0});
    if (qAbs(determinant) <= 1.0e-10 * scale * scale) {
        return std::nullopt;
    }
    const double aSquared = a.x() * a.x() + a.y() * a.y();
    const double bSquared = b.x() * b.x() + b.y() * b.y();
    const double cSquared = c.x() * c.x() + c.y() * c.y();
    return QPointF(
        (aSquared * (b.y() - c.y())
         + bSquared * (c.y() - a.y())
         + cSquared * (a.y() - b.y()))
            / determinant,
        (aSquared * (c.x() - b.x())
         + bSquared * (a.x() - c.x())
         + cSquared * (b.x() - a.x()))
            / determinant);
}

QPainterPath arcPathThroughThreePoints(const QPointF &start,
                                       const QPointF &through,
                                       const QPointF &end)
{
    const std::optional<QPointF> center =
        circleCenterFromThreePoints(start, through, end);
    if (!center) {
        return {};
    }
    const double radius = QLineF(*center, start).length();
    if (radius <= 1.0e-9) {
        return {};
    }
    const auto angleOf = [&center](const QPointF &point) {
        return std::atan2(point.y() - center->y(),
                          point.x() - center->x());
    };
    const double startAngle = angleOf(start);
    const double throughAngle = angleOf(through);
    const double endAngle = angleOf(end);
    double sweep = endAngle - startAngle;
    while (sweep <= 0.0) {
        sweep += 2.0 * kPi;
    }
    double throughSweep = throughAngle - startAngle;
    while (throughSweep < 0.0) {
        throughSweep += 2.0 * kPi;
    }
    if (throughSweep > sweep) {
        sweep -= 2.0 * kPi;
    }
    const QRectF circleBox(
        center->x() - radius, center->y() - radius,
        radius * 2.0, radius * 2.0);
    const double startDegrees = -startAngle * 180.0 / kPi;
    const double sweepDegrees = -sweep * 180.0 / kPi;
    QPainterPath path;
    path.arcMoveTo(circleBox, startDegrees);
    path.arcTo(circleBox, startDegrees, sweepDegrees);
    return path;
}

const QStringList &cadFontFamilies()
{
    static const QStringList families = [] {
        QStringList result;
        const int technicalId =
            QFontDatabase::addApplicationFont(
                QStringLiteral(":/fonts/osifont-lgpl3fe.ttf"));
        const int cjkId =
            QFontDatabase::addApplicationFont(
                QStringLiteral(":/fonts/NotoSansCJKsc-Regular.otf"));
        result.append(QFontDatabase::applicationFontFamilies(technicalId));
        result.append(QFontDatabase::applicationFontFamilies(cjkId));
        if (result.isEmpty()) {
            result.append(QStringLiteral("Arial"));
        }
        return result;
    }();
    return families;
}

QFont cadFont(int pixelSize, QFont::Weight weight = QFont::Normal)
{
    QFont font;
    font.setFamilies(cadFontFamilies());
    font.setPixelSize(qMax(1, pixelSize));
    font.setWeight(weight);
    return font;
}

QFont cadFontForText(const QString &text, int pixelSize,
                     QFont::Weight weight = QFont::Normal)
{
    const int target = qMax(1, pixelSize);
    QFont font = cadFont(target, weight);
    const QFontMetricsF metrics(font);
    double visibleHeight = 0.0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString sample = line.isEmpty() ? QStringLiteral("Ag") : line;
        visibleHeight = qMax(
            visibleHeight, metrics.boundingRect(sample).height());
    }
    if (visibleHeight > 0.5) {
        const double normalized =
            target * double(target) / visibleHeight;
        font.setPixelSize(qBound(
            1, qRound(normalized), target * 3));
    }
    return font;
}

} // namespace

CadView::CadView(QWidget *parent) : QOpenGLWidget(parent)
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSwapInterval(1);
    setFormat(format);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::BlankCursor);
    m_framePump.setInterval(16);
    m_framePump.setTimerType(Qt::PreciseTimer);
    connect(&m_framePump, &QTimer::timeout, this, [this] {
        if (m_frameDirty) {
            m_frameDirty = false;
            update();
        } else {
            m_framePump.stop();
        }
    });
}

void CadView::requestRenderFrame()
{
    m_frameDirty = true;
    // React to sustained load, not a single expensive frame. A one-off
    // cache rebuild used to pin the pump at 20fps for the next second even
    // though all following frames were sub-millisecond bitmap blits.
    const double cost = m_frameCostEmaMs;
    const int targetInterval =
        cost >= 40.0 ? 50 : cost >= 24.0 ? 33 : 16;
    if (m_framePump.interval() != targetInterval) {
        m_framePump.setInterval(targetInterval);
    }
    if (!m_framePump.isActive()) {
        m_framePump.start();
    }
}

QColor CadView::canvasBackgroundColor() const
{
    switch (m_drawingTheme) {
    case DrawingTheme::CadLight:
        return QColor(QStringLiteral("#f7f7f4"));
    case DrawingTheme::CadGray:
        return QColor(QStringLiteral("#2b3034"));
    case DrawingTheme::CadBlueprint:
        return QColor(QStringLiteral("#102f4a"));
    case DrawingTheme::CadDark:
        break;
    }
    return QColor(QStringLiteral("#000000"));
}

QColor CadView::printColor(const QColor &source) const
{
    if (m_printBlackInk) {
        return QColor(Qt::black);
    }
    return source.isValid() ? source : QColor(Qt::black);
}

QColor CadView::printColor(const CadEntity &entity) const
{
    if (m_printBlackInk) {
        return QColor(Qt::black);
    }
    if (entity.sourceColor.isValid()) {
        return entity.sourceColor;
    }
    return entity.color.isValid() ? entity.color : QColor(Qt::black);
}

QColor CadView::displayColor(const QColor &source) const
{
    if (!source.isValid()) {
        return m_drawingTheme == DrawingTheme::CadLight
            ? QColor(QStringLiteral("#101820"))
            : QColor(QStringLiteral("#f2f4f5"));
    }
    if (m_drawingTheme != DrawingTheme::CadLight) {
        return displayColorForDarkCanvas(source);
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
    if (luminance(source) <= 0.48) {
        return source;
    }
    QColor adjusted = source;
    for (double amount = 0.08; amount <= 1.0; amount += 0.08) {
        adjusted.setRed(qRound(source.red() * (1.0 - amount)));
        adjusted.setGreen(qRound(source.green() * (1.0 - amount)));
        adjusted.setBlue(qRound(source.blue() * (1.0 - amount)));
        if (luminance(adjusted) <= 0.4) {
            adjusted.setAlpha(source.alpha());
            return adjusted;
        }
    }
    adjusted = QColor(QStringLiteral("#101820"));
    adjusted.setAlpha(source.alpha());
    return adjusted;
}

void CadView::setDrawingTheme(DrawingTheme theme)
{
    if (m_drawingTheme == theme) {
        return;
    }
    m_drawingTheme = theme;
    if (isValid()) {
        makeCurrent();
        const QColor background = canvasBackgroundColor();
        glClearColor(background.redF(), background.greenF(),
                     background.blueF(), background.alphaF());
        doneCurrent();
    }
    m_cadTextPathCache.clear();
    invalidateDrawingCache();
}

void CadView::setDocument(
    std::shared_ptr<CadDocument> document,
    std::shared_ptr<SpatialIndex> preparedIndex,
    QHash<QString, QVector<CadEntity>> preparedLayouts)
{
    invalidateSceneCache();
    m_document = std::move(document);
    m_cadTextPathCache.clear();
    m_hatchLineCache.clear();
    m_hatchLineCacheSignatures.clear();
    m_hatchPathCache.clear();
    m_dimensionTextHeights.clear();
    m_measurements.clear();
    m_layoutCache = std::move(preparedLayouts);
    m_spaceViews.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_selectedMeasurementId.clear();
    clearDraft();
    if (m_document) {
        // Older documents may carry only VPLAYER frozen layer names.
        // Rebuild the index set so they still get precise matching. New
        // caches persist the indices alongside names.
        for (auto &viewports : m_document->layoutViewports) {
            for (CadViewportDefinition &viewport : viewports) {
                if (!viewport.frozenLayerIndices.isEmpty()
                    || viewport.frozenLayers.isEmpty()) {
                    continue;
                }
                QSet<int> resolved;
                bool complete = true;
                for (const QString &layerName :
                     std::as_const(viewport.frozenLayers)) {
                    const int index = m_document->layerIndex(layerName);
                    if (index < 0) {
                        complete = false;
                        break;
                    }
                    resolved.insert(index);
                }
                if (complete) {
                    viewport.frozenLayerIndices = std::move(resolved);
                }
            }
        }
        m_activeSpace = m_document->isPdf()
            ? m_document->pdfPages.first().name
            : QStringLiteral("模型");
        m_modelEntities = m_document->entities;
        m_modelBounds = m_document->isPdf()
            ? m_document->pdfPages.first().pageRect
            : m_document->bounds;
        m_settings.lengthUnit = m_document->sourceUnit == MeasuringUnit::None
            ? MeasuringUnit::Millimeter
            : m_document->sourceUnit;
        if (preparedIndex) {
            m_spatialIndex = std::move(*preparedIndex);
        } else {
            m_spatialIndex.rebuild(*m_document);
        }
        m_snapEngine.setDocument(m_document.get(), &m_spatialIndex);
        const QRectF initialBounds = m_document->isPdf()
            ? m_document->bounds
            : cadInitialViewBounds(m_document->entities,
                                   m_document->bounds);
        m_center = initialBounds.center();
        m_scale = 1.0;
        m_initialFitPending = true;
        QTimer::singleShot(20, this, [this] {
            if (m_initialFitPending && m_document) {
                fitToInitialView();
            }
        });
    } else {
        m_document.reset();
        m_modelEntities = QVector<CadEntity>();
        m_modelBounds = QRectF();
        m_layoutCache = QHash<QString, QVector<CadEntity>>();
        m_spaceViews = QHash<QString, SavedSpaceView>();
        m_cadTextPathCache = QHash<QString, QPainterPath>();
        m_hatchLineCache.clear();
        m_hatchLineCacheSignatures.clear();
        m_hatchPathCache.clear();
        m_dimensionTextHeights = QHash<QString, double>();
        m_measurements = QVector<Measurement>();
        m_labelHitRects = QHash<QString, QRectF>();
        m_undoStack = QVector<QVector<Measurement>>();
        m_redoStack = QVector<QVector<Measurement>>();
        m_framePump.stop();
        m_frameDirty = false;
        m_spatialIndex = SpatialIndex();
        m_snapEngine.setDocument(nullptr, nullptr);
    }
    emit measurementsChanged();
    emit viewTransformChanged();
    update();
}

void CadView::setActiveSpace(const QString &spaceName)
{
    if (!m_document) {
        return;
    }
    const QString target = spaceName.isEmpty() ? QStringLiteral("模型")
                                               : spaceName;
    if (target == m_activeSpace) {
        return;
    }
    if (m_document->isPdf()) {
        CadPdfPage *targetPage = m_document->pdfPage(target);
        if (!targetPage) {
            return;
        }
        if (m_scale > 0.0) {
            m_spaceViews.insert(
                m_activeSpace, {m_center, m_scale, m_fitScale});
        }
        if (targetPage->image.isNull()) {
            int pageIndex = -1;
            for (int i = 0; i < m_document->pdfPages.size(); ++i) {
                if (m_document->pdfPages[i].name == target) {
                    pageIndex = i;
                    break;
                }
            }
            QString error;
            if (pageIndex < 0
                || !renderPdfPage(m_document->path, pageIndex,
                               3200, &targetPage->image, &error)) {
                emit statusMessage(error);
                return;
            }
        }
        m_activeSpace = target;
        m_document->entities.clear();
        m_document->bounds = targetPage->pageRect;
        invalidateSceneCache();
        m_spatialIndex = SpatialIndex();
        m_snapEngine.setDocument(nullptr, nullptr);
        m_selectedMeasurementId.clear();
        clearDraft();
        const auto saved = m_spaceViews.constFind(target);
        if (saved != m_spaceViews.cend() && saved->scale > 0.0) {
            m_center = saved->center;
            m_scale = saved->scale;
            m_fitScale = saved->fitScale;
            invalidateSceneCache();
            emit viewTransformChanged();
            emit zoomChanged(
                m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
            update();
        } else {
            fitToDrawing();
        }
        emit activeSpaceChanged(target);
        return;
    }
    if (m_scale > 0.0) {
        m_spaceViews.insert(
            m_activeSpace, {m_center, m_scale, m_fitScale});
    }
    if (m_activeSpace == QStringLiteral("模型")) {
        m_modelEntities = m_document->entities;
        m_modelBounds = m_document->bounds;
    }
    if (target == QStringLiteral("模型")) {
        m_document->entities = m_modelEntities;
        m_document->bounds = m_modelBounds;
    } else {
        if (!m_document->layoutEntities.contains(target)) {
            return;
        }
        const auto cached = m_layoutCache.constFind(target);
        m_document->entities = cached == m_layoutCache.cend()
            ? buildLayoutContent(target)
            : cached.value();
        if (cached == m_layoutCache.cend()) {
            m_layoutCache.insert(target, m_document->entities);
        }
        m_document->bounds =
            m_document->layoutBounds.value(target,
                                           m_document->bounds);
    }
    m_activeSpace = target;
    invalidateSceneCache();
    m_spatialIndex.rebuild(*m_document);
    m_snapEngine.setDocument(m_document.get(), &m_spatialIndex);
    m_selectedMeasurementId.clear();
    clearDraft();
    const auto saved = m_spaceViews.constFind(target);
    if (saved != m_spaceViews.cend() && saved->scale > 0.0) {
        m_center = saved->center;
        m_scale = saved->scale;
        m_fitScale = saved->fitScale;
        invalidateSceneCache();
        emit viewTransformChanged();
        emit zoomChanged(
            m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
        update();
    } else {
        fitToDrawing();
    }
    emit activeSpaceChanged(target);
}

void CadView::setMeasurements(const QVector<Measurement> &measurements)
{
    m_measurements = measurements;
    for (Measurement &measurement : m_measurements) {
        measurement.normalizeGeometry();
    }
    m_undoStack.clear();
    m_redoStack.clear();
    m_selectedMeasurementId.clear();
    emit measurementsChanged();
    emit selectionChanged({});
    update();
}

void CadView::addTextAnnotation(const QPointF &position, const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }
    Measurement measurement;
    measurement.id = defaultMeasurementId();
    measurement.space = m_activeSpace;
    measurement.type = MeasurementType::Text;
    measurement.note = text.trimmed();
    measurement.points = {position};
    measurement.labelPosition = position;
    measurement.normalizeGeometry();
    pushUndo();
    m_measurements.append(measurement);
    m_selectedMeasurementId = measurement.id;
    emit measurementsChanged();
    emit measurementAdded(measurement.id);
    emit selectionChanged(measurement.id);
    update();
}

void CadView::removeMeasurement(const QString &id)
{
    const int index = measurementIndex(id);
    if (index < 0) {
        return;
    }
    pushUndo();
    m_measurements.removeAt(index);
    if (m_selectedMeasurementId == id) {
        m_selectedMeasurementId.clear();
        emit selectionChanged({});
    }
    emit measurementsChanged();
    update();
}

void CadView::removeAllMeasurements()
{
    if (m_measurements.isEmpty()) {
        return;
    }
    pushUndo();
    m_measurements.clear();
    m_selectedMeasurementId.clear();
    emit measurementsChanged();
    emit selectionChanged({});
    update();
}

void CadView::setMeasurementVisible(const QString &id, bool visible)
{
    const int index = measurementIndex(id);
    if (index < 0 || m_measurements[index].visible == visible) {
        return;
    }
    pushUndo();
    m_measurements[index].visible = visible;
    emit measurementsChanged();
    update();
}

void CadView::setMeasurementNote(const QString &id, const QString &note)
{
    const int index = measurementIndex(id);
    if (index < 0 || m_measurements[index].note == note) {
        return;
    }
    pushUndo();
    m_measurements[index].note = note;
    emit measurementsChanged();
    update();
}

void CadView::focusMeasurement(const QString &id)
{
    const int index = measurementIndex(id);
    if (index < 0) {
        return;
    }
    if (m_measurements[index].space != m_activeSpace) {
        setActiveSpace(m_measurements[index].space);
    }
    m_selectedMeasurementId = id;
    m_center = m_measurements[index].labelPosition;
    invalidateVectorCache();
    if (m_scale <= 0.0) {
        fitToDrawing();
    }
    emit selectionChanged(id);
    emit viewTransformChanged();
    update();
}

void CadView::selectMeasurement(const QString &id)
{
    m_selectedMeasurementId = id;
    emit selectionChanged(id);
    update();
}

void CadView::beginTool(ToolMode mode, double auxiliaryValue)
{
    if (m_mode == ToolMode::Calibrate) {
        m_pendingCalibrationRawLength = 0.0;
        m_calibrationPoints.clear();
    }
    m_mode = mode;
    m_toolAuxiliaryValue = auxiliaryValue;
    m_hoveredMeasurementId.clear();
    clearDraft();
    emit toolChanged(mode);
    emit statusMessage(toolPrompt());
    setCursor(Qt::BlankCursor);
    setFocus();
    update();
}

void CadView::finishCurrentMeasurement()
{
    if (m_awaitingLabel || m_draftPoints.size() < 2) {
        return;
    }
    switch (m_mode) {
    case ToolMode::Continuous:
    case ToolMode::SideArea:
    case ToolMode::Batch:
    case ToolMode::Polygon:
        finishDraftForLabel();
        break;
    default:
        break;
    }
}

void CadView::cancelCurrentTool()
{
    const bool canceledPrintSelection =
        m_mode == ToolMode::PrintArea;
    m_pendingCalibrationRawLength = 0.0;
    m_calibrationPoints.clear();
    clearDraft();
    emit statusMessage(toolPrompt());
    update();
    if (canceledPrintSelection) {
        emit printAreaSelectionCanceled();
    }
}

void CadView::resetView()
{
    fitToInitialView();
}

void CadView::clearDraft()
{
    m_draftPoints.clear();
    m_usedContinuousArcIndices.clear();
    m_awaitingLabel = false;
    m_polygonClosurePrompted = false;
    m_labelPreview = QPointF();
    m_committablePreview.reset();
}

void CadView::fitToDrawing()
{
    if (!m_document) {
        return;
    }
    fitToBounds(m_document->bounds);
}

void CadView::fitToInitialView()
{
    if (!m_document) {
        return;
    }
    // CAD files persist the active model viewport (VIEWCTR / VIEWSIZE). Restore
    // that camera on first open, matching the reference viewer's location and
    // zoom. Entity-cluster fitting is only a fallback when no view was saved.
    if (!m_document->isPdf() && m_document->hasSavedView
        && m_document->savedViewWidth > 0.0
        && m_document->savedViewHeight > 0.0) {
        const QSizeF viewport(size());
        const double availableWidth = qMax(1.0, viewport.width());
        const double availableHeight = qMax(1.0, viewport.height());
        m_scale = qMax(
            1.0e-14,
            qMin(availableWidth / m_document->savedViewWidth,
                 availableHeight / m_document->savedViewHeight));
        m_fitScale = m_scale;
        m_center = m_document->savedViewCenter;
        m_initialFitPending = false;
        invalidateSceneCache();
        if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
            std::fprintf(
                stderr,
                "saved-view center=(%.3f,%.3f) size=%gx%g "
                "viewport=%dx%d scale=%.9g\n",
                m_center.x(), m_center.y(),
                m_document->savedViewWidth,
                m_document->savedViewHeight,
                int(viewport.width()), int(viewport.height()), m_scale);
        }
        emit viewTransformChanged();
        emit zoomChanged(100.0);
        scheduleSceneRefresh();
        return;
    }
    fitToBounds(m_document->isPdf()
                    ? m_document->bounds
                    : cadInitialViewBounds(m_document->entities,
                                           m_document->bounds));
}

void CadView::fitToBounds(const QRectF &bounds)
{
    if (!m_document || !bounds.isValid()) {
        return;
    }
    const double availableWidth = qMax(1.0, width() * 0.92);
    const double availableHeight = qMax(1.0, height() * 0.92);
    const double xScale = availableWidth
        / qMax(1.0e-9, bounds.width());
    const double yScale = availableHeight
        / qMax(1.0e-9, bounds.height());
    m_scale = qMax(1.0e-12, qMin(xScale, yScale));
    m_fitScale = m_scale;
    m_center = bounds.center();
    m_initialFitPending = false;
    invalidateSceneCache();
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(stderr,
                     "fit bounds=(%.3f,%.3f,%.3f,%.3f) viewport=%dx%d "
                     "scale=%.9g center=(%.3f,%.3f)\n",
                     bounds.left(), bounds.top(), bounds.right(),
                     bounds.bottom(), width(), height(), m_scale,
                     m_center.x(), m_center.y());
    }
    emit viewTransformChanged();
    emit zoomChanged(100.0);
    scheduleSceneRefresh();
}

void CadView::zoomAt(qreal factor, const QPoint &anchor)
{
    if (!m_document || factor <= 0.0) {
        return;
    }
    const QPointF before = screenToWorld(anchor);
    m_scale = qBound(1.0e-14, m_scale * factor, 1.0e14);
    const QPointF after = screenToWorld(anchor);
    m_center += before - after;
    // A zoom only changes the affine transform of the world-space geometry
    // bitmap; the cached pixels stay valid and are reused by the interaction
    // path. Invalidating here forced a full CPU re-render on *every* wheel
    // notch, which was the dominant zoom-time stall. The cache is rebuilt
    // once, at full quality, when the zoom gesture finishes.
    m_zoomPreviewActive = true;
    const quint64 generation = ++m_zoomPreviewGeneration;
    emit viewTransformChanged();
    emit zoomChanged(m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
    requestRenderFrame();
    // Debounce the end-of-gesture rebuild. This is intentionally independent
    // of smoothZoom: a 0ms timer used to fire between individual wheel
    // notches and turn every zoom step into a full CPU re-render.
    QTimer::singleShot(120, this, [this, generation] {
        if (generation != m_zoomPreviewGeneration) {
            return;
        }
        m_zoomPreviewActive = false;
        scheduleSceneRefresh();
    });
}

void CadView::zoomToWorldRect(const QRectF &area)
{
    if (!m_document || !area.isValid()
        || area.width() <= 1.0e-12 || area.height() <= 1.0e-12) {
        return;
    }
    const QSizeF viewport =
        m_renderSize.isValid() ? m_renderSize : QSizeF(size());
    const double availableWidth = qMax(1.0, viewport.width() * 0.96);
    const double availableHeight = qMax(1.0, viewport.height() * 0.96);
    m_scale = qBound(
        1.0e-14,
        qMin(availableWidth / area.width(),
             availableHeight / area.height()),
        1.0e14);
    m_center = area.center();
    m_zoomPreviewActive = false;
    ++m_zoomPreviewGeneration;
    invalidateSceneCache();
    emit viewTransformChanged();
    emit zoomChanged(m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
    scheduleSceneRefresh();
}

void CadView::setDisplayUnit(MeasuringUnit unit)
{
    m_settings.lengthUnit = unit;
    emit measurementsChanged();
    update();
}

void CadView::setDisplaySettings(const DisplaySettings &settings)
{
    const QColor previousAnnotationColor =
        m_settings.annotationColor;
    m_settings = settings;
    m_settings.lengthPrecision = qBound(0, m_settings.lengthPrecision, 6);
    m_settings.areaPrecision = qBound(0, m_settings.areaPrecision, 6);
    if (m_settings.annotationColor != previousAnnotationColor) {
        for (Measurement &measurement : m_measurements) {
            measurement.color = m_settings.annotationColor;
        }
    }
    invalidateSceneCache();
    emit measurementsChanged();
    scheduleSceneRefresh();
}

double CadView::annotationTextPixelSize() const
{
    const double pixelsPerUnit = std::isfinite(m_scale) ? m_scale : 1.0;
    double sourceTextPixels =
        pixelsPerUnit * dimensionReferenceTextHeightWorld();
    if (!m_printStyle) {
        sourceTextPixels = m_settings.annotationScaleWithDrawing
            ? qMax(sourceTextPixels,
                   kMinimumReadableDimensionTextPixels)
            : kDefaultDimensionTextPixels;
    }
    return sourceTextPixels
        * m_currentMeasurementScale
        * qBound(0.5, m_settings.dimensionTextScale, 2.5);
}

void CadView::setPrecision(int precision)
{
    m_settings.lengthPrecision = qBound(0, precision, 6);
    m_settings.areaPrecision = m_settings.lengthPrecision;
    emit measurementsChanged();
    update();
}

void CadView::showAllMeasurements()
{
    if (m_measurements.isEmpty()) {
        return;
    }
    pushUndo();
    for (Measurement &measurement : m_measurements) {
        measurement.visible = true;
    }
    emit measurementsChanged();
    update();
}

void CadView::hideAllMeasurements()
{
    if (m_measurements.isEmpty()) {
        return;
    }
    pushUndo();
    for (Measurement &measurement : m_measurements) {
        measurement.visible = false;
    }
    emit measurementsChanged();
    update();
}

void CadView::setLineWeightsVisible(bool visible)
{
    m_lineWeightsVisible = visible;
    update();
}

void CadView::setSnapModes(const SnapModes &modes)
{
    m_snapModes = modes;
}

void CadView::setSnapEnabled(bool enabled)
{
    m_snapEnabled = enabled;
}

void CadView::setGridVisible(bool visible)
{
    m_gridVisible = visible;
    update();
}

void CadView::setOrthoEnabled(bool enabled)
{
    m_orthoEnabled = enabled;
    update();
}

void CadView::setRenderingSettings(const RenderingSettings &settings)
{
    m_renderingSettings = settings;
    m_lineWeightsVisible = settings.lineWeights;
    invalidateDrawingCache();
}

bool CadView::antialiasingEnabled() const
{
    return m_printStyle
        || (!m_panning && !m_zoomPreviewActive
            && m_renderingSettings.antialiasing
            && m_renderingSettings.quality
                != RenderingQuality::Performance);
}

qreal CadView::renderCachePixelRatio() const
{
    switch (m_renderingSettings.quality) {
    case RenderingQuality::High:
        return qBound<qreal>(1.0, devicePixelRatioF(), 2.0);
    case RenderingQuality::Balanced:
        return qBound<qreal>(1.0, devicePixelRatioF(), 1.5);
    case RenderingQuality::Performance:
        return qBound<qreal>(1.0, devicePixelRatioF(), 1.25);
    }
    return 1.0;
}

void CadView::applyCalibration(double knownLength,
                               MeasuringUnit entryUnit)
{
    if (m_document && knownLength > 0.0
        && m_pendingCalibrationRawLength > 0.0) {
        m_document->metersPerDrawingUnit =
            knownLength * measuringUnitMeters(entryUnit)
            / m_pendingCalibrationRawLength;
        m_document->calibrated = true;
        if (m_calibrationPoints.size() >= 2) {
            Measurement dimension;
            dimension.id = defaultMeasurementId();
            dimension.space = m_activeSpace;
            dimension.type = MeasurementType::Aligned;
            dimension.points = m_calibrationPoints.mid(0, 2);
            QPointF direction =
                dimension.points[1] - dimension.points[0];
            QPointF normal(-direction.y(), direction.x());
            const double normalLength =
                std::hypot(normal.x(), normal.y());
            if (normalLength > 1.0e-12) {
                normal /= normalLength;
            }
            dimension.labelPosition =
                (dimension.points[0] + dimension.points[1]) * 0.5
                + normal * (20.0 / qMax(1.0e-12, m_scale));
            dimension.color = m_settings.annotationColor;
            dimension.normalizeGeometry();
            pushUndo();
            m_measurements.append(dimension);
            m_selectedMeasurementId = dimension.id;
            emit measurementAdded(dimension.id);
            emit selectionChanged(dimension.id);
        }
        m_pendingCalibrationRawLength = 0.0;
        m_calibrationPoints.clear();
        emit measurementsChanged();
        update();
        beginTool(ToolMode::Select);
    }
}

bool CadView::undo()
{
    if (m_undoStack.isEmpty()) {
        return false;
    }
    m_redoStack.append(m_measurements);
    m_measurements = m_undoStack.takeLast();
    m_selectedMeasurementId.clear();
    emit measurementsChanged();
    emit selectionChanged({});
    update();
    return true;
}

bool CadView::redo()
{
    if (m_redoStack.isEmpty()) {
        return false;
    }
    m_undoStack.append(m_measurements);
    m_measurements = m_redoStack.takeLast();
    m_selectedMeasurementId.clear();
    emit measurementsChanged();
    emit selectionChanged({});
    update();
    return true;
}

void CadView::pushUndo()
{
    m_undoStack.append(m_measurements);
    if (m_undoStack.size() > 80) {
        m_undoStack.removeFirst();
    }
    m_redoStack.clear();
}

int CadView::measurementIndex(const QString &id) const
{
    for (int i = 0; i < m_measurements.size(); ++i) {
        if (m_measurements[i].id == id) {
            return i;
        }
    }
    return -1;
}

QPointF CadView::worldToScreen(const QPointF &world) const
{
    return worldToScreenTransform().map(world);
}

QTransform CadView::worldToScreenTransform() const
{
    const QSizeF viewport =
        m_renderSize.isValid() ? m_renderSize : QSizeF(size());
    return QTransform(
        m_scale, 0.0, 0.0, -m_scale,
        viewport.width() * 0.5 - m_center.x() * m_scale,
        viewport.height() * 0.5 + m_center.y() * m_scale);
}

QPointF CadView::screenToWorld(const QPointF &screen) const
{
    const QSizeF viewport =
        m_renderSize.isValid() ? m_renderSize : QSizeF(size());
    return QPointF(
        (screen.x() - viewport.width() * 0.5) / m_scale + m_center.x(),
        (viewport.height() * 0.5 - screen.y()) / m_scale + m_center.y());
}

QRectF CadView::visibleWorldRect() const
{
    const QPointF topLeft = screenToWorld(QPointF(0.0, 0.0));
    const QSizeF viewport =
        m_renderSize.isValid() ? m_renderSize : QSizeF(size());
    const QPointF bottomRight =
        screenToWorld(QPointF(viewport.width(), viewport.height()));
    return QRectF(topLeft, bottomRight).normalized();
}

double CadView::snapRadiusWorld() const
{
    return 12.0 / qMax(1.0e-14, m_scale);
}

double CadView::dimensionReferenceTextHeightWorld() const
{
    if (!m_document) {
        return 1.0;
    }
    const auto cached = m_dimensionTextHeights.constFind(m_activeSpace);
    if (cached != m_dimensionTextHeights.cend()) {
        return cached.value() > 0.0
            ? cached.value()
            : kDefaultDimensionTextPixels / qMax(1.0e-14, m_scale);
    }

    QVector<double> dimensionLayerHeights;
    for (const CadEntity &entity : m_document->entities) {
        if (entity.type != CadEntityType::Text
            || !std::isfinite(entity.textHeight)
            || entity.textHeight <= 1.0e-9) {
            continue;
        }
        bool hasDigit = false;
        for (const QChar character : entity.text) {
            if (character.isDigit()) {
                hasDigit = true;
                break;
            }
        }
        if (!hasDigit) {
            continue;
        }
        const QString layer = entity.layer.toLower();
        if (layer.contains(QStringLiteral("dim"))
            || layer.contains(QStringLiteral("尺寸"))
            || layer.contains(QStringLiteral("标注"))) {
            dimensionLayerHeights.append(entity.textHeight);
        }
    }

    const auto median = [](QVector<double> values) {
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    double result = 0.0;
    if (!dimensionLayerHeights.isEmpty()) {
        result = median(std::move(dimensionLayerHeights));
    }
    if (!std::isfinite(result) || result <= 1.0e-9) {
        m_dimensionTextHeights.insert(m_activeSpace, -1.0);
        return kDefaultDimensionTextPixels / qMax(1.0e-14, m_scale);
    }
    m_dimensionTextHeights.insert(m_activeSpace, result);
    return result;
}

double CadView::annotationGeometryScale() const
{
    // 标注跟随图纸：字高取图纸自身的尺寸文字高度（世界单位），再按当前
    // 缩放换算成屏幕像素 —— 缩放图纸时标注一起放大缩小，与 CAD 内的
    // 标注实体一致。annotationStyleMetrics().textSize 是基准像素值，
    // 因此这里返回 px/基准像素 的比例系数。
    const double referenceHeight = dimensionReferenceTextHeightWorld();
    const double pixelsPerUnit = std::isfinite(m_scale) ? m_scale : 1.0;
    const double baseTextSize =
        qMax(1.0, annotationStyleMetrics(m_settings.annotationStyle)
                      .textSize);
    double sourceTextPixels = pixelsPerUnit * referenceHeight;
    if (m_applyingDimensionTextScale && !m_printStyle) {
        sourceTextPixels = m_settings.annotationScaleWithDrawing
            ? qMax(sourceTextPixels,
                   kMinimumReadableDimensionTextPixels)
            : kDefaultDimensionTextPixels;
    }
    const double scale = sourceTextPixels / baseTextSize;
    if (!std::isfinite(scale) || scale <= 0.0) {
        return 1.0;
    }
    const double dimensionScale =
        m_applyingDimensionTextScale
        ? qBound(0.5, m_settings.dimensionTextScale, 2.5) : 1.0;
    return scale * m_currentMeasurementScale * dimensionScale;
}

void CadView::updateSnap(const QPointF &screenPoint)
{
    m_cursorScreen = screenPoint;
    m_cursorWorld = screenToWorld(screenPoint);
    m_snap = {};
    // Object snap stays live even in the idle Select tool, exactly like
    // AutoCAD: the crosshair is pulled onto the snap point and the marker
    // shows what kind of point was found.
    if (m_snapEnabled && m_document && !m_awaitingLabel) {
        m_snap =
            m_snapEngine.findSnap(m_cursorWorld, snapRadiusWorld(), m_snapModes);
    }
    if (m_snap.valid) {
        m_cursorWorld = m_snap.point;
        emit snapChanged(m_snap.label);
    } else {
        emit snapChanged({});
    }
    const bool forceOrthogonal =
        m_orthoEnabled || m_mode == ToolMode::Linear;
    if (forceOrthogonal && !m_awaitingLabel && !m_draftPoints.isEmpty()) {
        const QPointF origin = m_draftPoints.last();
        const QPointF delta = m_cursorWorld - origin;
        m_cursorWorld =
            qAbs(delta.x()) >= qAbs(delta.y())
            ? QPointF(m_cursorWorld.x(), origin.y())
            : QPointF(origin.x(), m_cursorWorld.y());
        // Ortho moved the point away from the snap: the marker would lie.
        if (m_snap.valid
            && QLineF(m_snap.point, m_cursorWorld).length()
                > snapRadiusWorld() * 1.0e-3) {
            m_snap = {};
            emit snapChanged({});
        }
    }
    if (m_awaitingLabel) {
        m_labelPreview = m_cursorWorld;
    }
    const bool nearPolygonStart =
        m_mode == ToolMode::Polygon && !m_awaitingLabel
        && m_draftPoints.size() >= 3
        && QLineF(worldToScreen(m_draftPoints.first()), screenPoint).length()
            <= 14.0;
    m_polygonClosurePrompted = nearPolygonStart;
    m_displayScreen = worldToScreen(m_cursorWorld);
    emit cursorWorldChanged(m_cursorWorld);
}

void CadView::initializeGL()
{
    initializeOpenGLFunctions();
    glEnable(GL_MULTISAMPLE);
    const QColor background = canvasBackgroundColor();
    glClearColor(background.redF(), background.greenF(),
                 background.blueF(), background.alphaF());
}

void CadView::resizeGL(int width, int height)
{
    Q_UNUSED(width);
    Q_UNUSED(height);
    invalidateSceneCache();
    if (m_initialFitPending && m_document) {
        QTimer::singleShot(20, this, [this] {
            if (m_initialFitPending && m_document) {
                fitToInitialView();
            }
        });
    } else {
        scheduleSceneRefresh();
    }
}

void CadView::paintGL()
{
    m_renderSize = QSizeF(size());
    m_printStyle = false;
    QElapsedTimer frameTimer;
    frameTimer.start();
    QPainter painter(this);
    renderScene(painter, size());
    const double elapsedMs = frameTimer.nsecsElapsed() / 1.0e6;
    // Asymmetric smoothing: react quickly when frames get cheap again,
    // slowly when one outlier spikes.
    const double alpha = elapsedMs > m_frameCostEmaMs ? 0.25 : 0.5;
    m_frameCostEmaMs = m_frameCostEmaMs <= 0.0
        ? elapsedMs
        : m_frameCostEmaMs + alpha * (elapsedMs - m_frameCostEmaMs);
}

void CadView::scheduleSceneRefresh()
{
    update();
}

void CadView::invalidateSceneCache()
{
    invalidateVectorCache();
}

void CadView::invalidateVectorCache()
{
    m_vectorCacheValid = false;
    m_vectorCache = QImage();
    m_vectorCacheSize = QSize();
    m_vectorCachePad = 0;
    m_vectorCacheCenter = QPointF();
    m_vectorCacheScale = 0.0;
}

void CadView::invalidateDrawingCache()
{
    invalidateSceneCache();
    update();
}

QImage CadView::renderToImage(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (!m_document || size.isEmpty()) {
        return image;
    }
    const QSizeF previousSize = m_renderSize;
    const QPointF previousCenter = m_center;
    const double previousScale = m_scale;
    const double availableWidth = qMax(80.0, size.width() - 100.0);
    const double availableHeight = qMax(80.0, size.height() - 100.0);
    if (m_activeSpace == QStringLiteral("模型")
        && m_document->hasSavedView
        && m_document->savedViewHeight > 0.0) {
        m_scale = qMax(
            1.0e-14,
            qMin(availableWidth
                     / qMax(1.0e-9, m_document->savedViewWidth),
                 availableHeight / m_document->savedViewHeight));
        m_center = m_document->savedViewCenter;
    } else if (m_activeSpace == QStringLiteral("模型")
               && !m_document->isPdf()) {
        // Snapshots and exports must match the interactive initial view:
        // the raw bounds can be inflated by stray entities far outside the
        // actual content cluster.
        const QRectF initialBounds = cadInitialViewBounds(
            m_document->entities, m_document->bounds);
        m_scale = qMax(
            1.0e-14,
            qMin(availableWidth / qMax(1.0e-9, initialBounds.width()),
                 availableHeight
                     / qMax(1.0e-9, initialBounds.height())));
        m_center = initialBounds.center();
    } else {
        m_scale = qMax(
            1.0e-14,
            qMin(availableWidth
                     / qMax(1.0e-9, m_document->bounds.width()),
                 availableHeight
                     / qMax(1.0e-9, m_document->bounds.height())));
        m_center = m_document->bounds.center();
    }
    m_renderSize = QSizeF(size);
    m_printStyle = false;
    {
        QPainter painter(&image);
        renderScene(painter, size, true);
    }
    m_renderSize = previousSize;
    m_center = previousCenter;
    m_scale = previousScale;
    return image;
}

QImage CadView::renderWorldRectToImage(const QRectF &area,
                                       const QSize &size,
                                       bool includePaperSpace)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (!m_document || size.isEmpty() || !area.isValid()) {
        return image;
    }
    const QSizeF previousSize = m_renderSize;
    const QPointF previousCenter = m_center;
    const double previousScale = m_scale;
    const bool previousSkipPaperSpace = m_skipPaperSpace;
    m_skipPaperSpace = !includePaperSpace;
    m_center = area.center();
    m_scale = qMax(
        1.0e-14,
        qMin(qMax(1.0, size.width() - 20.0)
                 / qMax(1.0e-9, area.width()),
             qMax(1.0, size.height() - 20.0)
                 / qMax(1.0e-9, area.height())));
    m_renderSize = QSizeF(size);
    m_printStyle = false;
    {
        QPainter painter(&image);
        renderScene(painter, size, true);
    }
    m_renderSize = previousSize;
    m_center = previousCenter;
    m_scale = previousScale;
    m_skipPaperSpace = previousSkipPaperSpace;
    return image;
}

QImage CadView::renderPrintAreaToImage(const QRectF &area,
                                       const QSize &size,
                                       bool blackInk)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    if (!m_document || size.isEmpty() || !area.isValid()
        || area.width() <= 0.0 || area.height() <= 0.0) {
        return image;
    }
    const QSizeF previousSize = m_renderSize;
    const QPointF previousCenter = m_center;
    const double previousScale = m_scale;
    const bool previousPrintStyle = m_printStyle;
    const bool previousBlackInk = m_printBlackInk;
    const double margin =
        qMin(size.width(), size.height()) * 0.035;
    const double availableWidth =
        qMax(1.0, size.width() - margin * 2.0);
    const double availableHeight =
        qMax(1.0, size.height() - margin * 2.0);
    m_renderSize = QSizeF(size);
    m_center = area.center();
    m_scale = qMin(availableWidth / area.width(),
                   availableHeight / area.height());
    m_printStyle = true;
    m_printBlackInk = blackInk;
    {
        QPainter painter(&image);
        const QRectF contentRect(
            (size.width() - area.width() * m_scale) * 0.5,
            (size.height() - area.height() * m_scale) * 0.5,
            area.width() * m_scale,
            area.height() * m_scale);
        painter.setClipRect(contentRect);
        renderScene(painter, size, true);
    }
    m_renderSize = previousSize;
    m_center = previousCenter;
    m_scale = previousScale;
    m_printStyle = previousPrintStyle;
    m_printBlackInk = previousBlackInk;
    return image;
}

QImage CadView::renderPrintPageToImage(
    const CadPrintPage &page,
    const QSize &size,
    bool blackInk)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    if (!m_document || size.isEmpty() || !page.worldArea.isValid()) {
        return image;
    }
    const QString targetSpace = page.spaceName.isEmpty()
        ? m_activeSpace : page.spaceName;
    const bool spaceExists = m_document->isPdf()
        ? m_document->pdfPage(targetSpace) != nullptr
        : (targetSpace == QStringLiteral("模型")
           || m_document->layoutEntities.contains(targetSpace));
    if (!spaceExists) {
        return image;
    }
    const QString oldSpace = m_activeSpace;
    const QSizeF oldSize = m_renderSize;
    const QPointF oldCenter = m_center;
    const double oldScale = m_scale;
    const double oldFitScale = m_fitScale;
    if (targetSpace != m_activeSpace) {
        setActiveSpace(targetSpace);
    }
    image = renderPrintAreaToImage(page.worldArea, size, blackInk);
    if (m_activeSpace != oldSpace) {
        setActiveSpace(oldSpace);
    }
    m_renderSize = oldSize;
    m_center = oldCenter;
    m_scale = oldScale;
    m_fitScale = oldFitScale;
    invalidateSceneCache();
    emit viewTransformChanged();
    update();
    return image;
}

void CadView::renderScene(QPainter &painter, const QSize &viewport,
                          bool exporting)
{
    m_exportingScene = exporting;
    painter.setClipRect(
        QRect(QPoint(0, 0), viewport), Qt::IntersectClip);
    painter.setRenderHint(QPainter::Antialiasing,
                          antialiasingEnabled());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setRenderHint(QPainter::TextAntialiasing, false);
    drawBackground(painter, viewport, exporting);
    drawPdfPage(painter);
    drawCadEntities(painter, viewport);
    drawMeasurements(painter);
    drawDraft(painter);
    drawEraseRectangle(painter);
    if (m_mode == ToolMode::Erase && !m_hoveredMeasurementId.isEmpty()
        && !m_eraseDragging) {
        drawDeleteHoverMarker(painter);
    }

    if (!exporting && m_haveCursor && !m_panning
        && !m_dragLabel && !m_spaceDown) {
        painter.save();
        painter.setClipRect(QRect(QPoint(0, 0), viewport));
        // The crosshair centre is the *effective* point: it is pulled onto
        // the snap point exactly like AutoCAD, so what you see is what a
        // click will pick.
        const QPointF shot = m_displayScreen;
        const bool snapped = m_snap.valid;
        QColor baseColor = m_settings.crosshairColor;
        if (!baseColor.isValid()) {
            baseColor = QColor(QStringLiteral("#ffffff"));
        }
        const QColor ink = snapped ? QColor(QStringLiteral("#5ff0c1"))
                                   : baseColor;
        const double crosshairLineWidth = qBound(
            0.5, m_settings.crosshairLineWidth, 4.0);
        const int crosshairPercent = qBound(
            1, m_settings.crosshairLength, 100);
        // AutoCAD-style relative size: the configured percentage is based on
        // the viewport width, not on the shorter viewport dimension.
        const double halfLength =
            viewport.width() * crosshairPercent / 200.0;
        const double left = shot.x() - halfLength;
        const double right = shot.x() + halfLength;
        const double top = shot.y() - halfLength;
        const double bottom = shot.y() + halfLength;
        // Soft magnet halo underneath the marker.
        if (snapped) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(95, 240, 193, 34));
            painter.drawEllipse(shot, 14.0, 14.0);
            painter.setBrush(Qt::NoBrush);
        }
        QColor lineColor = ink;
        lineColor.setAlpha(snapped ? 230 : 218);
        QPen crosshairPen(lineColor, crosshairLineWidth);
        crosshairPen.setCosmetic(true);
        painter.setPen(crosshairPen);
        painter.drawLine(QPointF(left, shot.y()), QPointF(right, shot.y()));
        painter.drawLine(QPointF(shot.x(), top), QPointF(shot.x(), bottom));
        if (m_settings.crosshairCenterBoxVisible) {
            // AutoCAD-style pick box centred on the crosshair.
            painter.setBrush(Qt::NoBrush);
            QColor pickColor = ink.lighter(120);
            pickColor.setAlpha(255);
            painter.setPen(
                QPen(pickColor, qMax(1.0, crosshairLineWidth)));
            const double halfPick = qBound(
                1.0, m_settings.crosshairCenterBoxSize, 24.0) * 0.5;
            painter.drawRect(
                QRectF(shot.x() - halfPick, shot.y() - halfPick,
                       halfPick * 2.0, halfPick * 2.0));
        }
        if (snapped) {
            drawSnapMarker(painter, shot, m_snap.kind);
        }
        painter.restore();
    }
}

void CadView::drawPdfPage(QPainter &painter)
{
    if (!m_document || !m_document->isPdf()) {
        return;
    }
    CadPdfPage *page = m_document->pdfPage(m_activeSpace);
    if (!page || page->image.isNull()) {
        return;
    }
    const QImage *sourceImage = &page->image;
    if (m_printStyle && m_printBlackInk) {
        if (page->printImage.size() != page->image.size()) {
            page->printImage = page->image.convertToFormat(
                QImage::Format_Mono, Qt::ThresholdDither);
            page->printImage =
                page->printImage.convertToFormat(
                    QImage::Format_ARGB32_Premultiplied);
        }
        sourceImage = &page->printImage;
    }
    const QPointF topLeft = worldToScreen(
        QPointF(page->pageRect.left(), page->pageRect.top()));
    const QPointF bottomRight = worldToScreen(
        QPointF(page->pageRect.right(), page->pageRect.bottom()));
    const QRectF target = QRectF(topLeft, bottomRight).normalized();
    if (!target.intersects(QRectF(QPointF(0.0, 0.0), m_renderSize))) {
        return;
    }
    painter.save();
    painter.setRenderHint(
        QPainter::SmoothPixmapTransform,
        m_renderingSettings.smoothZoom);
    painter.drawImage(target, *sourceImage);
    painter.restore();
}

void CadView::drawBackground(QPainter &painter, const QSize &viewport,
                             bool exporting)
{
    Q_UNUSED(exporting);
    painter.fillRect(QRect(QPoint(0, 0), viewport),
                     m_printStyle ? QColor(Qt::white)
                                  : canvasBackgroundColor());
    if (m_printStyle) {
        return;
    }
    if (!m_document || m_scale <= 0.0 || !m_gridVisible) {
        return;
    }
    const double targetWorld = 90.0 / m_scale;
    const double exponent = std::floor(std::log10(qMax(1.0e-15, targetWorld)));
    const double base = std::pow(10.0, exponent);
    const double fraction = targetWorld / base;
    const double step = base * (fraction > 5.0 ? 5.0 : fraction > 2.0 ? 2.0
                                                                      : 1.0);
    const QRectF world = visibleWorldRect();
    const double left = std::floor(world.left() / step) * step;
    const double top = std::floor(world.top() / step) * step;
    QColor gridColor;
    switch (m_drawingTheme) {
    case DrawingTheme::CadLight:
        gridColor = QColor(110, 122, 128, 64);
        break;
    case DrawingTheme::CadGray:
        gridColor = QColor(185, 198, 205, 48);
        break;
    case DrawingTheme::CadBlueprint:
        gridColor = QColor(139, 215, 244, 44);
        break;
    case DrawingTheme::CadDark:
        gridColor = QColor(44, 50, 56, 95);
        break;
    }
    painter.setPen(QPen(gridColor, 0.55));
    QVector<QLineF> lines;
    lines.reserve(200);
    for (double x = left; x <= world.right() + step; x += step) {
        const double screenX = worldToScreen(QPointF(x, 0.0)).x();
        lines.append(QLineF(screenX, 0.0, screenX, viewport.height()));
    }
    for (double y = top; y <= world.bottom() + step; y += step) {
        const double screenY = worldToScreen(QPointF(0.0, y)).y();
        lines.append(QLineF(0.0, screenY, viewport.width(), screenY));
    }
    painter.drawLines(lines);
}

void CadView::drawCadEntities(QPainter &painter, const QSize &viewport)
{
    QElapsedTimer renderTimer;
    renderTimer.start();
    // Opt-in stage profiling: CADLENS_PROFILE_RENDER=1 prints where a full
    // geometry rebuild actually spends its time (spatial query, batch
    // building, hatch resolution/generation/rasterising, line batches,
    // text). Off by default so it costs nothing in normal use.
    const bool profile =
        qEnvironmentVariableIsSet("CADLENS_PROFILE_RENDER");
    QElapsedTimer stageTimer;
    stageTimer.start();
    auto mark = [&]() -> double {
        if (!profile) {
            return 0.0;
        }
        const double ms = stageTimer.nsecsElapsed() / 1.0e6;
        stageTimer.restart();
        return ms;
    };
    // Diagnostic-only switches for isolating hatch rasterisation cost.
    const bool hatchNoClip =
        qEnvironmentVariableIsSet("CADLENS_HATCH_NO_CLIP");
    const bool hatchFlat =
        qEnvironmentVariableIsSet("CADLENS_HATCH_FLAT");
    // A/B switch: force the old complex-path clip for pixel comparison.
    const bool hatchForcePathClip =
        qEnvironmentVariableIsSet("CADLENS_HATCH_FORCE_PATH_CLIP");
    double hatchResolveMs = 0.0;
    double hatchGenerateMs = 0.0;
    double hatchDrawMs = 0.0;
    quint64 hatchDeflinesVisited = 0;
    quint64 hatchDeflinesGenerated = 0;
    quint64 hatchGeneratedLines = 0;
    quint64 hatchRectClipGroups = 0;
    quint64 hatchRectClipLines = 0;
    if (!m_document || m_spatialIndex.isEmpty()) {
        return;
    }
    const bool interacting = m_panning || m_zoomPreviewActive;
    // The CAD geometry layer is static in world space: while panning or
    // zooming, the cached full-quality bitmap only needs an affine transform
    // to match the new view. Rebuilding every path, hatch line and screen
    // coordinate on each interaction frame is what makes large drawings
    // stutter. A timed refresh inside the gesture used to inject a several
    // hundred millisecond CPU frame every ~140ms; refresh now happens only
    // when the gesture ends (or when the overdraw margin is exhausted).
    // Set CADLENS_DISABLE_INTERACTION_BITMAP=1 for the previous behaviour.
    const bool verbose = qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER");
    const bool bitmapEnabled =
        interacting && !m_printStyle && !m_exportingScene
        && !m_buildingVectorCache
        && !qEnvironmentVariableIsSet("CADLENS_DISABLE_INTERACTION_BITMAP");
    if (bitmapEnabled && m_vectorCacheValid
        && m_vectorCacheSize == viewport && m_vectorCacheScale > 0.0) {
        const double ratio = m_scale / m_vectorCacheScale;
        const QSizeF cachedViewport(m_vectorCacheSize);
        const double panX =
            (m_vectorCacheCenter.x() - m_center.x()) * m_scale;
        const double panY =
            (m_vectorCacheCenter.y() - m_center.y()) * m_scale;
        const bool usable = std::isfinite(ratio) && ratio > 0.0
            && std::isfinite(panX) && std::isfinite(panY);
        // Exact coverage test. With A/B below, current screen coordinates
        // relate to the cached view by p2 = (ratio * p1.x + A,
        //                                   ratio * p1.y + B).
        // The padded bitmap contains cached-view points in
        // [-pad, W+pad] x [-pad, H+pad]. Inverting the visible viewport
        // into that space must stay inside the bitmap, otherwise a gap
        // would appear. Unlike fixed pan/zoom thresholds this is correct
        // for any wheel anchor, including zooms at the screen edge.
        const double pad = double(m_vectorCachePad);
        const double W = cachedViewport.width();
        const double H = cachedViewport.height();
        const double A = W * 0.5 * (1.0 - ratio) + panX;
        const double B = H * 0.5 * (1.0 - ratio) - panY;
        const bool covered = usable && pad > 0.0
            && A <= ratio * pad
            && A >= W - ratio * (W + pad)
            && B <= ratio * pad
            && B >= H - ratio * (H + pad);
        // Zooming in only magnifies existing pixels; the DPR supersampled
        // cache stays sharp up to dpr. Beyond that, redraw at full quality.
        const bool sharpEnough =
            ratio <= qMax(1.25, renderCachePixelRatio() * 0.95);
        if (covered && sharpEnough) {
            // Cached pixel q represents cached-view point q - pad, so the
            // blit offset is (A, B) shifted by ratio * pad.
            const double offsetX = A - ratio * pad;
            const double offsetY = B - ratio * pad;
            painter.save();
            // Smooth only when actually scaling: a pure pan stays pixel-crisp.
            if (!qFuzzyCompare(ratio, 1.0)) {
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            }
            painter.translate(offsetX, offsetY);
            painter.scale(ratio, ratio);
            painter.drawImage(QPointF(0.0, 0.0), m_vectorCache);
            painter.restore();
            ++m_interactionBitmapFrames;
            if (verbose && (m_interactionBitmapFrames % 60 == 1)) {
                std::fprintf(stderr,
                             "bitmap frame #%d ratio=%.6g elapsed=%.2fms\n",
                             m_interactionBitmapFrames, ratio,
                             renderTimer.nsecsElapsed() / 1.0e6);
            }
            return;
        }
        if (verbose) {
            std::fprintf(stderr,
                         "interaction refresh after %d bitmap frames "
                         "ratio=%.6g pan=(%.1f,%.1f) pad=%.0f covered=%d\n",
                         m_interactionBitmapFrames, ratio, panX, panY,
                         pad, covered ? 1 : 0);
        }
        m_interactionBitmapFrames = 0;
    }
    // Every interaction frame that does need a real redraw uses the same
    // full-quality result as the static view. The old translucent "preview"
    // fills were a second, inconsistent appearance that showed up only while
    // dragging, so they are no longer produced here.
    const bool interactivePreview =
        interacting && !bitmapEnabled && !m_printStyle
        && !m_exportingScene && !m_buildingVectorCache;
    double lodTolerance = 0.35;
    double entityMinimumPixels = 0.08;
    double minimumTextPixels = 1.5;
    int textLimit = 2200;
    switch (m_renderingSettings.quality) {
    case RenderingQuality::High:
        lodTolerance = 0.15;
        entityMinimumPixels = 0.05;
        minimumTextPixels = 0.9;
        textLimit = 3200;
        break;
    case RenderingQuality::Balanced:
        lodTolerance = 0.35;
        entityMinimumPixels = 0.08;
        minimumTextPixels = 1.5;
        textLimit = 2200;
        break;
    case RenderingQuality::Performance:
        lodTolerance = 0.95;
        entityMinimumPixels = 0.35;
        minimumTextPixels = 3.0;
        textLimit = 1100;
        break;
    }
    if (interactivePreview) {
        lodTolerance = qMax(lodTolerance + 0.80, 1.6);
    }
    if (m_printStyle) {
        lodTolerance = 0.05;
        entityMinimumPixels = 0.0;
        minimumTextPixels = 0.25;
        textLimit = std::numeric_limits<int>::max();
    }
    const bool cacheViewMatches =
        m_vectorCacheCenter == m_center
        && qFuzzyCompare(1.0 + m_vectorCacheScale, 1.0 + m_scale);
    if (!interactivePreview && !m_printStyle && !m_buildingVectorCache) {
        if (!m_vectorCacheValid || m_vectorCacheSize != viewport
            || !cacheViewMatches) {
            const qreal dpr = renderCachePixelRatio();
            // Screen caches carry an overdraw margin so a pan/zoom gesture
            // can slide real pixels into view instead of forcing a full CPU
            // rebuild. Snapshots/print exports need no margin (and must stay
            // the exact requested pixel size), so they keep pad = 0.
            const int pad = m_exportingScene
                ? 0
                : qBound(64,
                         qMin(viewport.width(), viewport.height()) / 5,
                         192);
            const QSize padded(viewport.width() + 2 * pad,
                               viewport.height() + 2 * pad);
            QImage image(
                QSize(qMax(1, int(std::ceil(padded.width() * dpr))),
                      qMax(1, int(std::ceil(padded.height() * dpr)))),
                QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            // worldToScreen() and visibleWorldRect() derive their viewport
            // from m_renderSize. Widening it (same centre, same scale) makes
            // cached pixel q represent screen point q - pad, i.e. every side
            // gains `pad` px of genuinely rendered geometry.
            const QSizeF previousRenderSize = m_renderSize;
            if (pad > 0) {
                m_renderSize = QSizeF(padded);
            }
            {
                QPainter cachePainter(&image);
                cachePainter.setRenderHint(
                    QPainter::Antialiasing,
                    antialiasingEnabled());
                m_buildingVectorCache = true;
                drawCadEntities(cachePainter, padded);
                m_buildingVectorCache = false;
            }
            m_renderSize = previousRenderSize;
            m_vectorCache = std::move(image);
            m_vectorCacheSize = viewport;
            m_vectorCachePad = pad;
            m_vectorCacheCenter = m_center;
            m_vectorCacheScale = m_scale;
            m_vectorCacheValid = true;
        }
        painter.drawImage(
            QPointF(-m_vectorCachePad, -m_vectorCachePad), m_vectorCache);
        if (verbose) {
            std::fprintf(stderr,
                         "geometry-cache viewport=%dx%d pad=%d "
                         "elapsed=%.2fms\n",
                         viewport.width(), viewport.height(),
                         m_vectorCachePad,
                         renderTimer.nsecsElapsed() / 1.0e6);
        }
        return;
    }
    const double worldMargin =
        2.0 / qMax(1.0e-14, m_scale);
    const QRectF world =
        visibleWorldRect().adjusted(-worldMargin, -worldMargin,
                                    worldMargin, worldMargin);
    const QVector<int> visible =
        m_spatialIndex.query(world, m_document->entities.size());
    if (!m_printStyle) {
        if (visible.size() > 120000) {
            lodTolerance = qMax(lodTolerance, 0.90);
            entityMinimumPixels = qMax(entityMinimumPixels, 0.22);
            minimumTextPixels = qMax(minimumTextPixels, 2.4);
            textLimit = qMin(textLimit, 1400);
        } else if (visible.size() > 60000) {
            lodTolerance = qMax(lodTolerance, 0.65);
            entityMinimumPixels = qMax(entityMinimumPixels, 0.14);
            minimumTextPixels = qMax(minimumTextPixels, 1.8);
            textLimit = qMin(textLimit, 1800);
        }
    }
    const double minimumScreenEdge =
        qMax(kMinScreenSegment, entityMinimumPixels * 0.75);
    const double queryMs = mark();
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(stderr,
                     "paint viewport=%dx%d scale=%.9g world=(%.3f,%.3f,"
                     "%.3f,%.3f) visible=%lld\n",
                     viewport.width(), viewport.height(), m_scale,
                     world.left(), world.top(), world.right(),
                     world.bottom(), static_cast<long long>(visible.size()));
    }
    painter.setClipRect(
        QRect(QPoint(0, 0), viewport), Qt::IntersectClip);

    struct LineBatch {
        QColor color;
        double width = 0.62;
        QRectF clip;
        QVector<QLineF> lines;
    };
    struct TextBatch {
        QRectF clip;
        QVector<const CadEntity *> entities;
    };
    struct TextCandidate {
        const CadEntity *entity = nullptr;
        double pixelHeight = 0.0;
    };
    QHash<LineBatchKey, LineBatch> batches;
    QHash<ClipKey, TextBatch> textBatches;
    QHash<int, QVector<int>> hatchGroups;
    QVector<int> solids;
    QVector<QPointF> points;
    QVector<TextCandidate> textCandidates;
    QVector<QPointF> screenPoints;
    solids.reserve(qMin(visible.size(), 256));
    batches.reserve(qMin(visible.size(), 64));
    textBatches.reserve(qMin(visible.size(), 32));
    textCandidates.reserve(qMin(visible.size(), textLimit));

    for (int index : visible) {
        const CadEntity &entity = m_document->entities[index];
        const CadLayer *layer = m_document->layerOf(entity);
        if (layer && !layer->visible) {
            continue;
        }
        // Looking "through" a single viewport must show only model-space
        // content. Paper-space geometry (title block, logo, viewport
        // frames) carries no viewport clip rectangle, so skip anything
        // without one when m_skipPaperSpace is set.
        if (m_skipPaperSpace && !entity.clipRect.isValid()) {
            continue;
        }
        if (!isRenderableCadEntity(entity)) {
            continue;
        }
        const double projectedWidth =
            qAbs(entity.bounds.width()) * m_scale;
        const double projectedHeight =
            qAbs(entity.bounds.height()) * m_scale;
        const double projectedExtent =
            qMax(projectedWidth, projectedHeight);
        if (!m_printStyle && entity.type != CadEntityType::Text
            && projectedExtent < entityMinimumPixels) {
            continue;
        }
        if (entity.type == CadEntityType::Text) {
            const double pixelHeight = entity.textHeight * m_scale;
            if (pixelHeight >= minimumTextPixels
                && pixelHeight <= 4000.0) {
                textCandidates.append({&entity, pixelHeight});
            }
            continue;
        }
        if (entity.type == CadEntityType::Point && !entity.points.isEmpty()) {
            // Paper-space viewport copies of model points must stay inside
            // their own viewport window. Without this clip a point from a
            // neighbouring (or overlapping) viewport shows up in this one.
            if (entity.clipRect.isValid()
                && entity.clipRect.contains(entity.points.first())) {
                points.append(worldToScreen(entity.points.first()));
            }
            continue;
        }
        if (entity.hatchGroup >= 0) {
            // Belongs to a HATCH fill group: only contribute its polygon to
            // the odd-even fill path (paintHatches). Do NOT also stroke its
            // boundary here — that would draw every hatch edge twice (fill +
            // outline) and surface spurious outlines the DWG does not show.
            hatchGroups[entity.hatchGroup].append(index);
            continue;
        }
        if (entity.type == CadEntityType::Solid && entity.filled
            && entity.hatchGroup < 0 && entity.points.size() >= 3) {
            solids.append(index);
            continue;
        }
        const double lineWidth =
            lineWidthPixels(entity.lineWeight, m_lineWeightsVisible);
        const QColor strokeColor =
            m_printStyle ? printColor(entity)
                         : displayColor(entity.color);
        LineBatchKey key;
        key.color = strokeColor.rgba();
        key.width = qBound(0, int(lineWidth * 20.0), 255);
        if (entity.clipRect.isValid()) {
            key.left = doubleBits(entity.clipRect.left());
            key.top = doubleBits(entity.clipRect.top());
            key.right = doubleBits(entity.clipRect.right());
            key.bottom = doubleBits(entity.clipRect.bottom());
        }
        LineBatch &batch = batches[key];
        if (batch.lines.isEmpty()) {
            batch.color = strokeColor;
            batch.width = lineWidth;
            batch.clip = entity.clipRect;
        }
        if (entity.points.size() < 2) {
            continue;
        }
        screenPoints.clear();
        screenPoints.reserve(entity.points.size());
        for (const QPointF &point : entity.points) {
            if (std::isfinite(point.x()) && std::isfinite(point.y())) {
                screenPoints.append(worldToScreen(point));
            }
        }
        if (entity.type == CadEntityType::Segment
            || screenPoints.size() <= 2) {
            appendScreenPolylineSegments(
                batch.lines, screenPoints, entity.closed,
                minimumScreenEdge);
        } else {
            const QVector<QPointF> simplifiedPoints =
                simplifyScreenPolyline(screenPoints, lodTolerance);
            appendScreenPolylineSegments(
                batch.lines, simplifiedPoints, entity.closed,
                minimumScreenEdge);
        }
    }

    std::sort(
        textCandidates.begin(), textCandidates.end(),
        [](const TextCandidate &first, const TextCandidate &second) {
            return first.pixelHeight > second.pixelHeight;
        });
    if (textCandidates.size() > textLimit) {
        textCandidates.resize(textLimit);
    }
    for (const TextCandidate &candidate : std::as_const(textCandidates)) {
        const CadEntity &entity = *candidate.entity;
        ClipKey key;
        if (entity.clipRect.isValid()) {
            key.left = doubleBits(entity.clipRect.left());
            key.top = doubleBits(entity.clipRect.top());
            key.right = doubleBits(entity.clipRect.right());
            key.bottom = doubleBits(entity.clipRect.bottom());
        }
        TextBatch &batch = textBatches[key];
        if (batch.entities.isEmpty()) {
            batch.clip = entity.clipRect;
        }
        batch.entities.append(&entity);
    }
    const double buildMs = mark();

    qsizetype renderedHatchPatterns = 0;
    const bool hatchDebug =
        qEnvironmentVariableIsSet("CADLENS_HATCH_DEBUG");
    qsizetype hatchDebugGroups = 0;
    qsizetype hatchDebugBudgetSkipped = 0;
    qsizetype hatchDebugFlatFilled = 0;
    qsizetype hatchDebugPreviewFilled = 0;
    qsizetype hatchDebugPatternLines = 0;
    qsizetype hatchDebugSolidFilled = 0;
    // A single global pattern-line budget used to be shared by every hatch
    // group in QHash iteration order: a handful of groups consumed all
    // 80000 lines, and every group after that was replaced by an opaque
    // flat fill. Those rectangles are the "grey blocks" that show up only
    // at some zoom levels and on some (but not other) identical fills:
    // once baked into the interaction bitmap they stay there until the next
    // full redraw, which is why they look random. Give every group its own
    // fair quota and sample dense custom patterns evenly instead of
    // dropping the whole group, so every hatch keeps its texture at every
    // zoom, like CAD quick viewers do.
    // When a pattern is finer than one pixel its average tone is a mix of
    // line colour and canvas, never the full opaque layer colour (which
    // reads as a grey slab next to real pattern). Using the average tone
    // makes the pattern -> tone transition continuous while zooming.
    const QColor canvas = m_printStyle ? QColor(Qt::white)
                                       : canvasBackgroundColor();
    QVector<const QVector<int> *> orderedHatchGroups;
    orderedHatchGroups.reserve(hatchGroups.size());
    for (auto it = hatchGroups.cbegin(); it != hatchGroups.cend(); ++it) {
        if (!it.value().isEmpty()) {
            orderedHatchGroups.append(&it.value());
        }
    }
    std::sort(
        orderedHatchGroups.begin(), orderedHatchGroups.end(),
        [](const QVector<int> *first, const QVector<int> *second) {
            return first->first() < second->first();
        });
    const auto patternTone = [&](const QColor &ink) {
        if (m_printStyle) {
            return ink;
        }
        constexpr double coverage = 0.32;
        return QColor::fromRgbF(
            canvas.redF() * (1.0 - coverage) + ink.redF() * coverage,
            canvas.greenF() * (1.0 - coverage) + ink.greenF() * coverage,
            canvas.blueF() * (1.0 - coverage) + ink.blueF() * coverage);
    };
    const QSizeF patternViewport =
        m_renderSize.isValid() ? m_renderSize : QSizeF(size());
    const qsizetype globalPatternBudget = interactivePreview
        ? qBound<qsizetype>(
              qsizetype(30000),
              qsizetype(patternViewport.width()
                        * patternViewport.height() / 20),
              qsizetype(80000))
        : qBound<qsizetype>(
              qsizetype(60000),
              qsizetype(patternViewport.width()
                        * patternViewport.height() / 8),
              qsizetype(180000));
    qsizetype remainingPatternGroups = 0;
    for (const QVector<int> *group : std::as_const(orderedHatchGroups)) {
        for (int index : *group) {
            if (index < 0 || index >= m_document->entities.size()) {
                continue;
            }
            const std::shared_ptr<CadHatchPattern> &pattern =
                m_document->entities[index].hatchPattern;
            if (pattern && !pattern->solid
                && !pattern->lines.isEmpty()) {
                ++remainingPatternGroups;
                break;
            }
        }
    }
    qsizetype remainingPatternBudget = globalPatternBudget;
    const auto paintHatches = [&](QPainter &target) {
        QElapsedTimer hatchStage;
        for (const QVector<int> *groupPtr :
             std::as_const(orderedHatchGroups)) {
            const QVector<int> &group = *groupPtr;
            ++hatchDebugGroups;
            QPainterPath path =
                hatchPathForGroup(group.first(), group);
            QRectF clip;
            QColor color;
            std::shared_ptr<CadHatchPattern> pattern;
            bool fillShape = false;
            for (int index : group) {
                const CadEntity &entity = m_document->entities[index];
                if (entity.points.size() < 3) {
                    continue;
                }
                if (!clip.isValid()) {
                    clip = entity.clipRect;
                }
                if (!color.isValid()) {
                    color = entity.color;
                }
                if (!pattern) {
                    pattern = entity.hatchPattern;
                }
                fillShape = fillShape || entity.filled;
            }
            if (path.isEmpty() || !pattern) {
                continue;
            }
            const QRectF worldHatchBounds = path.boundingRect();
            const QTransform worldTransform = worldToScreenTransform();
            const QRectF hatchBounds =
                worldTransform.mapRect(worldHatchBounds);
            // A HATCH whose boundary is exactly one axis-aligned rectangle
            // can be clipped with setClipRect instead of setClipPath. Both
            // produce identical pixels, but QPainter re-tests a complex clip
            // path against every single pattern segment, which measured
            // 10-16x slower than a rectangular clip on these drawings.
            // Only accept the fast path when every boundary point lies on
            // the bounding box and all four corners are present, so arcs,
            // islands and L/triangle shapes keep the exact path clip.
            bool useRectClip = false;
            if (group.size() == 1 && worldHatchBounds.isValid()
                && m_document->entities[group.first()].points.size() >= 4
                && m_document->entities[group.first()].points.size() <= 5) {
                // Strict: exactly one closed 4-corner loop, every point is
                // one of the bounding box's four corners. A looser "all
                // points lie on an edge" test accepted shapes made of
                // several segments and let pattern lines spill into the
                // bounding box, visibly turning hatches into grey slabs.
                const double eps = qMax(
                    1.0e-9,
                    qMax(worldHatchBounds.width(),
                         worldHatchBounds.height()) * 1.0e-7);
                bool corners[4] = {false, false, false, false};
                bool allAtCorners = true;
                for (const QPointF &point :
                     m_document->entities[group.first()].points) {
                    const bool left =
                        qAbs(point.x() - worldHatchBounds.left()) <= eps;
                    const bool right =
                        qAbs(point.x() - worldHatchBounds.right()) <= eps;
                    const bool top =
                        qAbs(point.y() - worldHatchBounds.top()) <= eps;
                    const bool bottom =
                        qAbs(point.y() - worldHatchBounds.bottom()) <= eps;
                    if ((!left && !right) || (!top && !bottom)) {
                        allAtCorners = false;
                        break;
                    }
                    if (left && top) corners[0] = true;
                    if (right && top) corners[1] = true;
                    if (right && bottom) corners[2] = true;
                    if (left && bottom) corners[3] = true;
                }
                useRectClip = allAtCorners && corners[0] && corners[1]
                    && corners[2] && corners[3];
            }
            const QRectF groupWorldBounds = worldHatchBounds;
            // Only cull genuinely sub-pixel hatches when static. During
            // interaction we always draw them (cheap preview fill) so they
            // don't flicker/disappear while zooming, and at rest small
            // hatches stay visible instead of being dropped below a size
            // threshold (AutoCAD shows all hatches regardless of size).
            if (!m_printStyle && !interactivePreview
                && qMax(hatchBounds.width(), hatchBounds.height())
                    < 0.5) {
                continue;
            }
            // Fill rule per Autodesk HATCH style (DXF group 75):
            //   0 Normal / 1 Outer -> odd-even parity (islands cut holes)
            //   2 Ignore           -> winding (fill through, no holes)
            path.setFillRule(pattern->style == 2 ? Qt::WindingFill
                                                  : Qt::OddEvenFill);
            target.save();
            target.setWorldTransform(worldTransform);
            if (clip.isValid()) {
                target.setClipRect(clip, Qt::IntersectClip);
            }
            const QColor hatchColor =
                m_printStyle ? printColor(color) : displayColor(color);
            if (pattern->solid && fillShape) {
                target.fillPath(path, hatchColor);
                ++renderedHatchPatterns;
                ++hatchDebugSolidFilled;
                if (hatchDebug) {
                    std::fprintf(stderr,
                                 "[hg] solid n=%d layer='%s' handle=%llu bbox=[%g %g %g %g]\n",
                                 int(group.size()),
                                 m_document->entities[group.first()]
                                     .layer.toUtf8().constData(),
                                 (unsigned long long)
                                     m_document->entities[group.first()]
                                         .sourceHandle,
                                 groupWorldBounds.left(),
                                 groupWorldBounds.bottom(),
                                 groupWorldBounds.right(),
                                 groupWorldBounds.top());
                }
            } else if (!pattern->lines.isEmpty()) {
                double minimumPatternSpacingPx =
                    std::numeric_limits<double>::max();
                for (const CadHatchPatternLine &line : pattern->lines) {
                    const double spacing =
                        std::hypot(line.offset.x(), line.offset.y())
                        * m_scale;
                    if (spacing > 1.0e-9) {
                        minimumPatternSpacingPx =
                            qMin(minimumPatternSpacingPx, spacing);
                    }
                }
                const QVector<QLineF> noLines;
                const QVector<QLineF> *worldLines = &noLines;
                if (!hatchFlat && minimumPatternSpacingPx >= 1.15) {
                    const qsizetype fairShare =
                        remainingPatternGroups > 0
                        ? remainingPatternBudget
                            / remainingPatternGroups
                        : 0;
                    const qsizetype groupBudget = qMin(
                        remainingPatternBudget,
                        qMax<qsizetype>(256, fairShare));
                    hatchStage.restart();
                    if (groupBudget > 0) {
                        worldLines = &hatchPatternLinesForGroup(
                            group.first(), group, groupBudget,
                            quint64(groupBudget));
                    }
                    if (profile) {
                        hatchResolveMs +=
                            hatchStage.nsecsElapsed() / 1.0e6;
                        hatchDeflinesVisited += pattern->lines.size();
                    }
                }
                remainingPatternBudget =
                    qMax<qsizetype>(
                        0, remainingPatternBudget - worldLines->size());
                remainingPatternGroups =
                    qMax<qsizetype>(0, remainingPatternGroups - 1);
                if (worldLines->isEmpty() || hatchFlat) {
                    target.fillPath(path, patternTone(hatchColor));
                    ++renderedHatchPatterns;
                    ++hatchDebugFlatFilled;
                    if (hatchDebug) {
                        std::fprintf(stderr,
                                     "[hg] flat n=%d solid=%d layer='%s' "
                                     "handle=%llu bbox=[%g %g %g %g]\n",
                                     int(group.size()), pattern->solid ? 1 : 0,
                                     m_document->entities[group.first()]
                                         .layer.toUtf8().constData(),
                                     (unsigned long long)
                                         m_document->entities[group.first()]
                                             .sourceHandle,
                                     groupWorldBounds.left(),
                                     groupWorldBounds.bottom(),
                                     groupWorldBounds.right(),
                                     groupWorldBounds.top());
                    }
                } else {
                    const bool hasDotDashes = std::any_of(
                        pattern->lines.cbegin(), pattern->lines.cend(),
                        [](const CadHatchPatternLine &line) {
                            return std::any_of(
                                line.dashes.cbegin(), line.dashes.cend(),
                                [](double dash) {
                                    return qAbs(dash) < 1.0e-9;
                                });
                        });
                    QVector<QPointF> dots;
                    if (hasDotDashes) {
                        dots.reserve(worldLines->size() / 16);
                        for (const QLineF &line : *worldLines) {
                            if (line.length() <= 1.0e-12) {
                                dots.append(line.p1());
                            }
                        }
                    }
                    if (hatchNoClip
                        || (useRectClip && !hatchForcePathClip)) {
                        target.setClipRect(worldHatchBounds,
                                           Qt::IntersectClip);
                    } else {
                        target.setClipPath(path, Qt::IntersectClip);
                    }
                    hatchStage.restart();
                    if (profile) {
                        hatchGenerateMs +=
                            hatchStage.nsecsElapsed() / 1.0e6;
                        hatchDeflinesGenerated += pattern->lines.size();
                        hatchGeneratedLines += worldLines->size();
                    }
                    QPen patternPen(hatchColor, 0.62);
                    patternPen.setCosmetic(true);
                    patternPen.setCapStyle(Qt::RoundCap);
                    target.setPen(patternPen);
                    hatchStage.restart();
                    target.drawLines(*worldLines);
                    if (!dots.isEmpty()) {
                        target.drawPoints(dots);
                    }
                    if (profile) {
                        hatchDrawMs +=
                            hatchStage.nsecsElapsed() / 1.0e6;
                        if (useRectClip) {
                            ++hatchRectClipGroups;
                            hatchRectClipLines += worldLines->size();
                        }
                    }
                    renderedHatchPatterns += worldLines->size();
                    ++hatchDebugPatternLines;
                    if (hatchDebug && worldLines->size() > 2000) {
                        std::fprintf(stderr,
                                     "[hg] lines n=%d count=%lld layer='%s' handle=%llu bbox=[%g %g %g %g]\n",
                                     int(group.size()),
                                     (long long)worldLines->size(),
                                     m_document->entities[group.first()]
                                         .layer.toUtf8().constData(),
                                     (unsigned long long)
                                         m_document->entities[group.first()]
                                             .sourceHandle,
                                     groupWorldBounds.left(),
                                     groupWorldBounds.bottom(),
                                     groupWorldBounds.right(),
                                     groupWorldBounds.top());
                    }
                }
            } else if (pattern->gradient) {
                const QColor gradientColor =
                    m_printStyle
                    ? QColor(Qt::black)
                    : QColor(hatchColor.red(), hatchColor.green(),
                             hatchColor.blue(), 55);
                target.fillPath(path, gradientColor);
                ++renderedHatchPatterns;
            }
            target.restore();
        }
        if (hatchDebug) {
            std::fprintf(stderr,
                         "[hatch] groups=%lld budgetSkipped=%lld solid=%lld "
                         "flat=%lld preview=%lld lines=%lld used=%lld\n",
                         static_cast<long long>(hatchDebugGroups),
                         static_cast<long long>(hatchDebugBudgetSkipped),
                         static_cast<long long>(hatchDebugSolidFilled),
                         static_cast<long long>(hatchDebugFlatFilled),
                         static_cast<long long>(hatchDebugPreviewFilled),
                         static_cast<long long>(hatchDebugPatternLines),
                         static_cast<long long>(renderedHatchPatterns));
        }
    };
    // Draw hatches in both static and interactive modes. The lambda already
    // uses a cheap semi-transparent fill for pattern hatches while panning or
    // zooming, so keeping it active avoids hatches blinking out during
    // interaction (the "only visible after a certain zoom, then flickering"
    // defect).
    paintHatches(painter);
    const double hatchMs = mark();
    QElapsedTimer detailStage;
    detailStage.start();
    qint64 renderedSegments = 0;
    for (const LineBatch &batch : std::as_const(batches)) {
        renderedSegments += batch.lines.size();
        QPen pen(batch.color, batch.width, Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        pen.setCosmetic(!m_printStyle);
        painter.save();
        if (batch.clip.isValid()) {
            const QPointF a = worldToScreen(batch.clip.topLeft());
            const QPointF b = worldToScreen(batch.clip.bottomRight());
            painter.setClipRect(QRectF(a, b).normalized(),
                    Qt::IntersectClip);
        }
        painter.setPen(pen);
        painter.drawLines(batch.lines);
        painter.restore();
    }
    const double linesMs = mark();
    detailStage.restart();
    for (int index : solids) {
        const CadEntity &entity = m_document->entities[index];
        QVector<QPointF> screenPoints;
        screenPoints.reserve(entity.points.size());
        for (const QPointF &point : entity.points) {
            screenPoints.append(worldToScreen(point));
        }
        // CAD SOLID fills are opaque; these drawings carry no per-entity
        // transparency (DXF group 440 is ByLayer/ByBlock throughout). Keep
        // screen and print identical instead of washing the fill out.
        const QColor fill =
            m_printStyle ? printColor(entity)
                         : displayColor(entity.color);
        const QColor stroke =
            m_printStyle ? printColor(entity)
                         : displayColor(entity.color);
        painter.save();
        if (entity.clipRect.isValid()) {
            const QPointF a = worldToScreen(entity.clipRect.topLeft());
            const QPointF b = worldToScreen(entity.clipRect.bottomRight());
            painter.setClipRect(QRectF(a, b).normalized(),
                    Qt::IntersectClip);
        }
        painter.setPen(QPen(stroke,
                            lineWidthPixels(entity.lineWeight,
                                            m_lineWeightsVisible)));
        painter.setBrush(entity.filled ? QBrush(fill) : QBrush(Qt::NoBrush));
        painter.drawPath(
            polygonPath(painter, screenPoints, entity.closed));
        painter.restore();
    }
    const double solidsMs =
        detailStage.nsecsElapsed() / 1.0e6;
    detailStage.restart();
    painter.setPen(Qt::NoPen);
    painter.setBrush(
        m_printStyle
            ? QColor(Qt::black)
            : (m_drawingTheme == DrawingTheme::CadLight
                   ? QColor(QStringLiteral("#111820"))
                   : QColor(QStringLiteral("#f1f3f4"))));
    for (const QPointF &point : std::as_const(points)) {
        painter.drawEllipse(point, 1.4, 1.4);
    }
    const double pointsMs =
        detailStage.nsecsElapsed() / 1.0e6;
    detailStage.restart();
    painter.setBrush(Qt::NoBrush);
    qsizetype renderedTexts = 0;
    for (const TextBatch &batch : std::as_const(textBatches)) {
        painter.save();
        if (batch.clip.isValid()) {
            const QPointF a = worldToScreen(batch.clip.topLeft());
            const QPointF b = worldToScreen(batch.clip.bottomRight());
            painter.setClipRect(QRectF(a, b).normalized(),
                    Qt::IntersectClip);
        }
        for (const CadEntity *entity : batch.entities) {
            drawCadText(painter, *entity);
        }
        renderedTexts += batch.entities.size();
        painter.restore();
    }
    const double textMs =
        detailStage.nsecsElapsed() / 1.0e6;
    const double restMs = mark();
    if (profile) {
        std::fprintf(
            stderr,
            "[profile] total=%.1fms | query=%.1f build=%.1f hatch=%.1f "
            "(resolve=%.1f generate=%.1f draw=%.1f path+fill=%.1f) "
            "lines=%.1f solids=%.1f points=%.1f text=%.1f rest=%.1f | "
            "visible=%lld batches=%lld "
            "segments=%lld deflines-visited=%llu deflines-generated=%llu "
            "generated-lines=%llu rect-clip-groups=%llu "
            "rect-clip-lines=%llu\n",
            renderTimer.nsecsElapsed() / 1.0e6,
            queryMs, buildMs, hatchMs, hatchResolveMs,
            hatchGenerateMs, hatchDrawMs,
            qMax(0.0, hatchMs - hatchResolveMs - hatchGenerateMs
                          - hatchDrawMs),
            linesMs, solidsMs, pointsMs, textMs, restMs,
            static_cast<long long>(visible.size()),
            static_cast<long long>(batches.size()),
            static_cast<long long>(renderedSegments),
            (unsigned long long)hatchDeflinesVisited,
            (unsigned long long)hatchDeflinesGenerated,
            (unsigned long long)hatchGeneratedLines,
            (unsigned long long)hatchRectClipGroups,
            (unsigned long long)hatchRectClipLines);
    }
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(stderr, "geometry visible=%lld batches=%lld "
                             "screen-segments=%lld hatch-patterns=%lld "
                             "solids=%lld texts=%lld "
                             "text-batches=%lld elapsed=%.2fms\n",
                     static_cast<long long>(visible.size()),
                     static_cast<long long>(batches.size()),
                     static_cast<long long>(renderedSegments),
                     static_cast<long long>(renderedHatchPatterns),
                     static_cast<long long>(solids.size()),
                     static_cast<long long>(renderedTexts),
                     static_cast<long long>(textBatches.size()),
                     renderTimer.nsecsElapsed() / 1.0e6);
    }
}

void CadView::drawCadText(QPainter &painter, const CadEntity &entity)
{
    if (entity.text.isEmpty()) {
        return;
    }
    const double pixelHeight = entity.textHeight * m_scale;
    if (pixelHeight < kMinTextPixelHeight || pixelHeight > 4000.0) {
        return;
    }
    const QPointF anchor = worldToScreen(entity.textPosition);
    const QSizeF viewport =
        m_renderSize.isValid() ? m_renderSize : QSizeF(size());
    if (anchor.x() < -viewport.width()
        || anchor.x() > viewport.width() * 2.0
        || anchor.y() < -viewport.height()
        || anchor.y() > viewport.height() * 2.0) {
        return;
    }
    // Screen caches are raster surfaces. Rasterising glyphs directly avoids
    // rebuilding complex CJK outlines for every unique label; vector paths
    // remain mandatory for print, SVG and exact image exports.
    if (!m_printStyle && !m_exportingScene) {
        painter.save();
        painter.translate(anchor);
        painter.rotate(-entity.textRotation);
        if (entity.textMirrored) {
            painter.scale(1.0, -1.0);
        }
        QFont font = cadFontForText(
            entity.text, qMax(1, qRound(pixelHeight)));
        font.setStretch(qBound(
            40, int(entity.textWidthScale * 100.0), 400));
        const QFontMetricsF metrics(font);
        const QStringList lines =
            entity.text.split(QLatin1Char('\n'));
        double textWidth = 0.0;
        for (const QString &line : lines) {
            textWidth = qMax(
                textWidth, metrics.horizontalAdvance(line));
        }
        const double lineSpacing = metrics.lineSpacing();
        const int lineCount = qMax(1, lines.size());
        const QRectF textBounds(
            0.0, -metrics.ascent(), textWidth,
            lineSpacing * qMax(0, lineCount - 1)
                + metrics.ascent() + metrics.descent());
        double shiftX = 0.0;
        double shiftY = 0.0;
        if (entity.textAttachment >= 1 && entity.textAttachment <= 9) {
            const int column = (entity.textAttachment - 1) % 3;
            const int row = (entity.textAttachment - 1) / 3;
            shiftX = column == 0 ? -textBounds.left()
                     : column == 1 ? -textBounds.center().x()
                                   : -textBounds.right();
            shiftY = row == 0 ? -textBounds.top()
                     : row == 1 ? -textBounds.center().y()
                                : -textBounds.bottom();
        } else {
            shiftX = entity.textHAlign == 1 || entity.textHAlign == 4
                ? -textBounds.center().x()
                : entity.textHAlign == 2 ? -textBounds.right()
                                         : -textBounds.left();
            shiftY = entity.textHAlign == 4 || entity.textVAlign == 2
                ? -textBounds.center().y()
                : entity.textVAlign == 3 ? -textBounds.top()
                : entity.textVAlign == 1 ? -textBounds.bottom()
                                         : 0.0;
        }
        if (entity.textFit && textWidth > 1.0e-9) {
            const double targetWidth =
                QLineF(worldToScreen(entity.textPosition),
                       worldToScreen(entity.textAlignmentPoint))
                    .length();
            if (targetWidth > 0.01) {
                painter.scale(targetWidth / textWidth, 1.0);
            }
        }
        painter.setFont(font);
        painter.setPen(
            m_printStyle ? printColor(entity)
                         : displayColor(entity.color));
        painter.setBrush(Qt::NoBrush);
        for (int index = 0; index < lines.size(); ++index) {
            painter.drawText(
                QPointF(shiftX, shiftY + lineSpacing * index),
                lines[index]);
        }
        painter.restore();
        return;
    }
    painter.save();
    painter.translate(anchor);
    painter.rotate(-entity.textRotation);
    if (entity.textMirrored) {
        painter.scale(1.0, -1.0);
    }
    constexpr int basePixelHeight = 64;
    const int stretch =
        qBound(40, int(entity.textWidthScale * 100.0), 400);
    const QString cacheKey =
        QString::number(stretch) + QLatin1Char('|') + entity.text;
    auto cachedPath = m_cadTextPathCache.constFind(cacheKey);
    if (cachedPath == m_cadTextPathCache.cend()) {
        if (m_cadTextPathCache.size() >= 4096) {
            m_cadTextPathCache.clear();
        }
        QFont font = cadFontForText(entity.text, basePixelHeight);
        font.setStretch(stretch);
        QPainterPath basePath;
        const QStringList lines =
            entity.text.split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            basePath.addText(
                QPointF(0.0, basePixelHeight * i * 1.2),
                font, lines[i]);
        }
        cachedPath =
            m_cadTextPathCache.insert(cacheKey, basePath);
    }
    QPainterPath path = cachedPath.value();
    const double glyphScale = pixelHeight / basePixelHeight;
    if (qAbs(glyphScale - 1.0) > 1.0e-9) {
        QTransform scaleTransform;
        scaleTransform.scale(glyphScale, glyphScale);
        path = scaleTransform.map(path);
    }
    if (entity.textFit && path.boundingRect().width() > 1.0e-9) {
        const double targetWidth =
            QLineF(worldToScreen(entity.textPosition),
                   worldToScreen(entity.textAlignmentPoint))
                .length();
        if (targetWidth > 0.01) {
            const double scaleX =
                targetWidth / path.boundingRect().width();
            QTransform fit;
            fit.scale(scaleX, 1.0);
            path = fit.map(path);
        }
    }
    const QRectF textBounds = path.boundingRect();
    double shiftX = 0.0;
    double shiftY = 0.0;
    if (entity.textAttachment >= 1 && entity.textAttachment <= 9) {
        const int column = (entity.textAttachment - 1) % 3;
        const int row = (entity.textAttachment - 1) / 3;
        shiftX = column == 0 ? -textBounds.left()
                 : column == 1 ? -textBounds.center().x()
                               : -textBounds.right();
        shiftY = row == 0 ? -textBounds.top()
                 : row == 1 ? -textBounds.center().y()
                            : -textBounds.bottom();
    } else {
        shiftX = entity.textHAlign == 1 || entity.textHAlign == 4
            ? -textBounds.center().x()
            : entity.textHAlign == 2 ? -textBounds.right()
                                     : -textBounds.left();
        shiftY = entity.textHAlign == 4 || entity.textVAlign == 2
            ? -textBounds.center().y()
            : entity.textVAlign == 3 ? -textBounds.top()
            : entity.textVAlign == 1 ? -textBounds.bottom()
                                     : 0.0;
    }
    path.translate(shiftX, shiftY);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_printStyle ? printColor(entity)
                                  : displayColor(entity.color));
    painter.drawPath(path);
    painter.restore();
}

void CadView::drawSnapMarker(QPainter &painter, const QPointF &point,
                             SnapKind kind)
{
    painter.save();
    const QColor color("#5ff0c1");
    painter.setPen(QPen(color, 1.4));
    QColor markerFill = canvasBackgroundColor();
    markerFill.setAlpha(m_drawingTheme == DrawingTheme::CadLight
                            ? 230 : 225);
    painter.setBrush(markerFill);
    const double radius = 5.0;
    switch (kind) {
    case SnapKind::Endpoint:
        painter.drawRect(QRectF(point.x() - radius, point.y() - radius,
                                radius * 2.0, radius * 2.0));
        break;
    case SnapKind::Midpoint: {
        QPainterPath triangle;
        triangle.moveTo(point + QPointF(0.0, -radius));
        triangle.lineTo(point + QPointF(radius, radius));
        triangle.lineTo(point + QPointF(-radius, radius));
        triangle.closeSubpath();
        painter.drawPath(triangle);
        break;
    }
    case SnapKind::Intersection:
        painter.drawLine(point + QPointF(-radius, -radius),
                         point + QPointF(radius, radius));
        painter.drawLine(point + QPointF(-radius, radius),
                         point + QPointF(radius, -radius));
        break;
    case SnapKind::Perpendicular:
        painter.drawLine(point + QPointF(-radius, -radius),
                         point + QPointF(radius, -radius));
        painter.drawLine(point + QPointF(radius, -radius),
                         point + QPointF(radius, radius));
        break;
    case SnapKind::Center:
        painter.drawEllipse(point, radius, radius);
        painter.drawLine(point + QPointF(-radius, 0),
                         point + QPointF(radius, 0));
        painter.drawLine(point + QPointF(0, -radius),
                         point + QPointF(0, radius));
        break;
    case SnapKind::Quadrant: {
        QPainterPath diamond;
        diamond.moveTo(point + QPointF(0, -radius));
        diamond.lineTo(point + QPointF(radius, 0));
        diamond.lineTo(point + QPointF(0, radius));
        diamond.lineTo(point + QPointF(-radius, 0));
        diamond.closeSubpath();
        painter.drawPath(diamond);
        break;
    }
    case SnapKind::Nearest:
        painter.drawEllipse(point, 2.5, 2.5);
        break;
    case SnapKind::None:
        break;
    }
    painter.restore();
}

void CadView::drawArrow(QPainter &painter, const QPointF &tip,
                        const QPointF &toward, const QColor &color)
{
    QLineF line(tip, toward);
    if (line.length() < 0.001) {
        return;
    }
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    const double markerSize = qMax(
        0.65, metrics.arrowSize * geometryScale);
    line.setLength(markerSize);
    const double angle = std::atan2(line.dy(), line.dx());
    const QPointF direction = tip - toward;
    const double directionLength =
        std::hypot(direction.x(), direction.y());
    if (directionLength <= 1.0e-9) {
        return;
    }
    const QPointF unit = direction / directionLength;
    if (metrics.marker == AnnotationMarker::ArchitecturalTick) {
        // GB/T 50001：尺寸起止符号为中粗斜短线，长约 2~3mm，
        // 倾斜方向与尺寸界线成 45°。
        const QPointF tick =
            (unit + QPointF(-unit.y(), unit.x()))
            * (markerSize * 0.55);
        painter.save();
        painter.setPen(
            QPen(color,
                 annotationStroke(qMax(1.15, metrics.lineWidth),
                                  geometryScale),
                 Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(tip - tick, tip + tick);
        painter.restore();
        return;
    }
    if (metrics.marker == AnnotationMarker::Dot) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const double radius = qMax(0.8, markerSize * 0.34);
        painter.drawEllipse(tip, radius, radius);
        painter.restore();
        return;
    }
    if (metrics.marker == AnnotationMarker::EndBar) {
        const QPointF normal(-unit.y(), unit.x());
        painter.save();
        painter.setPen(
            QPen(color,
                 annotationStroke(metrics.lineWidth, geometryScale),
                 Qt::SolidLine, Qt::SquareCap));
        painter.drawLine(tip - normal * (markerSize * 0.62),
                         tip + normal * (markerSize * 0.62));
        painter.restore();
        return;
    }
    const bool open =
        metrics.marker == AnnotationMarker::OpenArrow;
    const double spread = 0.3398;
    QPainterPath arrow;
    arrow.moveTo(tip);
    arrow.lineTo(tip + QPointF(std::cos(angle - spread),
                              std::sin(angle - spread))
                            * markerSize);
    arrow.lineTo(tip + QPointF(std::cos(angle + spread),
                              std::sin(angle + spread))
                            * markerSize);
    arrow.closeSubpath();
    painter.save();
    if (open) {
        painter.setPen(
            QPen(color,
                 annotationStroke(metrics.lineWidth, geometryScale),
                 Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
    }
    painter.drawPath(arrow);
    painter.restore();
}

void CadView::drawVectorLabel(QPainter &painter, const QString &text,
                              const QPointF &anchor, const QColor &color,
                              QRectF *hitRect, bool emphasized)
{
    if (text.isEmpty()) {
        if (hitRect) {
            *hitRect = QRectF();
        }
        return;
    }
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    const int fontPixels = qMax(
        1, qRound(metrics.textSize * geometryScale
                  + (emphasized ? 1.0 : 0.0)));
    QFont font = cadFontForText(
        text, fontPixels,
        emphasized ? QFont::DemiBold : QFont::Medium);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const double lineHeight =
        (metrics.textSize + 3.0) * geometryScale
        + (emphasized ? 1.0 : 0.0);
    QPainterPath textPath;
    for (int i = 0; i < lines.size(); ++i) {
        textPath.addText(
            QPointF(0.0, lineHeight * i), font, lines[i]);
    }
    const QRectF textBounds = textPath.boundingRect();
    textPath.translate(-textBounds.center().x(),
                       -textBounds.center().y());
    const QRectF screenBounds =
        textPath.boundingRect().translated(anchor);
    painter.save();
    const QColor ink = m_printStyle ? printColor(color) : color;
    painter.translate(anchor);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPath(textPath);
    painter.restore();
    if (hitRect) {
        *hitRect = screenBounds.adjusted(-3.0, -3.0, 3.0, 3.0);
    }
}

void CadView::drawNotePlate(QPainter &painter, const QString &text,
                            const QPointF &anchor, const QColor &color,
                            QRectF *hitRect, bool emphasized)
{
    if (text.isEmpty()) {
        if (hitRect) {
            *hitRect = QRectF();
        }
        return;
    }
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    const int fontPixels = qMax(
        1, qRound(metrics.textSize * geometryScale
                  + (emphasized ? 1.0 : 0.0)));
    QFont font = cadFontForText(
        text, fontPixels,
        emphasized ? QFont::DemiBold : QFont::Medium);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const double lineHeight =
        (metrics.textSize + 3.0) * geometryScale
        + (emphasized ? 1.0 : 0.0);
    QPainterPath textPath;
    for (int i = 0; i < lines.size(); ++i) {
        textPath.addText(
            QPointF(0.0, lineHeight * i), font, lines[i]);
    }
    const QRectF textBounds = textPath.boundingRect();
    textPath.translate(-textBounds.center().x(),
                       -textBounds.center().y());
    const double padX = qMax(3.5, 5.5 * geometryScale);
    const double padY = qMax(2.5, 4.0 * geometryScale);
    const QRectF plate =
        textPath.boundingRect().adjusted(-padX, -padY, padX, padY);
    const QColor ink = m_printStyle ? printColor(color) : color;
    QColor plateColor = m_printStyle
        ? QColor(255, 255, 255) : canvasBackgroundColor();
    plateColor.setAlpha(m_printStyle ? 240 : 226);

    painter.save();
    painter.translate(anchor);
    painter.setBrush(plateColor);
    painter.setPen(QPen(ink, annotationStroke(1.0, geometryScale),
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const double radius = qMin(3.0, plate.height() * 0.25);
    painter.drawRoundedRect(plate, radius, radius);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPath(textPath);
    painter.restore();

    if (hitRect) {
        *hitRect = plate.translated(anchor).adjusted(-2.0, -2.0, 2.0, 2.0);
    }
}

void CadView::drawDimensionText(QPainter &painter, const QString &text,
                                const QPointF &anchor,
                                const QColor &color,
                                double angleDegrees, QRectF *hitRect,
                                bool emphasized)
{
    if (text.isEmpty()) {
        if (hitRect) {
            *hitRect = QRectF();
        }
        return;
    }
    while (angleDegrees > 90.0) {
        angleDegrees -= 180.0;
    }
    while (angleDegrees < -90.0) {
        angleDegrees += 180.0;
    }
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    const int fontPixels = qMax(
        1, qRound(metrics.textSize * geometryScale
                  + (emphasized ? 1.0 : 0.0)));
    QFont font = cadFontForText(
        text, fontPixels,
        emphasized ? QFont::DemiBold : QFont::Medium);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const double lineHeight =
        (metrics.textSize + 3.0) * geometryScale
        + (emphasized ? 1.0 : 0.0);
    QPainterPath textPath;
    for (int i = 0; i < lines.size(); ++i) {
        textPath.addText(
            QPointF(0.0, lineHeight * i), font, lines[i]);
    }
    const QRectF textBounds = textPath.boundingRect();
    textPath.translate(-textBounds.center().x(),
                       -textBounds.center().y());
    const QPointF drawAnchor = anchor;
    painter.save();
    painter.translate(drawAnchor);
    painter.rotate(angleDegrees);
    painter.setPen(Qt::NoPen);
    const QColor ink = m_printStyle ? printColor(color) : color;
    painter.setBrush(ink);
    painter.drawPath(textPath);
    if (hitRect) {
        QTransform transform;
        transform.translate(drawAnchor.x(), drawAnchor.y());
        transform.rotate(angleDegrees);
        *hitRect = transform.mapRect(
            textPath.boundingRect().adjusted(-2.0, -2.0, 2.0, 2.0));
    }
    painter.restore();
}

void CadView::drawInlineDimension(
    QPainter &painter, const QString &text,
    const QPointF &worldStart, const QPointF &worldEnd,
    const QPointF &leaderHint, const QColor &color,
    QRectF *hitRect, bool bothTerminals)
{
    if (text.isEmpty()) {
        return;
    }
    const QPointF start = worldToScreen(worldStart);
    const QPointF end = worldToScreen(worldEnd);
    const QPointF midpoint = (start + end) * 0.5;
    const QPointF lineVector = end - start;
    const double lineLength =
        std::hypot(lineVector.x(), lineVector.y());
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    const int fontPixels = qMax(
        1, qRound(metrics.textSize * geometryScale));
    QFont font = cadFontForText(text, fontPixels, QFont::Medium);
    const QFontMetricsF fontMetrics(font);
    double textWidth = 1.0;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        textWidth = qMax(
            textWidth, fontMetrics.horizontalAdvance(line));
    }
    const int lineCount = text.count(QLatin1Char('\n')) + 1;
    const double textHeight =
        qMax(1.0, fontMetrics.height() * lineCount);
    const double gap = qMax(3.0, geometryScale * 3.0);
    const double angle = lineLength > 1.0e-9
        ? std::atan2(lineVector.y(), lineVector.x()) * 180.0 / kPi
        : 0.0;
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(
            stderr,
            "inline-dimension text=%s start=(%.2f,%.2f) "
            "end=(%.2f,%.2f) length=%.2f width=%.2f\n",
            text.toUtf8().constData(), start.x(), start.y(),
            end.x(), end.y(), lineLength, textWidth);
    }

    if (lineLength >= textWidth + gap * 2.0) {
        const QPointF unit = lineVector / lineLength;
        const double halfText = textWidth * 0.5;
        const QPointF before =
            midpoint - unit * (halfText + gap);
        const QPointF after =
            midpoint + unit * (halfText + gap);
        if (QLineF(start, before).length() > 0.25) {
            painter.drawLine(start, before);
        }
        if (QLineF(after, end).length() > 0.25) {
            painter.drawLine(after, end);
        }
        if (bothTerminals) {
            drawArrow(painter, start, midpoint, color);
        }
        drawArrow(painter, end, midpoint, color);
        drawDimensionText(
            painter, text, midpoint, color, angle, hitRect, false);
        return;
    }

    if (lineLength > 1.0e-9) {
        painter.drawLine(start, end);
        if (bothTerminals) {
            drawArrow(painter, start, midpoint, color);
        }
        drawArrow(painter, end, midpoint, color);
    }
    QPointF normal =
        lineLength > 1.0e-9
        ? QPointF(-lineVector.y() / lineLength,
                  lineVector.x() / lineLength)
        : QPointF(0.0, -1.0);
    const QPointF hint = worldToScreen(leaderHint);
    if (QPointF::dotProduct(hint - midpoint, normal) < 0.0) {
        normal = -normal;
    }
    const QPointF labelAnchor =
        midpoint + normal * (textHeight + gap * 2.0 + 8.0);
    const QPointF textEdge =
        labelAnchor - normal * (textHeight * 0.58);
    painter.setPen(QPen(color, annotationStroke(0.8, geometryScale),
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(midpoint, textEdge);
    drawDimensionText(
        painter, text, labelAnchor, color, 0.0, hitRect, false);
}

QPainterPath CadView::measurementOutline(
    const Measurement &measurement,
    const QVector<QPointF> &screenPoints) const
{
    QPainterPath path;
    if (screenPoints.size() < 2) {
        return path;
    }
    const bool hasEdges = !measurement.edgeStarts.isEmpty()
        && measurement.edgeStarts.size() == measurement.edgeEnds.size()
        && measurement.edgeStarts.size()
            == measurement.edgeMidpoints.size();
    if (!measurement.segments.isEmpty()) {
        for (const MeasurementSegment &segment : measurement.segments) {
            const QPointF start = worldToScreen(segment.start);
            const QPointF end = worldToScreen(segment.end);
            if (path.isEmpty()) {
                path.moveTo(start);
            } else {
                path.lineTo(start);
            }
            if (segment.kind == MeasurementSegmentKind::Arc) {
                const QPainterPath arcPath = arcPathThroughThreePoints(
                    start, worldToScreen(segment.midpoint), end);
                if (!arcPath.isEmpty()) {
                    path.addPath(arcPath);
                    path.lineTo(end);
                    continue;
                }
            }
            path.lineTo(end);
        }
        path.closeSubpath();
        return path;
    }
    if (hasEdges) {
        for (int i = 0; i < measurement.edgeStarts.size(); ++i) {
            const QPointF start = worldToScreen(measurement.edgeStarts[i]);
            const QPointF end = worldToScreen(measurement.edgeEnds[i]);
            if (path.isEmpty()) {
                path.moveTo(start);
            } else {
                path.lineTo(start);
            }
            const bool arc = i < measurement.edgeArcLengths.size()
                && measurement.edgeArcLengths[i] > 0.0;
            if (arc) {
                const QPainterPath arcPath = arcPathThroughThreePoints(
                    start, worldToScreen(measurement.edgeMidpoints[i]), end);
                if (!arcPath.isEmpty()) {
                    path.addPath(arcPath);
                    path.lineTo(end);
                    continue;
                }
            }
            path.lineTo(end);
        }
        path.closeSubpath();
        return path;
    }
    path.addPolygon(QPolygonF(screenPoints));
    path.closeSubpath();
    return path;
}

void CadView::drawContinuousDimension(
    QPainter &painter, const Measurement &measurement,
    const QVector<QPointF> &screenPoints, const QColor &color,
    bool selected)
{
    if (!m_document || screenPoints.size() < 2) {
        return;
    }
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    struct Segment {
        QPointF start;
        QPointF end;
        QPointF mid;
        QPointF center;
        bool arc = false;
        double value = 0.0;
    };
    QVector<Segment> segments;
    if (!measurement.segments.isEmpty()) {
        segments.reserve(measurement.segments.size());
        for (const MeasurementSegment &source : measurement.segments) {
            Segment segment;
            segment.start = worldToScreen(source.start);
            segment.end = worldToScreen(source.end);
            segment.mid = worldToScreen(source.midpoint);
            segment.center = worldToScreen(source.center);
            segment.arc =
                source.kind == MeasurementSegmentKind::Arc;
            segment.value = source.length > 0.0
                ? source.length
                : QLineF(source.start, source.end).length();
            segments.append(segment);
        }
    } else if (!measurement.edgeStarts.isEmpty()
               && measurement.edgeStarts.size()
                   == measurement.edgeEnds.size()) {
        segments.reserve(measurement.edgeStarts.size());
        for (int i = 0; i < measurement.edgeStarts.size(); ++i) {
            Segment segment;
            segment.start = worldToScreen(measurement.edgeStarts[i]);
            segment.end = worldToScreen(measurement.edgeEnds[i]);
            segment.mid = i < measurement.edgeMidpoints.size()
                ? worldToScreen(measurement.edgeMidpoints[i])
                : (segment.start + segment.end) * 0.5;
            segment.arc = i < measurement.edgeArcLengths.size()
                && measurement.edgeArcLengths[i] > 0.0;
            segment.value = i < measurement.edgeLengths.size()
                ? measurement.edgeLengths[i]
                : QLineF(measurement.edgeStarts[i],
                         measurement.edgeEnds[i]).length();
            segments.append(segment);
        }
    } else {
        segments.reserve(screenPoints.size() - 1);
        for (int i = 1; i < screenPoints.size(); ++i) {
            Segment segment;
            segment.start = screenPoints[i - 1];
            segment.end = screenPoints[i];
            segment.mid = (segment.start + segment.end) * 0.5;
            segment.value = QLineF(measurement.points[i - 1],
                                   measurement.points[i]).length();
            segments.append(segment);
        }
    }
    if (segments.isEmpty()) {
        return;
    }

    const QFont valueFont = cadFontForText(
        QStringLiteral("0123456789"),
        qMax(1, qRound(metrics.textSize * geometryScale)),
        QFont::Medium);
    const QFontMetricsF valueMetrics(valueFont);
    const double dotRadius =
        qBound(2.2, 2.4 * geometryScale, 5.0);
    QPen dimensionPen(
        color,
        annotationStroke(selected ? metrics.lineWidth + 0.55
                                  : metrics.lineWidth,
                         geometryScale),
        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    dimensionPen.setCosmetic(!m_printStyle);
    const auto textTangent =
        [](const Segment &segment) {
            QPointF tangent = segment.end - segment.start;
            if (segment.arc
                && QLineF(segment.center, segment.mid).length() > 1.0e-6) {
                const QPointF radial = segment.mid - segment.center;
                tangent = QPointF(-radial.y(), radial.x());
                if (QPointF::dotProduct(
                        tangent, segment.end - segment.start) < 0.0) {
                    tangent = -tangent;
                }
            }
            return std::atan2(tangent.y(), tangent.x())
                * 180.0 / kPi;
        };

    painter.save();
    painter.setBrush(Qt::NoBrush);
    for (int i = 1; i < screenPoints.size(); ++i) {
        m_measurementHitSegments[measurement.id].append(
            QLineF(screenPoints[i - 1], screenPoints[i]));
    }
    QVector<QPointF> endpoints;
    endpoints.reserve(segments.size() + 1);
    for (const Segment &segment : std::as_const(segments)) {
        const QString value = formatDimensionValue(
            segment.value, *m_document, m_settings.lengthUnit,
            m_settings.lengthPrecision);
        const double angle = textTangent(segment);
        const double textWidth =
            valueMetrics.horizontalAdvance(value);
        const QPointF chord = segment.end - segment.start;
        const double chordLength =
            std::hypot(chord.x(), chord.y());
        painter.setPen(dimensionPen);
        if (segment.arc) {
            const QPainterPath arc = arcPathThroughThreePoints(
                segment.start, segment.mid, segment.end);
            if (!arc.isEmpty()) {
                painter.drawPath(arc);
            } else {
                painter.drawLine(segment.start, segment.end);
            }
        } else if (chordLength > textWidth + 10.0) {
            const QPointF unit = chord / chordLength;
            const QPointF before =
                segment.mid - unit * (textWidth * 0.5 + 4.0);
            const QPointF after =
                segment.mid + unit * (textWidth * 0.5 + 4.0);
            painter.drawLine(segment.start, before);
            painter.drawLine(after, segment.end);
        } else {
            painter.drawLine(segment.start, segment.end);
        }

        drawDimensionText(
            painter, value, segment.mid, color, angle,
            nullptr, selected);

        for (const QPointF &endpoint :
             {segment.start, segment.end}) {
            bool alreadyAdded = false;
            for (const QPointF &existing : std::as_const(endpoints)) {
                if (QLineF(endpoint, existing).length() < 0.5) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded) {
                endpoints.append(endpoint);
            }
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const QPointF &endpoint : std::as_const(endpoints)) {
        painter.drawEllipse(endpoint, dotRadius, dotRadius);
    }

    const QPointF label = worldToScreen(measurement.labelPosition);
    QPointF foot;
    double bestDistance = std::numeric_limits<double>::max();
    for (int i = 1; i < screenPoints.size(); ++i) {
        const QPointF start = screenPoints[i - 1];
        const QPointF end = screenPoints[i];
        const QPointF delta = end - start;
        const double lengthSquared = QPointF::dotProduct(delta, delta);
        const double t = lengthSquared > 1.0e-12
            ? qBound(0.0,
                     QPointF::dotProduct(label - start, delta)
                         / lengthSquared,
                     1.0)
            : 0.0;
        const QPointF candidate = start + delta * t;
        const double distance = QLineF(label, candidate).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            foot = candidate;
        }
    }

    const QString resultText =
        measurement.primaryValue(*m_document, m_settings);
    if (bestDistance < std::numeric_limits<double>::max()
        && bestDistance > 2.0) {
        painter.setPen(QPen(
            color, annotationStroke(0.9, geometryScale),
            Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(foot, label);
        m_measurementHitSegments[measurement.id].append(
            QLineF(foot, label));

        QLineF arrowLine(foot, label);
        const double arrowSize = qMax(
            6.0, metrics.arrowSize * geometryScale);
        arrowLine.setLength(arrowSize);
        const double angle =
            std::atan2(arrowLine.dy(), arrowLine.dx());
        constexpr double spread = 0.34;
        QPainterPath arrow;
        arrow.moveTo(foot);
        arrow.lineTo(
            foot + QPointF(std::cos(angle - spread),
                           std::sin(angle - spread)) * arrowSize);
        arrow.lineTo(
            foot + QPointF(std::cos(angle + spread),
                           std::sin(angle + spread)) * arrowSize);
        arrow.closeSubpath();
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(arrow);
        painter.restore();
    }
    QRectF hitRect;
    drawDimensionText(
        painter, resultText, label, color, 0.0,
        &hitRect, selected);
    if (hitRect.isValid()) {
        m_labelHitRects.insert(measurement.id, hitRect);
    }
    painter.restore();
}

void CadView::drawDimensionChain(QPainter &painter,
                                 const Measurement &measurement,
                                 const QVector<QPointF> &screenPoints,
                                 const QColor &color, bool selected)
{
    if (!m_document || screenPoints.size() < 2) {
        return;
    }
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    struct ChainSegment {
        QPointF start;
        QPointF end;
        QPointF mid;
        bool arc = false;
        double value = 0.0;
    };
    QVector<ChainSegment> segments;
    const bool hasCanonicalSegments = !measurement.segments.isEmpty();
    const bool hasEdges = !measurement.edgeStarts.isEmpty()
        && measurement.edgeStarts.size() == measurement.edgeEnds.size();
    if (hasCanonicalSegments) {
        segments.reserve(measurement.segments.size());
        for (const MeasurementSegment &source : measurement.segments) {
            ChainSegment segment;
            segment.start = worldToScreen(source.start);
            segment.end = worldToScreen(source.end);
            segment.mid = worldToScreen(source.midpoint);
            segment.arc =
                source.kind == MeasurementSegmentKind::Arc;
            segment.value = source.length > 0.0
                ? source.length
                : QLineF(source.start, source.end).length();
            segments.append(segment);
        }
    } else if (hasEdges) {
        segments.reserve(measurement.edgeStarts.size());
        for (int i = 0; i < measurement.edgeStarts.size(); ++i) {
            ChainSegment segment;
            segment.start = worldToScreen(measurement.edgeStarts[i]);
            segment.end = worldToScreen(measurement.edgeEnds[i]);
            segment.mid = i < measurement.edgeMidpoints.size()
                ? worldToScreen(measurement.edgeMidpoints[i])
                : (segment.start + segment.end) * 0.5;
            segment.arc = i < measurement.edgeArcLengths.size()
                && measurement.edgeArcLengths[i] > 0.0;
            segment.value = i < measurement.edgeLengths.size()
                ? measurement.edgeLengths[i]
                : QLineF(measurement.edgeStarts[i],
                         measurement.edgeEnds[i]).length();
            segments.append(segment);
        }
    } else {
        for (int i = 1; i < screenPoints.size(); ++i) {
            ChainSegment segment;
            segment.start = screenPoints[i - 1];
            segment.end = screenPoints[i];
            segment.mid = (segment.start + segment.end) * 0.5;
            segment.value = QLineF(measurement.points[i - 1],
                                   measurement.points[i]).length();
            segments.append(segment);
        }
    }
    if (segments.isEmpty()) {
        return;
    }

    // 尺寸线的偏置侧与距离由标签位置决定：把数字拖到哪一侧，
    // 整条尺寸链就整体偏到那一侧，符合建筑制图“尺寸线平行于被标注对象”的规则。
    const QPointF label = worldToScreen(measurement.labelPosition);
    int referenceIndex = 0;
    double bestDistance = std::numeric_limits<double>::max();
    for (int i = 0; i < segments.size(); ++i) {
        if (segments[i].arc) {
            continue;
        }
        const QPointF delta = segments[i].end - segments[i].start;
        const double lengthSquared = QPointF::dotProduct(delta, delta);
        const double t = lengthSquared > 1.0e-12
            ? qBound(0.0,
                     QPointF::dotProduct(label - segments[i].start, delta)
                         / lengthSquared,
                     1.0)
            : 0.0;
        const double distance =
            QLineF(label, segments[i].start + delta * t).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            referenceIndex = i;
        }
    }
    QPointF referenceDirection =
        segments[referenceIndex].end - segments[referenceIndex].start;
    double referenceLength = std::hypot(referenceDirection.x(),
                                        referenceDirection.y());
    if (referenceLength <= 1.0e-6) {
        referenceDirection = QPointF(1.0, 0.0);
    } else {
        referenceDirection /= referenceLength;
    }
    QPointF referenceNormal(-referenceDirection.y(),
                            referenceDirection.x());
    const QPointF referenceMid =
        (segments[referenceIndex].start + segments[referenceIndex].end) * 0.5;
    double offset =
        QPointF::dotProduct(label - referenceMid, referenceNormal);
    if (offset < 0.0) {
        referenceNormal = -referenceNormal;
        offset = -offset;
    }
    const QFont valueFont = cadFontForText(
        QStringLiteral("0123456789"),
        qMax(1, qRound(metrics.textSize * geometryScale)),
        QFont::Medium);
    const QFontMetricsF valueMetrics(valueFont);
    // 尺寸线偏置按字高推导：标注随图纸缩放时，偏距也一起缩放。
    const double minimumOffset = qMax(6.0, valueMetrics.height() * 1.9);
    offset = qBound(minimumOffset, offset, minimumOffset * 7.0);
    QPen dimensionPen(color,
                      annotationStroke(selected ? metrics.lineWidth + 0.55
                                                : metrics.lineWidth,
                                       geometryScale),
                      Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    dimensionPen.setCosmetic(!m_printStyle);
    QPen extensionPen(color,
                      annotationStroke(selected ? 0.95 : 0.72,
                                       geometryScale),
                      Qt::SolidLine, Qt::FlatCap);
    extensionPen.setCosmetic(!m_printStyle);
    const double textGap = qMax(2.5, metrics.textGap * geometryScale * 0.62);

    painter.save();
    painter.setBrush(Qt::NoBrush);
    const auto drawExtensionLine =
        [&](const QPointF &origin, const QPointF &target) {
            const QPointF direction = target - origin;
            const double length =
                std::hypot(direction.x(), direction.y());
            if (length <= 1.0e-6) {
                return;
            }
            const QPointF unit = direction / length;
            painter.drawLine(
                origin,
                target + unit * (metrics.extensionOverrun * geometryScale));
        };
    for (int i = 0; i < segments.size(); ++i) {
        const ChainSegment &segment = segments[i];
        QPointF direction = segment.end - segment.start;
        const double length = std::hypot(direction.x(), direction.y());
        QPointF normal = referenceNormal;
        if (length > 1.0e-6) {
            direction /= length;
            QPointF candidate(-direction.y(), direction.x());
            if (QPointF::dotProduct(candidate, referenceNormal) < 0.0) {
                candidate = -candidate;
            }
            normal = candidate;
        }
        const QPointF a = segment.start + normal * offset;
        const QPointF b = segment.end + normal * offset;

        painter.setPen(extensionPen);
        if (i == 0) {
            drawExtensionLine(segment.start, a);
        }
        drawExtensionLine(segment.end, b);

        painter.setPen(dimensionPen);
        if (segment.arc) {
            const QPainterPath arcPath = arcPathThroughThreePoints(
                a, segment.mid + normal * offset, b);
            if (!arcPath.isEmpty() && arcPath.length() > 1.0) {
                painter.drawPath(arcPath);
            } else {
                painter.drawLine(a, b);
            }
        } else {
            painter.drawLine(a, b);
        }
        if (i == 0) {
            drawArrow(painter, a, b, color);
        }
        drawArrow(painter, b, a, color);

        const QString value = formatDimensionValue(
            segment.value, *m_document, m_settings.lengthUnit,
            m_settings.lengthPrecision);
        const double angle = length > 1.0e-6
            ? std::atan2(direction.y(), direction.x()) * 180.0 / kPi
            : 0.0;
        // 数字写在尺寸线上方中部：先抬高半个字高，避免压线。
        const double textLift = valueMetrics.height() * 0.5;
        const QPointF textAnchor = segment.arc
            ? segment.mid + normal * (offset + textGap + textLift)
            : (a + b) * 0.5 + normal * (textGap + textLift);
        const double available = length;
        const double textWidth =
            valueMetrics.horizontalAdvance(value);
        if (available > textWidth * 0.92) {
            // 数字写在尺寸线上方中部（建筑标注习惯），尺寸线不断开。
            drawDimensionText(painter, value, textAnchor, color, angle,
                              nullptr, selected);
        } else {
            // 文字宽度超过两端间距时移到外侧，用无箭头引线接回尺寸线；
            // 引线起点落在尺寸线中点，终点止于文字靠近尺寸线的一侧。
            const QPointF outward =
                textAnchor + normal * (valueMetrics.height() + textGap);
            const QPointF textEdge =
                outward - normal * (valueMetrics.height() * 0.58);
            painter.setPen(QPen(
                color, annotationStroke(0.8, geometryScale),
                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine((a + b) * 0.5, textEdge);
            drawDimensionText(painter, value, outward, color, 0.0,
                              nullptr, selected);
        }
    }
    painter.restore();
}

void CadView::drawSideAreaAnnotation(
    QPainter &painter, const Measurement &measurement,
    const QVector<QPointF> &screenPoints, const QColor &color,
    bool selected)
{
    if (!m_document || screenPoints.size() < 2) {
        return;
    }
    const AnnotationStyleMetrics metrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    struct Edge {
        QPointF start;
        QPointF end;
        QPointF mid;
        bool arc = false;
        double value = 0.0;
    };
    QVector<Edge> edges;
    const bool hasCanonicalSegments = !measurement.segments.isEmpty();
    const bool hasEdges = !measurement.edgeStarts.isEmpty()
        && measurement.edgeStarts.size() == measurement.edgeEnds.size();
    if (hasCanonicalSegments) {
        edges.reserve(measurement.segments.size());
        for (const MeasurementSegment &source : measurement.segments) {
            Edge edge;
            edge.start = worldToScreen(source.start);
            edge.end = worldToScreen(source.end);
            edge.mid = worldToScreen(source.midpoint);
            edge.arc = source.kind == MeasurementSegmentKind::Arc;
            edge.value = source.length > 0.0
                ? source.length
                : QLineF(source.start, source.end).length();
            edges.append(edge);
        }
    } else if (hasEdges) {
        edges.reserve(measurement.edgeStarts.size());
        for (int i = 0; i < measurement.edgeStarts.size(); ++i) {
            Edge edge;
            edge.start = worldToScreen(measurement.edgeStarts[i]);
            edge.end = worldToScreen(measurement.edgeEnds[i]);
            edge.mid = i < measurement.edgeMidpoints.size()
                ? worldToScreen(measurement.edgeMidpoints[i])
                : (edge.start + edge.end) * 0.5;
            edge.arc = i < measurement.edgeArcLengths.size()
                && measurement.edgeArcLengths[i] > 0.0;
            edge.value = i < measurement.edgeLengths.size()
                ? measurement.edgeLengths[i]
                : QLineF(measurement.edgeStarts[i],
                         measurement.edgeEnds[i]).length();
            edges.append(edge);
        }
    } else {
        for (int i = 1; i < screenPoints.size(); ++i) {
            Edge edge;
            edge.start = screenPoints[i - 1];
            edge.end = screenPoints[i];
            edge.mid = (edge.start + edge.end) * 0.5;
            edge.value = QLineF(measurement.points[i - 1],
                                measurement.points[i]).length();
            edges.append(edge);
        }
    }
    if (edges.isEmpty()) {
        return;
    }

    const int fontPixels =
        qMax(1, qRound(metrics.textSize * geometryScale));
    const QFont valueFont = cadFontForText(
        QStringLiteral("0123456789"), fontPixels, QFont::Medium);
    const QFontMetricsF valueMetrics(valueFont);
    const double dotRadius =
        qBound(2.2, 2.4 * geometryScale, 5.0);

    QPen dimensionPen(
        color,
        annotationStroke(selected ? metrics.lineWidth + 0.55
                                  : metrics.lineWidth,
                         geometryScale),
        Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
    dimensionPen.setCosmetic(!m_printStyle);

    painter.save();
    painter.setBrush(Qt::NoBrush);
    for (int i = 1; i < screenPoints.size(); ++i) {
        m_measurementHitSegments[measurement.id].append(
            QLineF(screenPoints[i - 1], screenPoints[i]));
    }
    QVector<QPointF> endpoints;
    for (const Edge &edge : std::as_const(edges)) {
        const QPointF chord = edge.end - edge.start;
        const double chordLength = std::hypot(chord.x(), chord.y());
        QPointF tangent = chordLength > 1.0e-6
            ? chord / chordLength : QPointF(1.0, 0.0);
        if (edge.arc) {
            if (const std::optional<QPointF> center =
                    circleCenterFromThreePoints(
                        edge.start, edge.mid, edge.end)) {
                const QPointF radial = edge.mid - *center;
                const double radialLength =
                    std::hypot(radial.x(), radial.y());
                if (radialLength > 1.0e-6) {
                    QPointF arcTangent(-radial.y() / radialLength,
                                       radial.x() / radialLength);
                    if (QPointF::dotProduct(arcTangent, tangent) < 0.0) {
                        arcTangent = -arcTangent;
                    }
                    tangent = arcTangent;
                }
            }
        }
        const QPointF midRef =
            edge.arc ? edge.mid : (edge.start + edge.end) * 0.5;
        const QString value = formatDimensionValue(
            edge.value, *m_document, m_settings.lengthUnit,
            m_settings.lengthPrecision);
        const double angle =
            std::atan2(tangent.y(), tangent.x()) * 180.0 / kPi;
        const double textWidth =
            valueMetrics.horizontalAdvance(value);

        // 尺寸线直接落在实测线段或圆弧上。
        painter.setPen(dimensionPen);
        if (edge.arc) {
            const QPainterPath arcPath = arcPathThroughThreePoints(
                edge.start, edge.mid, edge.end);
            if (!arcPath.isEmpty() && arcPath.length() > 0.8) {
                painter.drawPath(arcPath);
            } else {
                painter.drawLine(edge.start, edge.end);
            }
        } else if (chordLength > textWidth + 10.0) {
            const QPointF unit = chord / chordLength;
            const QPointF before =
                midRef - unit * (textWidth * 0.5 + 4.0);
            const QPointF after =
                midRef + unit * (textWidth * 0.5 + 4.0);
            painter.drawLine(edge.start, before);
            painter.drawLine(after, edge.end);
        } else {
            painter.drawLine(edge.start, edge.end);
        }
        drawDimensionText(
            painter, value, midRef, color, angle,
            nullptr, selected);
        for (const QPointF &endpoint :
             {edge.start, edge.end}) {
            bool alreadyAdded = false;
            for (const QPointF &existing : std::as_const(endpoints)) {
                if (QLineF(endpoint, existing).length() < 0.5) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded) {
                endpoints.append(endpoint);
            }
        }
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (const QPointF &endpoint : std::as_const(endpoints)) {
        painter.drawEllipse(endpoint, dotRadius, dotRadius);
    }

    const QPointF label = worldToScreen(measurement.labelPosition);
    QPointF foot;
    double bestDistance = std::numeric_limits<double>::max();
    for (int i = 1; i < screenPoints.size(); ++i) {
        const QPointF start = screenPoints[i - 1];
        const QPointF end = screenPoints[i];
        const QPointF delta = end - start;
        const double lengthSquared = QPointF::dotProduct(delta, delta);
        const double t = lengthSquared > 1.0e-12
            ? qBound(0.0,
                     QPointF::dotProduct(label - start, delta)
                         / lengthSquared,
                     1.0)
            : 0.0;
        const QPointF candidate = start + delta * t;
        const double distance = QLineF(label, candidate).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            foot = candidate;
        }
    }
    if (bestDistance < std::numeric_limits<double>::max()
        && bestDistance > 2.0) {
        painter.setPen(QPen(
            color, annotationStroke(0.9, geometryScale),
            Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(foot, label);
        m_measurementHitSegments[measurement.id].append(
            QLineF(foot, label));

        QLineF arrowLine(foot, label);
        const double arrowSize = qMax(
            6.0, metrics.arrowSize * geometryScale);
        arrowLine.setLength(arrowSize);
        const double angle =
            std::atan2(arrowLine.dy(), arrowLine.dx());
        constexpr double spread = 0.34;
        QPainterPath arrow;
        arrow.moveTo(foot);
        arrow.lineTo(
            foot + QPointF(std::cos(angle - spread),
                           std::sin(angle - spread)) * arrowSize);
        arrow.lineTo(
            foot + QPointF(std::cos(angle + spread),
                           std::sin(angle + spread)) * arrowSize);
        arrow.closeSubpath();
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(arrow);
        painter.restore();
    }
    const QString resultText =
        measurement.primaryValue(*m_document, m_settings);
    QRectF hitRect;
    drawDimensionText(
        painter, resultText, label, color, 0.0,
        &hitRect, selected);
    if (hitRect.isValid()) {
        m_labelHitRects.insert(measurement.id, hitRect);
    }
    painter.restore();
}

void CadView::drawMeasurements(QPainter &painter)
{
    m_labelHitRects.clear();
    m_measurementHitSegments.clear();
    m_measurementHitBounds.clear();
    for (const Measurement &measurement : std::as_const(m_measurements)) {
        if (!measurement.visible || measurement.space != m_activeSpace) {
            continue;
        }
        drawMeasurement(
            painter, measurement,
            measurement.id == m_selectedMeasurementId
                || (m_mode == ToolMode::Erase
                    && measurement.id == m_hoveredMeasurementId));
        QRectF bounds = m_labelHitRects.value(measurement.id);
        for (const QLineF &line :
             m_measurementHitSegments.value(measurement.id)) {
            bounds = bounds.isNull()
                ? QRectF(line.p1(), line.p2()).normalized()
                : bounds.united(
                      QRectF(line.p1(), line.p2()).normalized());
        }
        for (const QPointF &point : measurement.points) {
            const QPointF screen = worldToScreen(point);
            const QRectF pointRect(screen, QSizeF(1.0, 1.0));
            bounds = bounds.isNull()
                ? pointRect : bounds.united(pointRect);
        }
        if (!bounds.isNull()) {
            m_measurementHitBounds.insert(
                measurement.id,
                bounds.adjusted(-9.0, -9.0, 9.0, 9.0));
        }
    }
}

void CadView::drawMeasurement(QPainter &painter,
                              const Measurement &measurement, bool selected)
{
    if (!m_document) {
        return;
    }
    const double previousMeasurementScale = m_currentMeasurementScale;
    const bool previousDimensionTextScale =
        m_applyingDimensionTextScale;
    m_currentMeasurementScale = qBound(
        0.1, measurement.dimensionScale, 100.0);
    m_applyingDimensionTextScale =
        measurement.type != MeasurementType::Text;
    QColor color = annotationColor(measurement);
    if (m_printStyle) {
        color = printColor(color);
    }
    if (selected) {
        color = color.lighter(125);
    }
    QVector<QPointF> points;
    points.reserve(measurement.points.size());
    for (const QPointF &point : measurement.points) {
        points.append(worldToScreen(point));
    }
    const QPointF label = worldToScreen(measurement.labelPosition);
    const QString value =
        measurement.primaryValue(*m_document, m_settings);
    const AnnotationStyleMetrics styleMetrics =
        annotationStyleMetrics(m_settings.annotationStyle);
    const double geometryScale = annotationGeometryScale();
    QPen pen(color,
             annotationStroke(selected ? styleMetrics.lineWidth + 0.65
                                       : styleMetrics.lineWidth,
                              geometryScale),
             Qt::SolidLine, Qt::RoundCap,
             Qt::RoundJoin);
    pen.setCosmetic(!m_printStyle);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (measurement.type == MeasurementType::Rectangle
        || measurement.type == MeasurementType::Polygon
        || measurement.type == MeasurementType::FilledArea) {
        // 建筑制图习惯：面积标注只画房间轮廓 + 居中的面积/周长文字，
        // 不在每条边上堆叠尺寸文字。
        if (points.size() >= 3) {
            const QPainterPath outline =
                measurementOutline(measurement, points);
            if (!outline.isEmpty()) {
                painter.save();
                // Area measurements are construction annotations, not filled
                // CAD HATCH entities. Keep the interior transparent so the
                // drawing underneath remains fully readable.
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(
                    color,
                    annotationStroke(selected ? 1.85 : 1.35,
                                     geometryScale),
                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPath(outline);
                painter.restore();
                if (selected) {
                    painter.save();
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(color.lighter(115));
                    for (const QPointF &vertex : points) {
                        painter.drawRect(QRectF(vertex.x() - 2.6,
                                                vertex.y() - 2.6,
                                                5.2, 5.2));
                    }
                    painter.restore();
                }
            }
            if (measurement.type == MeasurementType::Rectangle
                && points.size() >= 4) {
                QPointF centroid;
                for (const QPointF &point : std::as_const(points)) {
                    centroid += point;
                }
                centroid /= double(points.size());
                const double offsetPixels = qMax(
                    18.0,
                    styleMetrics.textGap * geometryScale
                        + 13.0 * geometryScale);
                const QFont valueFont = cadFontForText(
                    QStringLiteral("0123456789"),
                    qMax(1, qRound(
                        styleMetrics.textSize * geometryScale)),
                    QFont::Medium);
                const QFontMetricsF valueMetrics(valueFont);
                int horizontalIndex = -1;
                int verticalIndex = -1;
                double longestHorizontal = 0.0;
                double longestVertical = 0.0;
                for (int index = 0; index < 4; ++index) {
                    const QPointF start = points[index];
                    const QPointF end = points[(index + 1) % 4];
                    const double horizontal =
                        qAbs(end.x() - start.x());
                    const double vertical =
                        qAbs(end.y() - start.y());
                    if (horizontal >= vertical
                        && horizontal > longestHorizontal) {
                        longestHorizontal = horizontal;
                        horizontalIndex = index;
                    }
                    if (vertical >= horizontal
                        && vertical > longestVertical) {
                        longestVertical = vertical;
                        verticalIndex = index;
                    }
                }
                const auto drawSideDimension =
                    [&](int edgeIndex) {
                        if (edgeIndex < 0) {
                            return;
                        }
                        const QPointF start = points[edgeIndex];
                        const QPointF end =
                            points[(edgeIndex + 1) % points.size()];
                        QPointF direction = end - start;
                        const double length =
                            std::hypot(direction.x(), direction.y());
                        if (length <= 1.0e-6) {
                            return;
                        }
                        direction /= length;
                        QPointF normal(-direction.y(), direction.x());
                        const QPointF mid = (start + end) * 0.5;
                        if (QPointF::dotProduct(
                                mid - centroid, normal) < 0.0) {
                            normal = -normal;
                        }
                        const QPointF a =
                            start + normal * offsetPixels;
                        const QPointF b =
                            end + normal * offsetPixels;
                        painter.setPen(QPen(
                            color,
                            annotationStroke(0.72, geometryScale),
                            Qt::SolidLine, Qt::FlatCap));
                        painter.drawLine(
                            start,
                            a + normal
                                * (styleMetrics.extensionOverrun
                                   * geometryScale));
                        painter.drawLine(
                            end,
                            b + normal
                                * (styleMetrics.extensionOverrun
                                   * geometryScale));
                        const QPointF extensionStart =
                            a + normal
                                * (styleMetrics.extensionOverrun
                                   * geometryScale);
                        const QPointF extensionEnd =
                            b + normal
                                * (styleMetrics.extensionOverrun
                                   * geometryScale);
                        m_measurementHitSegments[measurement.id].append(
                            QLineF(start, extensionStart));
                        m_measurementHitSegments[measurement.id].append(
                            QLineF(end, extensionEnd));
                        painter.setPen(QPen(
                            color,
                            annotationStroke(
                                selected
                                    ? styleMetrics.lineWidth + 0.55
                                    : styleMetrics.lineWidth,
                                geometryScale),
                            Qt::SolidLine, Qt::FlatCap));
                        painter.drawLine(a, b);
                        m_measurementHitSegments[measurement.id].append(
                            QLineF(a, b));
                        drawArrow(painter, a, b, color);
                        drawArrow(painter, b, a, color);
                        const double value =
                            QLineF(measurement.points[edgeIndex],
                                   measurement.points[
                                       (edgeIndex + 1)
                                       % measurement.points.size()])
                                .length();
                        const QString label = formatDimensionValue(
                            value, *m_document,
                            m_settings.lengthUnit,
                            m_settings.lengthPrecision);
                        const double angle =
                            std::atan2(direction.y(), direction.x())
                            * 180.0 / kPi;
                        const QPointF textAnchor =
                            (a + b) * 0.5
                            + normal
                                * (styleMetrics.textGap
                                       * geometryScale
                                   + valueMetrics.height() * 0.55);
                        drawDimensionText(
                            painter, label, textAnchor, color,
                            angle, nullptr, selected);
                    };
                drawSideDimension(horizontalIndex);
                drawSideDimension(verticalIndex);
            }
        }
    } else if (measurement.type == MeasurementType::Batch
               && points.size() >= 2) {
        drawDimensionChain(
            painter, measurement, points, color, selected);
    } else if (measurement.type == MeasurementType::Continuous
               && points.size() >= 2) {
        drawContinuousDimension(
            painter, measurement, points, color, selected);
    } else if (measurement.type == MeasurementType::SideArea
               && points.size() >= 2) {
        // 侧面积：尺寸线画在选中的线段/圆弧本身上，段长直接写在线上，
        // 面积结果单独用引线标注，不与段长数字混淆。
        drawSideAreaAnnotation(painter, measurement, points, color,
                               selected);
    } else if ((measurement.type == MeasurementType::CircleArea
                || measurement.type == MeasurementType::Radius)
               && points.size() >= 2) {
        const double radius =
            QLineF(points[0], points[1]).length();
        bool drewSourceArc = false;
        if (measurement.type == MeasurementType::Radius) {
            for (const MeasurementSegment &segment :
                 std::as_const(measurement.segments)) {
                if (segment.kind != MeasurementSegmentKind::Arc) {
                    continue;
                }
                const QPainterPath arc = arcPathThroughThreePoints(
                    worldToScreen(segment.start),
                    worldToScreen(segment.midpoint),
                    worldToScreen(segment.end));
                if (!arc.isEmpty()) {
                    painter.drawPath(arc);
                    drewSourceArc = true;
                }
            }
        }
        if (!drewSourceArc) {
            painter.drawEllipse(points[0], radius, radius);
        }
        if (measurement.type == MeasurementType::Radius) {
            m_measurementHitSegments[measurement.id].append(
                QLineF(points[0], points[1]));
        }
    } else if (measurement.type == MeasurementType::Angle
               && points.size() >= 3) {
        const QPointF vertex = points[0];
        const double angleA = std::atan2(points[1].y() - vertex.y(),
                                         points[1].x() - vertex.x());
        const double angleB = std::atan2(points[2].y() - vertex.y(),
                                         points[2].x() - vertex.x());
        const double arcRadius = 28.0 * geometryScale;
        QRectF arcBox(vertex.x() - arcRadius,
                      vertex.y() - arcRadius,
                      arcRadius * 2.0, arcRadius * 2.0);
        QPainterPath arc;
        arc.arcMoveTo(arcBox, -angleA * 180.0 / kPi);
        double sweep = (angleB - angleA) * 180.0 / kPi;
        while (sweep > 180.0) {
            sweep -= 360.0;
        }
        while (sweep < -180.0) {
            sweep += 360.0;
        }
        arc.arcTo(arcBox, -angleA * 180.0 / kPi, -sweep);
        painter.drawPath(arc);
        for (int i = 1; i <= 24; ++i) {
            m_measurementHitSegments[measurement.id].append(
                QLineF(arc.pointAtPercent(double(i - 1) / 24.0),
                       arc.pointAtPercent(double(i) / 24.0)));
        }
        painter.drawLine(vertex, points[1]);
        painter.drawLine(vertex, points[2]);
    } else if (measurement.type == MeasurementType::ArcLength
               && points.size() >= 3) {
        const QPointF a = measurement.points[0];
        const QPointF b = measurement.points[1];
        const QPointF c = measurement.points[2];
        const double d = 2.0
            * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y())
               + c.x() * (a.y() - b.y()));
        if (qAbs(d) > 1.0e-12) {
            const QPointF center(
                ((a.x() * a.x() + a.y() * a.y()) * (b.y() - c.y())
                 + (b.x() * b.x() + b.y() * b.y()) * (c.y() - a.y())
                 + (c.x() * c.x() + c.y() * c.y()) * (a.y() - b.y()))
                    / d,
                ((a.x() * a.x() + a.y() * a.y()) * (c.x() - b.x())
                 + (b.x() * b.x() + b.y() * b.y()) * (a.x() - c.x())
                 + (c.x() * c.x() + c.y() * c.y()) * (b.x() - a.x()))
                    / d);
            QPainterPath path;
            path.moveTo(points[0]);
            const double start = std::atan2(points[0].y() - worldToScreen(center).y(),
                                            points[0].x() - worldToScreen(center).x());
            const double through = std::atan2(
                points[1].y() - worldToScreen(center).y(),
                points[1].x() - worldToScreen(center).x());
            const double end = std::atan2(points[2].y() - worldToScreen(center).y(),
                                          points[2].x() - worldToScreen(center).x());
            double sweep = end - start;
            if (sweep < 0.0) {
                sweep += 2.0 * kPi;
            }
            double throughSweep = through - start;
            if (throughSweep < 0.0) {
                throughSweep += 2.0 * kPi;
            }
            if (throughSweep > sweep) {
                sweep -= 2.0 * kPi;
            }
            QRectF circleBox(points[0], points[0]);
            const double screenRadius =
                QLineF(points[0], worldToScreen(center)).length();
            circleBox = QRectF(worldToScreen(center).x() - screenRadius,
                               worldToScreen(center).y() - screenRadius,
                               screenRadius * 2.0, screenRadius * 2.0);
            path = QPainterPath();
            path.arcMoveTo(circleBox, -start * 180.0 / kPi);
            path.arcTo(circleBox, -start * 180.0 / kPi,
                       -sweep * 180.0 / kPi);
            painter.drawPath(path);
            for (int i = 1; i <= 24; ++i) {
                m_measurementHitSegments[measurement.id].append(
                    QLineF(path.pointAtPercent(double(i - 1) / 24.0),
                           path.pointAtPercent(double(i) / 24.0)));
            }
        }
    } else if ((measurement.type == MeasurementType::Aligned
                || (measurement.type == MeasurementType::Linear
                    && measurement.auxiliaryValue < 0.5))
               && points.size() >= 2) {
        const QPointF direction = points[1] - points[0];
        QPointF normal(-direction.y(), direction.x());
        const double normalLength = std::hypot(normal.x(), normal.y());
        if (normalLength > 1.0e-9) {
            normal /= normalLength;
            const double offset =
                QPointF::dotProduct(label - points[0], normal);
            const QPointF a = points[0] + normal * offset;
            const QPointF b = points[1] + normal * offset;
            const auto drawExtension = [&](const QPointF &origin,
                                           const QPointF &dimensionPoint) {
                QPointF direction = dimensionPoint - origin;
                const double length =
                    std::hypot(direction.x(), direction.y());
                if (length <= 1.0e-9) {
                    return;
                }
                direction /= length;
                painter.drawLine(
                    origin,
                    dimensionPoint
                        + direction
                            * (styleMetrics.extensionOverrun
                               * geometryScale));
            };
            drawExtension(points[0], a);
            drawExtension(points[1], b);
            painter.drawLine(a, b);
            const QPointF midpoint = (a + b) * 0.5;
            drawArrow(painter, a, midpoint, color);
            drawArrow(painter, b, midpoint, color);
        }
    } else if (measurement.type == MeasurementType::Linear
               && points.size() >= 2) {
        QPointF a = points[0];
        QPointF b = points[1];
        if (measurement.auxiliaryValue == 1.0) {
            a.setY(label.y());
            b.setY(label.y());
        } else {
            a.setX(label.x());
            b.setX(label.x());
        }
        const auto drawExtension = [&](const QPointF &origin,
                                       const QPointF &dimensionPoint) {
            QPointF direction = dimensionPoint - origin;
            const double length =
                std::hypot(direction.x(), direction.y());
            if (length <= 1.0e-9) {
                return;
            }
            direction /= length;
            painter.drawLine(
                origin,
                dimensionPoint
                    + direction
                        * (styleMetrics.extensionOverrun
                           * geometryScale));
        };
        drawExtension(points[0], a);
        drawExtension(points[1], b);
        painter.drawLine(a, b);
        const QPointF midpoint = (a + b) * 0.5;
        drawArrow(painter, a, midpoint, color);
        drawArrow(painter, b, midpoint, color);
    }

    QString labelText = value;
    if (!measurement.note.isEmpty()
        && measurement.type != MeasurementType::Text) {
        labelText += QStringLiteral("\n") + measurement.note;
    }
    if (measurement.type == MeasurementType::Text) {
        labelText = measurement.note;
    }
    QRectF hitRect;
    const bool boxedAnnotation = measurement.type == MeasurementType::Text;
    double textAngle = 0.0;
    QPointF textAnchor = label;
    if (measurement.type == MeasurementType::Aligned
        && points.size() >= 2) {
        textAngle = std::atan2(points[1].y() - points[0].y(),
                               points[1].x() - points[0].x())
            * 180.0 / kPi;
        QPointF normal(-(points[1].y() - points[0].y()),
                       points[1].x() - points[0].x());
        const double normalLength = std::hypot(normal.x(), normal.y());
        if (normalLength > 1.0e-9) {
            normal /= normalLength;
            const double offset =
                QPointF::dotProduct(label - points[0], normal);
            const QPointF lineCenter =
                (points[0] + points[1]) * 0.5 + normal * offset;
            const double side =
                QPointF::dotProduct(label - lineCenter, normal) >= 0.0
                ? 1.0
                : -1.0;
            textAnchor =
                lineCenter
                + normal * side
                    * (styleMetrics.textGap * geometryScale);
        }
    } else if (measurement.type == MeasurementType::Linear
               && points.size() >= 2) {
        textAngle = measurement.auxiliaryValue == 2.0 ? -90.0 : 0.0;
        if (measurement.auxiliaryValue == 1.0) {
            const double side = label.y() >= points[0].y() ? 1.0 : -1.0;
            textAnchor = QPointF(
                (points[0].x() + points[1].x()) * 0.5,
                label.y()
                    + side
                        * (styleMetrics.textGap * geometryScale));
        } else {
            const double side = label.x() >= points[0].x() ? 1.0 : -1.0;
            textAnchor = QPointF(
                label.x()
                    + side
                        * (styleMetrics.textGap * geometryScale),
                (points[0].y() + points[1].y()) * 0.5);
        }
    } else if (measurement.type == MeasurementType::Radius) {
        textAnchor = label;
        textAngle = 0.0;
    }
    bool primaryTextDrawnInline = false;
    if ((measurement.type == MeasurementType::CircleArea
         || measurement.type == MeasurementType::Radius)
        && measurement.points.size() >= 2) {
        drawInlineDimension(
            painter, labelText, measurement.points[0],
            measurement.points[1], measurement.labelPosition, color,
            &hitRect, false);
        primaryTextDrawnInline = true;
    }
    const bool measurementLabelHandled =
        measurement.type == MeasurementType::Continuous
        || measurement.type == MeasurementType::SideArea;
    if (!boxedAnnotation && !primaryTextDrawnInline && !measurementLabelHandled
        && !points.isEmpty()) {
        const bool closedArea =
            measurement.type == MeasurementType::Rectangle
            || measurement.type == MeasurementType::Polygon
            || measurement.type == MeasurementType::FilledArea;
        const bool labelInsideArea =
            closedArea && points.size() >= 3
            && QPolygonF(points).containsPoint(
                textAnchor, Qt::OddEvenFill);
        QPointF leaderTarget;
        double nearestDistance =
            std::numeric_limits<double>::max();
        {
            const int segmentCount =
                closedArea && points.size() >= 3
                ? int(points.size()) : int(points.size()) - 1;
            for (int index = 0; index < segmentCount; ++index) {
                const QPointF start = points[index];
                const QPointF end =
                    points[(index + 1) % points.size()];
                const QPointF delta = end - start;
                const double lengthSquared =
                    QPointF::dotProduct(delta, delta);
                const double t = lengthSquared > 1.0e-12
                    ? qBound(
                          0.0,
                          QPointF::dotProduct(
                              textAnchor - start, delta)
                              / lengthSquared,
                          1.0)
                    : 0.0;
                const QPointF candidate = start + delta * t;
                const double distance =
                    QLineF(textAnchor, candidate).length();
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    leaderTarget = candidate;
                }
            }
        }
        const double leaderThreshold =
            (styleMetrics.textGap + 4.0) * geometryScale;
        if (!isDimensionMeasurement(measurement.type)
            && !labelInsideArea
            && nearestDistance
                < std::numeric_limits<double>::max()
            && nearestDistance > leaderThreshold) {
            painter.setPen(QPen(
                color, selected ? 1.35 : 1.0,
                Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(textAnchor, leaderTarget);
            m_measurementHitSegments[measurement.id].append(
                QLineF(textAnchor, leaderTarget));
            drawArrow(painter, leaderTarget, textAnchor, color);
        }
    }
    if (boxedAnnotation) {
        drawVectorLabel(painter, labelText, label, color, &hitRect,
                        selected);
    } else if (!primaryTextDrawnInline && !measurementLabelHandled) {
        if (measurement.type == MeasurementType::SideArea) {
            // 侧面积 = 选中线段/圆弧总长 × 输入高度，结果直接标在图上，
            // 用带底板的框与引线，避免和尺寸链的逐段数字混淆。
            drawNotePlate(painter, labelText, textAnchor, color, &hitRect,
                          selected);
        } else {
            // 连续尺寸链：图面只保留逐段数字，“总长”在测量列表查看，
            // 避免主标签与段数字重叠。
            drawDimensionText(painter, labelText, textAnchor, color,
                              textAngle, &hitRect, selected);
        }
    }
    if (hitRect.isValid()) {
        m_labelHitRects.insert(measurement.id, hitRect);
    }
    m_currentMeasurementScale = previousMeasurementScale;
    m_applyingDimensionTextScale = previousDimensionTextScale;
}

void CadView::populateTracedMeasurement(
    Measurement &measurement,
    const QVector<QPointF> &sourcePoints) const
{
    if (sourcePoints.size() < 2) {
        return;
    }
    QVector<QPointF> traced;
    traced.reserve(sourcePoints.size() * 8);
    traced.append(sourcePoints.first());
    for (int index = 1; index < sourcePoints.size(); ++index) {
        const QPointF start = sourcePoints[index - 1];
        const QPointF end = sourcePoints[index];
        QVector<QPointF> arcPath;
        double arcLength = 0.0;
        QPointF midpoint;
        if (traceArcBetween(
                start, end, &arcPath, &arcLength, &midpoint)) {
            if (!arcPath.isEmpty()) {
                arcPath.removeFirst();
            }
            traced.append(arcPath);
            measurement.edgeLengths.append(arcLength);
            measurement.edgeArcLengths.append(arcLength);
            measurement.edgeMidpoints.append(midpoint);
            measurement.edgeStarts.append(start);
            measurement.edgeEnds.append(end);
        } else {
            traced.append(end);
            const double length = QLineF(start, end).length();
            measurement.edgeLengths.append(length);
            measurement.edgeArcLengths.append(0.0);
            measurement.edgeMidpoints.append((start + end) * 0.5);
            measurement.edgeStarts.append(start);
            measurement.edgeEnds.append(end);
        }
    }
    measurement.points = traced;
}

std::optional<Measurement> CadView::draftMeasurement(
    const QPointF &labelWorld, bool includeProvisionalPoint) const
{
    if (m_draftPoints.isEmpty()) {
        return std::nullopt;
    }
    QVector<QPointF> sourcePoints = m_draftPoints;
    if (includeProvisionalPoint) {
        if (QLineF(sourcePoints.last(), m_cursorWorld).length()
            > snapRadiusWorld() * 0.02) {
            sourcePoints.append(m_cursorWorld);
        }
    }
    if (m_mode == ToolMode::Radius && sourcePoints.size() < 3) {
        return std::nullopt;
    }
    Measurement measurement;
    measurement.id = QStringLiteral("draft");
    measurement.labelPosition = labelWorld;
    measurement.points = sourcePoints;
    switch (m_mode) {
    case ToolMode::Aligned:
        measurement.type = MeasurementType::Aligned;
        break;
    case ToolMode::Linear:
        measurement.type = MeasurementType::Linear;
        measurement.auxiliaryValue =
            qAbs(sourcePoints.last().x() - sourcePoints.first().x())
                >= qAbs(sourcePoints.last().y()
                        - sourcePoints.first().y())
            ? 1.0
            : 2.0;
        break;
    case ToolMode::Continuous:
        measurement.type = MeasurementType::Continuous;
        populateTracedMeasurement(measurement, sourcePoints);
        break;
    case ToolMode::Batch:
        measurement.type = MeasurementType::Batch;
        break;
    case ToolMode::Rectangle:
        measurement.type = MeasurementType::Rectangle;
        measurement.closed = true;
        break;
    case ToolMode::Polygon:
        measurement.type = MeasurementType::Polygon;
        measurement.closed = true;
        if (sourcePoints.size() >= 3) {
            QVector<QPointF> traced;
            traced.reserve(sourcePoints.size() * 8);
            for (int i = 0; i < sourcePoints.size(); ++i) {
                const QPointF start = sourcePoints[i];
                const QPointF end =
                    sourcePoints[(i + 1) % sourcePoints.size()];
                if (traced.isEmpty()) {
                    traced.append(start);
                }
                QVector<QPointF> arcPath;
                double arcLength = 0.0;
                QPointF midpoint;
                if (traceArcBetween(start, end, &arcPath, &arcLength,
                                    &midpoint)) {
                    if (!arcPath.isEmpty()) {
                        arcPath.removeFirst();
                    }
                    traced.append(arcPath);
                    measurement.edgeLengths.append(arcLength);
                    measurement.edgeMidpoints.append(midpoint);
                    measurement.edgeStarts.append(start);
                    measurement.edgeEnds.append(end);
                } else {
                    traced.append(end);
                    measurement.edgeLengths.append(
                        QLineF(start, end).length());
                    measurement.edgeMidpoints.append((start + end) * 0.5);
                    measurement.edgeStarts.append(start);
                    measurement.edgeEnds.append(end);
                }
            }
            if (traced.size() > 1
                && QLineF(traced.first(), traced.last()).length()
                    < snapRadiusWorld() * 0.1) {
                traced.removeLast();
            }
            measurement.points = traced;
        }
        break;
    case ToolMode::CircleArea:
        measurement.type = MeasurementType::CircleArea;
        measurement.closed = true;
        break;
    case ToolMode::FilledArea:
        measurement.type = MeasurementType::FilledArea;
        measurement.closed = true;
        break;
    case ToolMode::SideArea:
        measurement.type = MeasurementType::SideArea;
        measurement.auxiliaryValue = m_toolAuxiliaryValue;
        populateTracedMeasurement(measurement, sourcePoints);
        break;
    case ToolMode::Radius:
        measurement.type = MeasurementType::Radius;
        break;
    case ToolMode::Angle:
        measurement.type = MeasurementType::Angle;
        break;
    case ToolMode::ArcLength:
        measurement.type = MeasurementType::ArcLength;
        break;
    default:
        return std::nullopt;
    }
    updateAnnotationGeometryFromDraft(measurement);
    measurement.normalizeGeometry();
    return measurement;
}

void CadView::updateAnnotationGeometryFromDraft(
    Measurement &measurement) const
{
    if ((measurement.type == MeasurementType::Radius
         || measurement.type == MeasurementType::CircleArea)
        && measurement.points.size() >= 3) {
        const auto center = circleCenterFromThreePoints(
            measurement.points[0], measurement.points[1],
            measurement.points[2]);
        if (center) {
            const QPointF rim = measurement.points.first();
            const double radius =
                QLineF(*center, rim).length();
            if (measurement.type == MeasurementType::CircleArea) {
                measurement.points = {*center, rim};
                QPointF labelDirection =
                    measurement.labelPosition - *center;
                double labelDistance =
                    std::hypot(labelDirection.x(), labelDirection.y());
                if (labelDistance <= 1.0e-9) {
                    labelDirection = QPointF(0.0, -1.0);
                    labelDistance = 1.0;
                } else {
                    labelDirection /= labelDistance;
                }
                measurement.labelPosition =
                    *center
                    + labelDirection * (radius * 0.55);
                return;
            }
            QPointF direction =
                measurement.labelPosition - *center;
            if (std::hypot(direction.x(), direction.y()) < 1.0e-9) {
                direction = measurement.points.first() - *center;
            }
            const double directionLength =
                std::hypot(direction.x(), direction.y());
            if (directionLength > 1.0e-9 && radius > 1.0e-9) {
                direction /= directionLength;
                measurement.points = {
                    *center, *center + direction * radius
                };
            }
        }
    }
    if (measurement.type == MeasurementType::Rectangle
        && measurement.points.size() >= 2) {
        const QPointF a = measurement.points[0];
        const QPointF b = measurement.points[1];
        measurement.points = {
            a, QPointF(b.x(), a.y()), b, QPointF(a.x(), b.y())
        };
    }
}

void CadView::drawDraft(QPainter &painter)
{
    if ((m_mode == ToolMode::PrintArea
         || m_mode == ToolMode::ZoomWindow)
        && m_draftPoints.size() == 1) {
        painter.save();
        const bool printArea = m_mode == ToolMode::PrintArea;
        const QColor previewColor = printArea
            ? QColor(QStringLiteral("#e1b65a"))
            : QColor(QStringLiteral("#44ef9a"));
        const QRectF selectionRect =
            QRectF(worldToScreen(m_draftPoints.first()), m_displayScreen)
                .normalized();
        // Translucent wash so the drawing stays readable underneath.
        QColor fill = previewColor;
        fill.setAlpha(printArea ? 46 : 28);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRect(selectionRect);
        // Strong solid frame + corner handles: unmistakable feedback for
        // the area that will be printed.
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(previewColor, 1.8));
        painter.drawRect(selectionRect);
        painter.setPen(QPen(previewColor, 2.4));
        constexpr double kHandle = 9.0;
        const QPointF corners[4] = {
            selectionRect.topLeft(), selectionRect.topRight(),
            selectionRect.bottomLeft(), selectionRect.bottomRight()
        };
        for (const QPointF &corner : corners) {
            painter.drawLine(corner + QPointF(-kHandle, 0.0), corner);
            painter.drawLine(corner, corner + QPointF(0.0, kHandle));
            painter.drawLine(corner + QPointF(kHandle, 0.0), corner);
            painter.drawLine(corner, corner + QPointF(0.0, -kHandle));
        }
        // Full-canvas guide lines through the two edges for alignment.
        painter.setPen(QPen(QColor(previewColor.red(), previewColor.green(),
                                   previewColor.blue(), 90),
                            1.0, Qt::DotLine));
        painter.drawLine(QPointF(0.0, selectionRect.top()),
                         QPointF(width(), selectionRect.top()));
        painter.drawLine(QPointF(0.0, selectionRect.bottom()),
                         QPointF(width(), selectionRect.bottom()));
        painter.drawLine(QPointF(selectionRect.left(), 0.0),
                         QPointF(selectionRect.left(), height()));
        painter.drawLine(QPointF(selectionRect.right(), 0.0),
                         QPointF(selectionRect.right(), height()));
        if (printArea && m_document) {
            // Live sheet size in millimetres, following the cursor.
            const double widthMm =
                qAbs(m_cursorWorld.x() - m_draftPoints.first().x())
                * m_document->metersPerDrawingUnit / 0.001;
            const double heightMm =
                qAbs(m_cursorWorld.y() - m_draftPoints.first().y())
                * m_document->metersPerDrawingUnit / 0.001;
            const QString label = QStringLiteral("%1 × %2 mm")
                .arg(widthMm, 0, 'f', widthMm >= 100.0 ? 0 : 1)
                .arg(heightMm, 0, 'f', heightMm >= 100.0 ? 0 : 1);
            QFont labelFont = painter.font();
            labelFont.setPointSizeF(10.5);
            labelFont.setBold(true);
            painter.setFont(labelFont);
            const QFontMetrics metrics(labelFont);
            const QRectF textRect = metrics.boundingRect(label);
            const QPointF anchor(
                m_displayScreen.x() + 14.0,
                qBound(18.0, m_displayScreen.y() - 14.0,
                       double(height()) - 18.0));
            QRectF pill = textRect.adjusted(-9.0, -5.0, 9.0, 5.0);
            pill.moveTopLeft(anchor);
            pill = pill.intersected(QRectF(QPointF(0, 0),
                                           QSizeF(width(), height())));
            painter.setPen(QPen(previewColor, 1.2));
            painter.setBrush(QColor(16, 22, 26, 225));
            painter.drawRoundedRect(pill, 5.0, 5.0);
            painter.setPen(QColor(QStringLiteral("#f5f8fa")));
            painter.drawText(pill, Qt::AlignCenter, label);
        }
        painter.restore();
        return;
    }
    const int draftPointCount = m_draftPoints.size();
    bool includeProvisionalPoint = false;
    bool previewReady = false;
    if (!m_awaitingLabel) {
        switch (m_mode) {
        case ToolMode::Aligned:
        case ToolMode::Linear:
        case ToolMode::Batch:
        case ToolMode::Continuous:
        case ToolMode::Rectangle:
        case ToolMode::Polygon:
        case ToolMode::SideArea:
            previewReady = draftPointCount >= 1;
            includeProvisionalPoint = previewReady;
            break;
        case ToolMode::CircleArea:
        case ToolMode::Radius:
        case ToolMode::Angle:
        case ToolMode::ArcLength:
            previewReady = draftPointCount >= 2;
            includeProvisionalPoint = previewReady;
            break;
        default:
            break;
        }
    } else {
        previewReady = draftPointCount >= 2;
    }
    if (previewReady) {
        std::optional<Measurement> preview =
            draftMeasurement(
                m_awaitingLabel ? m_labelPreview : m_cursorWorld,
                includeProvisionalPoint);
        if (preview) {
            preview->color = QColor(QStringLiteral("#58a6d6"));
            preview->id = QStringLiteral("preview");
            m_drawingDraftPreview = true;
            drawMeasurement(painter, *preview, false);
            m_drawingDraftPreview = false;
            if (m_awaitingLabel) {
                m_committablePreview = *preview;
            }
        }
    }
    painter.setPen(QPen(QColor("#62b5df"), 1.2));
    painter.setBrush(canvasBackgroundColor());
    for (const QPointF &point : std::as_const(m_draftPoints)) {
        painter.drawEllipse(worldToScreen(point), 3.0, 3.0);
    }
    if (m_polygonClosurePrompted && !m_draftPoints.isEmpty()) {
        const QPointF first = worldToScreen(m_draftPoints.first());
        painter.setPen(QPen(QColor("#e1b65a"), 1.8));
        painter.setBrush(QColor(225, 182, 90, 45));
        painter.drawEllipse(first, 9.0, 9.0);
        painter.drawEllipse(first, 6.5, 6.5);
        QPainterPath closureArc;
        closureArc.arcMoveTo(
            QRectF(first.x() - 11.0, first.y() - 11.0, 22.0, 22.0),
            35.0);
        closureArc.arcTo(
            QRectF(first.x() - 11.0, first.y() - 11.0, 22.0, 22.0),
            35.0, 250.0);
        painter.drawPath(closureArc);
        drawArrow(painter, first + QPointF(8.6, -6.2),
                  first + QPointF(4.0, -10.0), QColor("#e1b65a"));
    }
    if (!m_draftPoints.isEmpty() && !m_awaitingLabel
        && !includeProvisionalPoint) {
        painter.setPen(QPen(QColor(98, 181, 223, 190), 1.0, Qt::DashLine));
        painter.drawLine(
            worldToScreen(m_draftPoints.last()),
            m_polygonClosurePrompted ? worldToScreen(m_draftPoints.first())
                                     : m_displayScreen);
    }
}

void CadView::drawEraseRectangle(QPainter &painter)
{
    if (!m_eraseDragging || !m_eraseScreenRect.isValid()) {
        return;
    }
    painter.save();
    painter.setPen(QPen(QColor("#ff6b6b"), 1.1, Qt::DashLine));
    painter.setBrush(QColor(255, 96, 96, 28));
    painter.drawRect(m_eraseScreenRect);
    painter.restore();
}

void CadView::drawDeleteHoverMarker(QPainter &painter)
{
    painter.save();
    const QPointF center = m_cursorScreen;
    painter.setPen(QPen(QColor("#ff625e"), 1.7));
    QColor deleteFill = canvasBackgroundColor();
    deleteFill.setAlpha(225);
    painter.setBrush(deleteFill);
    painter.drawEllipse(center, 8.0, 8.0);
    painter.drawLine(center + QPointF(-3.6, -3.6),
                     center + QPointF(3.6, 3.6));
    painter.drawLine(center + QPointF(-3.6, 3.6),
                     center + QPointF(3.6, -3.6));
    const auto found = m_labelHitRects.constFind(m_hoveredMeasurementId);
    if (found != m_labelHitRects.cend()) {
        painter.setPen(QPen(QColor(255, 98, 94, 190), 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(found.value(), 2.0, 2.0);
    }
    painter.restore();
}

void CadView::mousePressEvent(QMouseEvent *event)
{
    setFocus();
    m_lastMousePosition = event->pos();
    if (event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton && m_spaceDown)) {
        m_leftPressPending = false;
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::RightButton) {
        if (m_mode == ToolMode::Erase) {
            beginTool(ToolMode::Select);
            return;
        }
        if (m_mode == ToolMode::ZoomWindow) {
            beginTool(ToolMode::Select);
            return;
        }
        if (m_mode == ToolMode::PrintArea) {
            beginTool(ToolMode::Select);
            emit printAreaSelectionCanceled();
            return;
        }
        if (m_mode == ToolMode::Continuous || m_mode == ToolMode::Batch
            || m_mode == ToolMode::SideArea
            || m_mode == ToolMode::Polygon) {
            finishDraftForLabel();
            update();
        }
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }
    if (m_mode == ToolMode::Erase) {
        const QString hit = hitMeasurementAt(event->position());
        if (!hit.isEmpty()) {
            pushUndo();
            const int index = measurementIndex(hit);
            if (index >= 0) {
                m_measurements.removeAt(index);
            }
            m_hoveredMeasurementId.clear();
            m_selectedMeasurementId.clear();
            emit measurementsChanged();
            emit selectionChanged({});
            setCursor(Qt::BlankCursor);
            emit statusMessage(
                QStringLiteral("标注已删除，删除工具保持启用"));
            update();
            return;
        }
        m_eraseDragging = true;
        m_eraseStartScreen = event->position();
        m_eraseScreenRect = QRectF(m_eraseStartScreen, m_eraseStartScreen);
        update();
        return;
    }
    updateSnap(event->position());
    if (m_awaitingLabel) {
        m_labelPreview = m_cursorWorld;
    }
    if (m_mode == ToolMode::Select) {
        const QString hit = hitMeasurementLabel(event->position());
        if (!hit.isEmpty()) {
            pushUndo();
            selectMeasurement(hit);
            m_dragLabel = true;
            m_dragMeasurementId = hit;
            m_dragStartWorld = m_cursorWorld;
            setCursor(Qt::SizeAllCursor);
        } else {
            selectMeasurement({});
            m_leftPressPending = true;
            m_leftPressScreen = event->pos();
        }
        return;
    }
    m_leftPressPending = true;
    m_leftPressScreen = event->pos();
}

void CadView::leftClick(const QPointF &screenPoint)
{
    updateSnap(screenPoint);
    if ((m_mode == ToolMode::Continuous
         || m_mode == ToolMode::SideArea) && !m_awaitingLabel
        && m_draftPoints.size() >= 3
        && QLineF(
               worldToScreen(m_draftPoints.first()), screenPoint)
               .length()
            <= 12.0) {
        m_draftPoints.append(m_draftPoints.first());
        finishDraftForLabel();
        update();
        return;
    }
    if ((m_mode == ToolMode::Continuous
         || m_mode == ToolMode::SideArea) && !m_awaitingLabel
        && appendContinuousArcAt(screenPoint)) {
        return;
    }
    if (m_mode == ToolMode::Select) {
        return;
    }
    if (m_mode == ToolMode::FilledArea) {
        std::optional<Measurement> filled = closedEntityMeasurementAt(
            screenPoint, MeasurementType::FilledArea);
        if (filled) {
            filled->id = defaultMeasurementId();
            filled->space = m_activeSpace;
            filled->color = m_settings.annotationColor;
            filled->normalizeGeometry();
            pushUndo();
            m_selectedMeasurementId = filled->id;
            m_measurements.append(*filled);
            emit measurementsChanged();
            emit measurementAdded(filled->id);
            emit selectionChanged(filled->id);
            update();
        } else {
            return;
        }
        return;
    }
    if (m_mode == ToolMode::Text) {
        emit textAnnotationRequested(screenToWorld(screenPoint));
        return;
    }
    if (m_awaitingLabel) {
        commitDraft(screenToWorld(screenPoint));
        return;
    }
    if (m_mode == ToolMode::Polygon && m_draftPoints.size() >= 3
        && QLineF(worldToScreen(m_draftPoints.first()), screenPoint).length()
            <= 12.0) {
        finishDraftForLabel();
        return;
    }
    appendDraftPoint(m_cursorWorld);
    const int count = m_draftPoints.size();
    if (m_mode == ToolMode::PrintArea && count >= 2) {
        const QRectF worldArea(m_draftPoints[0], m_draftPoints[1]);
        clearDraft();
        emit printAreaSelected(worldArea.normalized());
        return;
    }
    if (m_mode == ToolMode::ZoomWindow && count >= 2) {
        const QRectF worldArea =
            QRectF(m_draftPoints[0], m_draftPoints[1]).normalized();
        clearDraft();
        if (worldArea.width() > 1.0e-9
            && worldArea.height() > 1.0e-9) {
            zoomToWorldRect(worldArea);
        }
        beginTool(ToolMode::Select);
        return;
    }
    if (m_mode == ToolMode::Calibrate && count >= 2) {
        m_pendingCalibrationRawLength =
            QLineF(m_draftPoints[0], m_draftPoints[1]).length();
        m_calibrationPoints = {
            m_draftPoints[0], m_draftPoints[1]
        };
        emit calibrationRequested(m_pendingCalibrationRawLength);
        return;
    }
    const bool ready = ((m_mode == ToolMode::Aligned
                         || m_mode == ToolMode::Linear)
                        && count >= 2)
        || (m_mode == ToolMode::Rectangle && count >= 2)
        || (m_mode == ToolMode::CircleArea && count >= 3)
        || (m_mode == ToolMode::Radius && count >= 3)
        || (m_mode == ToolMode::Angle && count >= 3)
        || (m_mode == ToolMode::ArcLength && count >= 3)
        ;
    if (ready) {
        finishDraftForLabel();
    }
    update();
}

void CadView::appendDraftPoint(const QPointF &worldPoint)
{
    if (!m_draftPoints.isEmpty()
        && QLineF(m_draftPoints.last(), worldPoint).length()
            < snapRadiusWorld() * 0.08) {
        return;
    }
    if ((m_mode == ToolMode::Radius
         || m_mode == ToolMode::CircleArea)
        && m_draftPoints.size() == 2
        && !circleCenterFromThreePoints(
            m_draftPoints[0], m_draftPoints[1], worldPoint)) {
        emit statusMessage(
            QStringLiteral("三点共线，无法确定圆心，请重新选择第三点"));
        update();
        return;
    }
    m_draftPoints.append(worldPoint);
}

void CadView::finishDraftForLabel()
{
    bool valid = false;
    switch (m_mode) {
    case ToolMode::Continuous:
    case ToolMode::SideArea:
    case ToolMode::Batch:
        valid = m_draftPoints.size() >= 2;
        break;
    case ToolMode::Polygon:
        valid = m_draftPoints.size() >= 3;
        break;
    default:
        valid = m_draftPoints.size() >= 2;
        break;
    }
    if (!valid) {
        return;
    }
    m_awaitingLabel = true;
    m_polygonClosurePrompted = false;
    m_labelPreview = m_cursorWorld;
    m_committablePreview.reset();
    emit statusMessage(toolPrompt());
    update();
}

bool CadView::commitDraft(const QPointF &labelWorld)
{
    QPointF resolvedLabel = labelWorld;
    if (m_mode == ToolMode::Calibrate) {
        if (m_draftPoints.size() == 2) {
            m_calibrationPoints = {
                m_draftPoints[0], m_draftPoints[1]
            };
            m_pendingCalibrationRawLength =
                QLineF(m_draftPoints[0], m_draftPoints[1]).length();
            emit calibrationRequested(m_pendingCalibrationRawLength);
        }
        return false;
    }
    if (m_mode == ToolMode::Batch) {
        if (m_draftPoints.size() < 2) {
            return false;
        }
        pushUndo();
        const QVector<QPointF> nodes = m_draftPoints;
        QString lastId;
        for (int i = 1; i < nodes.size(); ++i) {
            Measurement segment;
            segment.id = defaultMeasurementId();
            segment.space = m_activeSpace;
            segment.type = MeasurementType::Linear;
            segment.points = {nodes[i - 1], nodes[i]};
            segment.auxiliaryValue =
                qAbs(nodes[i].x() - nodes[i - 1].x())
                    >= qAbs(nodes[i].y() - nodes[i - 1].y())
                ? 1.0
                : 2.0;
            const QPointF midpoint =
                (nodes[i - 1] + nodes[i]) * 0.5;
            if (segment.auxiliaryValue == 1.0) {
                // All horizontal batch dimensions share one baseline.
                segment.labelPosition =
                    QPointF(midpoint.x(), labelWorld.y());
            } else {
                // All vertical batch dimensions share one column.
                segment.labelPosition =
                    QPointF(labelWorld.x(), midpoint.y());
            }
            segment.color = m_settings.annotationColor;
            segment.normalizeGeometry();
            m_measurements.append(segment);
            lastId = segment.id;
        }
        m_selectedMeasurementId = lastId;
        clearDraft();
        emit measurementsChanged();
        if (!lastId.isEmpty()) {
            emit measurementAdded(lastId);
            emit selectionChanged(lastId);
        }
        emit statusMessage(toolPrompt());
        update();
        return true;
    }
    std::optional<Measurement> measurement =
        m_committablePreview
        ? m_committablePreview
        : draftMeasurement(resolvedLabel);
    if (!measurement) {
        return false;
    }
    measurement->id = defaultMeasurementId();
    measurement->space = m_activeSpace;
    measurement->color = m_settings.annotationColor;
    // The click point is authoritative. Never reuse the preview's previous
    // label anchor, which may have been produced before the final pointer move.
    measurement->labelPosition = resolvedLabel;
    measurement->normalizeGeometry();
    // Preserve the point chosen after the boundary is complete. Area
    // measurements previously snapped this back to the polygon centroid,
    // which prevented placing the label outside a crowded room.
    pushUndo();
    m_measurements.append(*measurement);
    m_selectedMeasurementId = measurement->id;
    clearDraft();
    emit measurementsChanged();
    emit measurementAdded(measurement->id);
    emit selectionChanged(measurement->id);
    emit statusMessage(toolPrompt());
    update();
    return true;
}

void CadView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint delta = event->pos() - m_lastMousePosition;
    m_lastMousePosition = event->pos();
    m_haveCursor = true;
    if (m_leftPressPending && !m_panning
        && (event->buttons() & Qt::LeftButton)
        && QLineF(QPointF(m_leftPressScreen), event->position()).length()
            >= QApplication::startDragDistance()) {
        m_panning = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (m_panning) {
        m_center += QPointF(-delta.x(), delta.y()) / m_scale;
        m_cursorWorld = screenToWorld(event->position());
        m_cursorScreen = event->position();
        emit viewTransformChanged();
        requestRenderFrame();
        return;
    }
    if (m_dragLabel && !m_dragMeasurementId.isEmpty()) {
        const int index = measurementIndex(m_dragMeasurementId);
        if (index >= 0) {
            m_cursorWorld = screenToWorld(event->position());
            m_measurements[index].labelPosition = m_cursorWorld;
            requestRenderFrame();
        }
        return;
    }
    if (m_eraseDragging) {
        m_eraseScreenRect =
            QRectF(m_eraseStartScreen, event->position()).normalized();
        requestRenderFrame();
        return;
    }
    updateSnap(event->position());
    if (m_mode == ToolMode::Erase) {
        const QString hovered =
            hitMeasurementAt(event->position());
        if (hovered != m_hoveredMeasurementId) {
            m_hoveredMeasurementId = hovered;
            setCursor(hovered.isEmpty() ? Qt::BlankCursor
                                        : Qt::PointingHandCursor);
        }
    } else if (!m_hoveredMeasurementId.isEmpty()) {
        m_hoveredMeasurementId.clear();
        setCursor(m_spaceDown ? Qt::OpenHandCursor
                              : Qt::BlankCursor);
    }
    // The CAD crosshair is drawn on the canvas in every idle tool state,
    // including selection, so it must follow every pointer movement.
    requestRenderFrame();
}

void CadView::enterEvent(QEnterEvent *event)
{
    m_haveCursor = true;
    m_cursorScreen = event->position();
    updateSnap(m_cursorScreen);
    if (!m_panning) {
        setCursor(m_spaceDown ? Qt::OpenHandCursor
                              : Qt::BlankCursor);
    }
    requestRenderFrame();
    QOpenGLWidget::enterEvent(event);
}

void CadView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_leftPressPending && event->button() == Qt::LeftButton) {
        const bool wasPanning = m_panning;
        m_leftPressPending = false;
        if (wasPanning) {
            m_panning = false;
            invalidateSceneCache();
            setCursor(m_spaceDown ? Qt::OpenHandCursor
                                  : Qt::BlankCursor);
            scheduleSceneRefresh();
            return;
        }
        leftClick(event->position());
        return;
    }
    if (m_panning
        && (event->button() == Qt::MiddleButton
            || event->button() == Qt::LeftButton)) {
        m_panning = false;
        invalidateSceneCache();
        setCursor(m_spaceDown ? Qt::OpenHandCursor
                              : Qt::BlankCursor);
        scheduleSceneRefresh();
        return;
    }
    if (m_dragLabel && event->button() == Qt::LeftButton) {
        m_dragLabel = false;
        m_dragMeasurementId.clear();
        m_draftPoints.clear();
        emit measurementsChanged();
        setCursor(Qt::BlankCursor);
        update();
        return;
    }
    if (m_eraseDragging && event->button() == Qt::LeftButton) {
        m_eraseDragging = false;
        QVector<QString> ids;
        if (QLineF(m_eraseStartScreen, event->position()).length() < 4.0) {
            const QString hit = hitMeasurementLabel(event->position());
            if (!hit.isEmpty()) {
                ids.append(hit);
            }
        } else {
            for (auto it = m_labelHitRects.cbegin();
                 it != m_labelHitRects.cend(); ++it) {
                if (m_eraseScreenRect.intersects(it.value())) {
                    ids.append(it.key());
                }
            }
            for (auto it = m_measurementHitSegments.cbegin();
                 it != m_measurementHitSegments.cend(); ++it) {
                if (ids.contains(it.key())) {
                    continue;
                }
                bool intersects = false;
                for (const QLineF &line : it.value()) {
                    const QRectF segmentRect(line.p1(), line.p2());
                    if (m_eraseScreenRect.intersects(
                            segmentRect.normalized().adjusted(
                                -3.0, -3.0, 3.0, 3.0))) {
                        intersects = true;
                        break;
                    }
                }
                if (intersects) {
                    ids.append(it.key());
                }
            }
        }
        if (!ids.isEmpty()) {
            pushUndo();
            for (const QString &id : std::as_const(ids)) {
                const int index = measurementIndex(id);
                if (index >= 0) {
                    m_measurements.removeAt(index);
                }
            }
            emit measurementsChanged();
            m_selectedMeasurementId.clear();
            emit selectionChanged({});
        }
        m_eraseScreenRect = QRectF();
        m_hoveredMeasurementId.clear();
        if (!ids.isEmpty()) {
            setCursor(Qt::BlankCursor);
            emit statusMessage(
                QStringLiteral("标注已删除，删除工具保持启用"));
            update();
            return;
        }
        update();
    }
}

void CadView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton) {
        fitToDrawing();
        return;
    }
    if (event->button() == Qt::LeftButton
        && (m_mode == ToolMode::Continuous || m_mode == ToolMode::Batch
            || m_mode == ToolMode::SideArea
            || m_mode == ToolMode::Polygon)
        && !m_awaitingLabel) {
        finishDraftForLabel();
    }
}

void CadView::wheelEvent(QWheelEvent *event)
{
    double steps = event->angleDelta().y() / 120.0;
    if (qAbs(steps) < 0.01 && !event->pixelDelta().isNull()) {
        steps = event->pixelDelta().y() / 32.0;
    }
    if (qAbs(steps) > 0.001) {
        zoomAt(std::pow(1.18, steps), event->position().toPoint());
        // 视图变换后重新投影光标，让十字/捕捉点立即对齐新视图。
        updateSnap(m_cursorScreen);
    }
    event->accept();
}

bool CadView::event(QEvent *event)
{
    if (event->type() == QEvent::NativeGesture) {
        auto *gesture = static_cast<QNativeGestureEvent *>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            const double factor = qBound(0.75, 1.0 + gesture->value(), 1.35);
            zoomAt(factor, gesture->position().toPoint());
            updateSnap(m_cursorScreen);
            event->accept();
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

void CadView::keyPressEvent(QKeyEvent *event)
{
    // 直接敲字母/数字 → 转投命令行（AutoCAD 习惯），空格保留给拖拽平移
    if ((event->modifiers()
         & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            == Qt::NoModifier
        && event->key() != Qt::Key_Space && event->key() != Qt::Key_unknown
        && !event->text().isEmpty()) {
        const QChar typed = event->text().at(0);
        if (typed.isPrint() && typed.unicode() < 128) {
            emit commandTextEntered(event->text());
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Escape) {
        if (!m_awaitingLabel
            && (m_mode == ToolMode::Continuous
                || m_mode == ToolMode::SideArea
                || m_mode == ToolMode::Batch
                || m_mode == ToolMode::Polygon)
            && (m_mode == ToolMode::Polygon
                ? m_draftPoints.size() >= 3
                : m_draftPoints.size() >= 2)) {
            finishDraftForLabel();
            return;
        }
        if (!m_draftPoints.isEmpty() || m_awaitingLabel) {
            cancelCurrentTool();
        } else if (m_mode != ToolMode::Select) {
            const bool canceledPrintSelection =
                m_mode == ToolMode::PrintArea;
            beginTool(ToolMode::Select);
            if (canceledPrintSelection) {
                emit printAreaSelectionCanceled();
            }
        }
        m_hoveredMeasurementId.clear();
        return;
    }
    if (event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        if ((m_mode == ToolMode::Continuous
             || m_mode == ToolMode::SideArea
             || m_mode == ToolMode::Batch
             || m_mode == ToolMode::Polygon)
            && !m_awaitingLabel) {
            finishDraftForLabel();
            return;
        }
    }
    if (event->key() == Qt::Key_Delete) {
        if (!m_selectedMeasurementId.isEmpty()) {
            removeMeasurement(m_selectedMeasurementId);
            beginTool(ToolMode::Select);
        }
        return;
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceDown = true;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void CadView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceDown = false;
        if (!m_panning) {
            setCursor(Qt::BlankCursor);
        }
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

void CadView::leaveEvent(QEvent *event)
{
    m_haveCursor = false;
    m_snap = {};
    emit snapChanged({});
    requestRenderFrame();
    QOpenGLWidget::leaveEvent(event);
}

QString CadView::hitMeasurementLabel(const QPointF &screenPoint) const
{
    for (auto it = m_labelHitRects.cbegin(); it != m_labelHitRects.cend();
         ++it) {
        if (it.value().contains(screenPoint)) {
            return it.key();
        }
    }
    return {};
}

QString CadView::hitMeasurementAt(const QPointF &screenPoint) const
{
    const bool currentScreenCache =
        m_vectorCacheValid
        && m_vectorCacheCenter == m_center
        && qFuzzyCompare(
            1.0 + m_vectorCacheScale, 1.0 + m_scale);
    for (int index = m_measurements.size() - 1; index >= 0; --index) {
        const Measurement &measurement = m_measurements[index];
        if (!measurement.visible || measurement.space != m_activeSpace) {
            continue;
        }
        const auto boundsHit =
            m_measurementHitBounds.constFind(measurement.id);
        if (currentScreenCache
            && boundsHit != m_measurementHitBounds.cend()
            && !boundsHit.value().contains(screenPoint)) {
            continue;
        }
        const auto labelHit = m_labelHitRects.constFind(measurement.id);
        if (labelHit != m_labelHitRects.cend()
            && labelHit.value().contains(screenPoint)) {
            return measurement.id;
        }
        const auto segmentHits =
            m_measurementHitSegments.constFind(measurement.id);
        if (segmentHits != m_measurementHitSegments.cend()) {
            for (const QLineF &line : segmentHits.value()) {
                if (distanceToSegment(line, screenPoint) <= 7.0) {
                    return measurement.id;
                }
            }
        }
        QVector<QPointF> points;
        points.reserve(measurement.points.size());
        for (const QPointF &point : measurement.points) {
            points.append(worldToScreen(point));
        }
        for (const QPointF &point : std::as_const(points)) {
            if (QLineF(point, screenPoint).length() <= 7.0) {
                return measurement.id;
            }
        }
        const bool closedArea =
            measurement.type == MeasurementType::Rectangle
            || measurement.type == MeasurementType::Polygon
            || measurement.type == MeasurementType::FilledArea;
        if (closedArea && points.size() >= 3
            && QPolygonF(points).containsPoint(screenPoint,
                                               Qt::OddEvenFill)) {
            return measurement.id;
        }
        for (int i = 1; i < points.size(); ++i) {
            if (distanceToSegment(QLineF(points[i - 1], points[i]),
                                  screenPoint)
                <= 7.0) {
                return measurement.id;
            }
        }
        const QPointF labelPosition =
            worldToScreen(measurement.labelPosition);
        if (measurement.type == MeasurementType::Aligned
            && points.size() >= 2) {
            QPointF normal(-(points[1].y() - points[0].y()),
                           points[1].x() - points[0].x());
            const double normalLength =
                std::hypot(normal.x(), normal.y());
            if (normalLength > 1.0e-9) {
                normal /= normalLength;
                const double offset =
                    QPointF::dotProduct(labelPosition - points[0], normal);
                if (distanceToSegment(
                        QLineF(points[0] + normal * offset,
                               points[1] + normal * offset),
                        screenPoint)
                    <= 7.0) {
                    return measurement.id;
                }
            }
        }
        if (measurement.type == MeasurementType::Linear
            && points.size() >= 2) {
            QPointF a = points[0];
            QPointF b = points[1];
            if (measurement.auxiliaryValue == 1.0) {
                a.setY(labelPosition.y());
                b.setY(labelPosition.y());
            } else {
                a.setX(labelPosition.x());
                b.setX(labelPosition.x());
            }
            if (distanceToSegment(QLineF(a, b), screenPoint) <= 7.0) {
                return measurement.id;
            }
        }
        if (measurement.type == MeasurementType::Rectangle
            && points.size() >= 4) {
            const AnnotationStyleMetrics styleMetrics =
                annotationStyleMetrics(m_settings.annotationStyle);
            const double geometryScale = annotationGeometryScale();
            const double offsetPixels = qMax(
                18.0,
                styleMetrics.textGap * geometryScale
                    + 13.0 * geometryScale);
            QPointF centroid;
            for (const QPointF &point : std::as_const(points)) {
                centroid += point;
            }
            centroid /= double(points.size());
            int horizontalIndex = -1;
            int verticalIndex = -1;
            double longestHorizontal = 0.0;
            double longestVertical = 0.0;
            for (int edge = 0; edge < 4; ++edge) {
                const QPointF start = points[edge];
                const QPointF end = points[(edge + 1) % 4];
                const double horizontal = qAbs(end.x() - start.x());
                const double vertical = qAbs(end.y() - start.y());
                if (horizontal >= vertical
                    && horizontal > longestHorizontal) {
                    longestHorizontal = horizontal;
                    horizontalIndex = edge;
                }
                if (vertical >= horizontal
                    && vertical > longestVertical) {
                    longestVertical = vertical;
                    verticalIndex = edge;
                }
            }
            const auto hitsOffsetDimension =
                [&](int edge) {
                    if (edge < 0) {
                        return false;
                    }
                    const QPointF start = points[edge];
                    const QPointF end = points[(edge + 1) % 4];
                    QPointF direction = end - start;
                    const double length =
                        std::hypot(direction.x(), direction.y());
                    if (length <= 1.0e-6) {
                        return false;
                    }
                    direction /= length;
                    QPointF normal(-direction.y(), direction.x());
                    const QPointF mid = (start + end) * 0.5;
                    if (QPointF::dotProduct(
                            mid - centroid, normal) < 0.0) {
                        normal = -normal;
                    }
                    return distanceToSegment(
                               QLineF(
                                   start + normal * offsetPixels,
                                   end + normal * offsetPixels),
                               screenPoint)
                        <= 7.0;
                };
            if (hitsOffsetDimension(horizontalIndex)
                || hitsOffsetDimension(verticalIndex)) {
                return measurement.id;
            }
        }
        if (measurement.type == MeasurementType::Angle
            && points.size() >= 3
            && (distanceToSegment(QLineF(points[0], points[1]),
                                  screenPoint)
                    <= 7.0
                || distanceToSegment(QLineF(points[0], points[2]),
                                     screenPoint)
                       <= 7.0)) {
            return measurement.id;
        }
        if ((measurement.type == MeasurementType::Radius
             || measurement.type == MeasurementType::CircleArea)
            && points.size() >= 2) {
            const double radius = QLineF(points[0], points[1]).length();
            const double cursorRadius =
                QLineF(points[0], screenPoint).length();
            if (qAbs(cursorRadius - radius) <= 7.0) {
                return measurement.id;
            }
        }
    }
    return {};
}

QVector<CadEntity> CadView::buildLayoutContent(const QString &name) const
{
    if (!m_document) {
        return {};
    }
    return composeLayoutContent(*m_document, m_modelEntities, name);
}

QVector<CadEntity> CadView::composeLayoutContent(
    const CadDocument &document,
    const QVector<CadEntity> &modelEntities,
    const QString &name)
{
    QVector<CadEntity> content = document.layoutEntities.value(name);
    const auto viewports =
        document.layoutViewports.value(name);
    content.reserve(content.size() + modelEntities.size());
    // Model entities are duplicated once per viewport, but their hatchGroup
    // id was copied unchanged. paintHatches() groups purely by that id, so
    // copies from *different* viewports were merged into one QPainterPath
    // and clipped with only the first copy's viewport rectangle. That puts
    // a fill in the wrong viewport. Remap ids per viewport so every copy
    // stays an independent, correctly clipped group while boundary loops
    // of the same HATCH still share an id (odd-even islands keep working).
    int nextGroupId = 0;
    qsizetype totalFrozenSkipped = 0;
    qsizetype frozenByIndex = 0;
    qsizetype frozenByName = 0;
    qsizetype entitiesWithoutLayerIndex = 0;
    for (const CadEntity &existing : std::as_const(content)) {
        nextGroupId = qMax(nextGroupId, existing.hatchGroup + 1);
    }
    const bool dbgVp4 =
        qEnvironmentVariableIsSet("CADLENS_DEBUG_VP4");
    QHash<int, QHash<QString, int>> vp4DrawnByLayer;
    int vpIndex = 0;
    for (const CadViewportDefinition &viewport : viewports) {
        const int thisVp = vpIndex++;
        if (!viewport.revealsModel
            || viewport.viewHeight <= 1.0e-12
            || viewport.windowSize.width() <= 1.0e-9
            || viewport.windowSize.height() <= 1.0e-9) {
            continue;
        }
        const double viewScale =
            viewport.windowSize.height() / viewport.viewHeight;
        const QRectF clip(
            viewport.center
                - QPointF(viewport.windowSize.width() * 0.5,
                          viewport.windowSize.height() * 0.5),
            viewport.windowSize);
        const double cosine = std::cos(viewport.twist);
        const double sine = std::sin(viewport.twist);
        // The visible model region is the paper window rotated back by
        // -twist about the model center. Cull against that region's
        // bounding box; using the unrotated paper aspect drops entities
        // near the edges of every twisted viewport.
        const double halfModelWidth =
            viewport.windowSize.width() * 0.5 / viewScale;
        const double halfModelHeight = viewport.viewHeight * 0.5;
        const double extentX =
            std::abs(halfModelWidth * cosine)
            + std::abs(halfModelHeight * sine);
        const double extentY =
            std::abs(halfModelWidth * sine)
            + std::abs(halfModelHeight * cosine);
        const QRectF modelWindow(
            viewport.modelCenter - QPointF(extentX, extentY),
            QSizeF(extentX * 2.0, extentY * 2.0));
        const auto mapPoint = [&](const QPointF &source) {
            const QPointF delta =
                (source - viewport.modelCenter) * viewScale;
            return viewport.center
                + QPointF(delta.x() * cosine - delta.y() * sine,
                          delta.x() * sine + delta.y() * cosine);
        };
        QHash<int, int> groupRemap;
        for (const CadEntity &source : modelEntities) {
            // VPLAYER freezes a *layer object*, not a layer name. Match by
            // the resolved index whenever we have it; fall back to the name
            // only for documents restored from the serialized cache (where
            // the indices were not persisted) or entities without an index.
            const bool hasIndex = source.layerIndex >= 0
                && !viewport.frozenLayerIndices.isEmpty();
            if (source.layerIndex < 0) {
                ++entitiesWithoutLayerIndex;
            }
            // A layer turned off at the layer table must stay hidden in
            // every viewport, independent of per-viewport freeze. Without
            // this gate, globally-off layers (e.g. $TD_AUDIT_GENERATED_*
            // junk swept up by AUDIT) still render inside viewports.
            if (source.layerIndex >= 0
                && source.layerIndex < document.layers.size()
                && !document.layers[source.layerIndex].visible) {
                continue;
            }
            bool frozen = hasIndex
                ? viewport.frozenLayerIndices.contains(source.layerIndex)
                : viewport.frozenLayers.contains(source.layer);
            if (!frozen) {
                if (!source.blockLayerIndices.isEmpty()
                    && !viewport.frozenLayerIndices.isEmpty()) {
                    for (int blockLayer : source.blockLayerIndices) {
                        if (viewport.frozenLayerIndices.contains(
                                blockLayer)) {
                            frozen = true;
                            break;
                        }
                    }
                } else if (!source.blockLayerNames.isEmpty()) {
                    for (const QString &blockLayer :
                         std::as_const(source.blockLayerNames)) {
                        if (viewport.frozenLayers.contains(blockLayer)) {
                            frozen = true;
                            break;
                        }
                    }
                }
            }
            if (frozen) {
                ++totalFrozenSkipped;
                if (hasIndex) {
                    ++frozenByIndex;
                } else {
                    ++frozenByName;
                }
                continue;
            }
            if (dbgVp4
                && (thisVp == 4 || thisVp == 6 || thisVp == 7)
                && source.layerIndex >= 0
                && source.layerIndex < document.layers.size()) {
                vp4DrawnByLayer[thisVp]
                    [document.layers[source.layerIndex].name]++;
            }
            if (source.bounds.isValid()
                && !source.bounds.intersects(modelWindow)) {
                continue;
            }
            CadEntity entity = source;
            if (entity.hatchGroup >= 0) {
                auto remapped =
                    groupRemap.constFind(entity.hatchGroup);
                if (remapped == groupRemap.cend()) {
                    groupRemap.insert(entity.hatchGroup, nextGroupId);
                    entity.hatchGroup = nextGroupId++;
                } else {
                    entity.hatchGroup = remapped.value();
                }
            }
            for (QPointF &point : entity.points) {
                point = mapPoint(point);
            }
            entity.center = mapPoint(entity.center);
            entity.textPosition = mapPoint(entity.textPosition);
            entity.textAlignmentPoint =
                mapPoint(entity.textAlignmentPoint);
            entity.radius *= viewScale;
            entity.textHeight *= viewScale;
            entity.textRotation +=
                qRadiansToDegrees(viewport.twist);
            for (CadDimensionEdge &edge : entity.dimensionEdges) {
                edge.midpoint = mapPoint(edge.midpoint);
                edge.length *= viewScale;
            }
            if (entity.hatchPattern) {
                auto pattern = std::make_shared<CadHatchPattern>(
                    *entity.hatchPattern);
                for (CadHatchPatternLine &line : pattern->lines) {
                    const QPointF origin = line.origin;
                    line.direction =
                        mapPoint(origin + line.direction)
                        - mapPoint(origin);
                    line.offset =
                        mapPoint(origin + line.offset)
                        - mapPoint(origin);
                    line.origin = mapPoint(origin);
                }
                entity.hatchPattern = std::move(pattern);
            }
            entity.clipRect = clip;
            entity.updateBounds();
            content.append(std::move(entity));
        }
        if (dbgVp4 && (thisVp == 4 || thisVp == 6 || thisVp == 7)) {
            std::fprintf(
                stderr,
                "[vpdraw %d] drawn entities by layer (after freeze):\n",
                thisVp);
            auto layerIt = vp4DrawnByLayer.constFind(thisVp);
            if (layerIt != vp4DrawnByLayer.cend()) {
                for (auto lit = layerIt.value().constBegin();
                     lit != layerIt.value().constEnd(); ++lit) {
                    std::fprintf(stderr, "    %6d  %s\n", lit.value(),
                                 lit.key().toUtf8().constData());
                }
            }
        }
    }
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        // A HATCH group id is copied verbatim from model space into every
        // viewport. If the same id ends up under two different viewport
        // clip rectangles the renderer merges all those boundary loops into
        // one fill path with only the first clip applied, which can put a
        // fill in the wrong viewport. Report how often that happens.
        QHash<int, QSet<QString>> groupClips;
        for (const CadEntity &entity : std::as_const(content)) {
            if (entity.hatchGroup >= 0 && entity.clipRect.isValid()) {
                groupClips[entity.hatchGroup].insert(
                    QString::number(entity.clipRect.left(), 'f', 3)
                    + QLatin1Char(',')
                    + QString::number(entity.clipRect.top(), 'f', 3));
            }
        }
        int crossViewportGroups = 0;
        for (auto it = groupClips.constBegin();
             it != groupClips.constEnd(); ++it) {
            if (it.value().size() > 1) {
                ++crossViewportGroups;
            }
        }
        std::fprintf(stderr,
                     "layout=%s viewports=%lld content=%lld "
                     "hatchGroups=%d crossViewportGroups=%d "
                     "frozenSkipped=%lld byIndex=%lld byName=%lld "
                     "noLayerIndex=%lld\n",
                     name.toUtf8().constData(),
                     static_cast<long long>(viewports.size()),
                     static_cast<long long>(content.size()),
                     int(groupClips.size()), crossViewportGroups,
                     static_cast<long long>(totalFrozenSkipped),
                     static_cast<long long>(frozenByIndex),
                     static_cast<long long>(frozenByName),
                     static_cast<long long>(entitiesWithoutLayerIndex));
            if (dbgVp4 && !vp4DrawnByLayer.isEmpty()) {
                std::fprintf(stderr, "[vp4drawn] top layers:\n");
                QHash<QString, int> drawnTotals;
                for (auto viewportIt = vp4DrawnByLayer.constBegin();
                     viewportIt != vp4DrawnByLayer.constEnd();
                     ++viewportIt) {
                    for (auto layerIt = viewportIt.value().constBegin();
                         layerIt != viewportIt.value().constEnd();
                         ++layerIt) {
                        drawnTotals[layerIt.key()] += layerIt.value();
                    }
                }
                auto items = drawnTotals.keys();
                std::sort(items.begin(), items.end(),
                          [&](const QString &a, const QString &b) {
                              return drawnTotals.value(a)
                                     > drawnTotals.value(b);
                          });
                for (const QString &k : items) {
                    if (drawnTotals.value(k) >= 3) {
                        std::fprintf(stderr, "   %s : %d\n",
                                     k.toUtf8().constData(),
                                     drawnTotals.value(k));
                    }
                }
            }
    }
    return content;
}

QHash<QString, QVector<CadEntity>>
CadView::prepareAllLayoutContents(const CadDocument &document)
{
    QHash<QString, QVector<CadEntity>> prepared;
    for (const QString &layout : document.layouts) {
        prepared.insert(
            layout,
            composeLayoutContent(document, document.entities, layout));
    }
    return prepared;
}

int CadView::arcEntityIndexAt(const QPointF &screenPoint) const
{
    if (!m_document) {
        return -1;
    }
    const QPointF worldPoint = screenToWorld(screenPoint);
    const double tolerance =
        10.0 / qMax(1.0e-12, m_scale);
    const QVector<int> candidates = m_spatialIndex.query(
        QRectF(
            worldPoint.x() - tolerance,
            worldPoint.y() - tolerance,
            tolerance * 2.0, tolerance * 2.0),
        512);
    int bestIndex = -1;
    double bestDistance = 10.0;
    for (int index : candidates) {
        const CadEntity &entity = m_document->entities[index];
        const CadLayer *layer = m_document->layerOf(entity);
        if ((layer && !layer->visible)
            || !isRenderableCadEntity(entity)
            || entity.type != CadEntityType::Arc
            || entity.points.size() < 2
            || (entity.clipRect.isValid()
                && !entity.clipRect.contains(worldPoint))) {
            continue;
        }
        for (qsizetype pointIndex = 1;
             pointIndex < entity.points.size(); ++pointIndex) {
            const double distance = distanceToSegment(
                QLineF(entity.points[pointIndex - 1],
                       entity.points[pointIndex]),
                worldPoint) * m_scale;
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = index;
            }
        }
    }
    return bestIndex;
}

bool CadView::appendContinuousArcAt(const QPointF &screenPoint)
{
    if (!m_document) {
        return false;
    }
    const int index = arcEntityIndexAt(screenPoint);
    if (index < 0) {
        return false;
    }
    if (m_usedContinuousArcIndices.contains(index)) {
        emit statusMessage(
            QStringLiteral("该圆弧已在当前连续测量中，不能重复选择"));
        return true;
    }
    QVector<QPointF> path = m_document->entities[index].points;
    if (path.size() < 2) {
        return false;
    }
    if (!m_draftPoints.isEmpty()
        && QLineF(m_draftPoints.last(), path.last()).length()
            < QLineF(m_draftPoints.last(), path.first()).length()) {
        std::reverse(path.begin(), path.end());
    }
    if (m_draftPoints.isEmpty()) {
        m_draftPoints.append(path.first());
    } else if (QLineF(m_draftPoints.last(), path.first()).length()
               > snapRadiusWorld() * 0.08) {
        m_draftPoints.append(path.first());
    }
    if (QLineF(m_draftPoints.last(), path.last()).length()
        > snapRadiusWorld() * 0.08) {
        m_draftPoints.append(path.last());
    }
    m_usedContinuousArcIndices.insert(index);
    m_cursorWorld = path.last();
    m_cursorScreen = worldToScreen(path.last());
    m_displayScreen = m_cursorScreen;
    const bool closed =
        m_draftPoints.size() >= 3
        && QLineF(
               worldToScreen(m_draftPoints.first()),
               m_cursorScreen)
               .length()
            <= 12.0;
    if (closed) {
        finishDraftForLabel();
    } else {
        emit statusMessage(
            QStringLiteral("已选择圆弧，继续点击线段或圆弧"));
    }
    update();
    return true;
}

std::optional<QLineF> CadView::circularRadiusAt(
    const QPointF &screenPoint) const
{
    if (!m_document) {
        return std::nullopt;
    }
    const QPointF worldPoint = screenToWorld(screenPoint);
    const double tolerance = 10.0 / qMax(1.0e-12, m_scale);
    const QVector<int> candidates = m_spatialIndex.query(
        QRectF(worldPoint.x() - tolerance, worldPoint.y() - tolerance,
               tolerance * 2.0, tolerance * 2.0),
        1024);
    double bestDistance = 10.0;
    QLineF bestRadius;
    bool found = false;
    for (int index : candidates) {
        const CadEntity &entity = m_document->entities[index];
        const CadLayer *layer = m_document->layerOf(entity);
        if ((layer && !layer->visible)
            || (entity.type != CadEntityType::Circle
                && entity.type != CadEntityType::Arc)
            || entity.radius <= 1.0e-9
            || entity.points.size() < 2) {
            continue;
        }
        const int segmentCount =
            entity.points.size() - (entity.closed ? 0 : 1);
        double entityDistance = std::numeric_limits<double>::max();
        for (int i = 0; i < segmentCount; ++i) {
            const QLineF segment(
                worldToScreen(entity.points[i]),
                worldToScreen(entity.points[(i + 1) % entity.points.size()]));
            entityDistance = qMin(
                entityDistance, distanceToSegment(segment, screenPoint));
        }
        if (entityDistance > bestDistance) {
            continue;
        }
        QPointF direction = worldPoint - entity.center;
        const double length = std::hypot(direction.x(), direction.y());
        direction = length > 1.0e-9
            ? direction / length : QPointF(1.0, 0.0);
        bestRadius = QLineF(
            entity.center, entity.center + direction * entity.radius);
        bestDistance = entityDistance;
        found = true;
    }
    return found ? std::optional<QLineF>(bestRadius) : std::nullopt;
}

std::optional<Measurement> CadView::closedEntityMeasurementAt(
    const QPointF &screenPoint, MeasurementType type) const
{
    if (!m_document) {
        return std::nullopt;
    }
    const QPointF worldPoint = screenToWorld(screenPoint);
    const double tolerance = 4.0 / qMax(1.0e-12, m_scale);
    const QVector<int> candidates = m_spatialIndex.query(
        QRectF(worldPoint.x() - tolerance, worldPoint.y() - tolerance,
               tolerance * 2.0, tolerance * 2.0),
        256);
    int bestIndex = -1;
    double bestArea = std::numeric_limits<double>::max();
    for (int index : candidates) {
        const CadEntity &entity = m_document->entities[index];
        const CadLayer *layer = m_document->layerOf(entity);
        if ((layer && !layer->visible) || entity.points.size() < 3
            || (!entity.closed && entity.type != CadEntityType::Solid
                && entity.type != CadEntityType::Circle)) {
            continue;
        }
        QPolygonF polygon;
        polygon.reserve(entity.points.size());
        for (const QPointF &point : entity.points) {
            polygon.append(point);
        }
        if (!polygon.containsPoint(worldPoint, Qt::OddEvenFill)) {
            continue;
        }
        const double area = qAbs(entity.bounds.width()
                                 * entity.bounds.height());
        if (area < bestArea) {
            bestArea = area;
            bestIndex = index;
        }
    }
    if (bestIndex < 0) {
        return std::nullopt;
    }
    Measurement measurement;
    measurement.type = type;
    measurement.closed =
        type == MeasurementType::FilledArea
        || type == MeasurementType::Rectangle
        || type == MeasurementType::Polygon;
    const CadEntity &source = m_document->entities[bestIndex];
    measurement.points = source.points;
    for (const CadDimensionEdge &edge : source.dimensionEdges) {
        measurement.edgeLengths.append(edge.length);
        measurement.edgeMidpoints.append(edge.midpoint);
        measurement.edgeArcLengths.append(edge.arc ? edge.length : 0.0);
    }
    measurement.labelPosition = worldPoint;
    measurement.color = m_settings.annotationColor;
    return measurement;
}

bool CadView::traceArcBetween(const QPointF &start, const QPointF &end,
                              QVector<QPointF> *path, double *length,
                              QPointF *midpoint) const
{
    if (!m_document || !path || !length || !midpoint) {
        return false;
    }
    const double tolerance = 5.0 / qMax(1.0e-12, m_scale);
    const QRectF box(start, end);
    const QVector<int> candidates = m_spatialIndex.query(
        box.normalized().adjusted(-tolerance, -tolerance, tolerance,
                                  tolerance),
        512);
    for (int index : candidates) {
        const CadEntity &entity = m_document->entities[index];
        const CadLayer *layer = m_document->layerOf(entity);
        if ((layer && !layer->visible)
            || entity.type != CadEntityType::Arc
            || entity.points.size() < 2) {
            continue;
        }
        bool forward =
            QLineF(entity.points.first(), start).length() <= tolerance
            && QLineF(entity.points.last(), end).length() <= tolerance;
        const bool reverse =
            QLineF(entity.points.last(), start).length() <= tolerance
            && QLineF(entity.points.first(), end).length() <= tolerance;
        if (!forward && !reverse) {
            continue;
        }
        *path = entity.points;
        if (reverse) {
            std::reverse(path->begin(), path->end());
        }
        double sweep = entity.endAngle - entity.startAngle;
        while (sweep <= 0.0) {
            sweep += 2.0 * kPi;
        }
        *length = entity.radius * sweep;
        const double middleAngle =
            entity.startAngle + sweep * 0.5;
        *midpoint = entity.center
            + QPointF(std::cos(middleAngle), std::sin(middleAngle))
                * entity.radius;
        return true;
    }
    return false;
}

QPointF CadView::areaInteriorPoint(
    const QVector<QPointF> &vertices) const
{
    if (vertices.isEmpty()) {
        return m_center;
    }
    QPolygonF polygon(vertices);
    double crossSum = 0.0;
    QPointF weighted;
    for (int i = 0; i < vertices.size(); ++i) {
        const QPointF a = vertices[i];
        const QPointF b = vertices[(i + 1) % vertices.size()];
        const double cross = a.x() * b.y() - b.x() * a.y();
        crossSum += cross;
        weighted += (a + b) * cross;
    }
    if (qAbs(crossSum) > 1.0e-12) {
        const QPointF centroid = weighted / (3.0 * crossSum);
        if (polygon.containsPoint(centroid, Qt::OddEvenFill)) {
            return centroid;
        }
    }
    QRectF bounds;
    for (const QPointF &point : vertices) {
        bounds = bounds.isNull() ? QRectF(point, point)
                                 : bounds.united(QRectF(point, point));
    }
    const QPointF preferred = bounds.center();
    if (polygon.containsPoint(preferred, Qt::OddEvenFill)) {
        return preferred;
    }
    QPointF best = vertices.first();
    double bestDistance = std::numeric_limits<double>::max();
    for (int y = 1; y < 20; ++y) {
        for (int x = 1; x < 20; ++x) {
            const QPointF candidate(
                bounds.left() + bounds.width() * x / 20.0,
                bounds.top() + bounds.height() * y / 20.0);
            if (!polygon.containsPoint(candidate, Qt::OddEvenFill)) {
                continue;
            }
            const double distance =
                QLineF(candidate, preferred).length();
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        }
    }
    return best;
}

QString CadView::toolPrompt() const
{
    switch (m_mode) {
    case ToolMode::Select:
        return QStringLiteral(
            "按住左键拖动图纸；滚轮缩放；单击标注文字后可拖动标注");
    case ToolMode::Erase:
        return QStringLiteral(
            "删除标注：单击标注删除，拖动框选批量删除，右键退出");
    case ToolMode::Aligned:
        return m_awaitingLabel
            ? QStringLiteral("单击放置对齐尺寸文字")
            : QStringLiteral("对齐尺寸：选择起点和终点，尺寸线与两点平行");
    case ToolMode::Linear:
        return m_awaitingLabel
            ? QStringLiteral("单击放置线性尺寸文字")
            : QStringLiteral("线性尺寸：选择起点和终点，自动取水平或垂直距离");
    case ToolMode::Continuous:
        return m_awaitingLabel
            ? QStringLiteral("单击放置连续测量总长标注")
            : QStringLiteral(
                "连续测量：左键选节点；右键、Enter、双击或工具栏"
                "“完成当前测量”结束路径，再单击放置总长标注");
    case ToolMode::Batch:
        return m_awaitingLabel
            ? QStringLiteral("单击确定批量标注偏移位置")
            : QStringLiteral("批量测量：连续选点，完成后一次生成各段尺寸");
    case ToolMode::Rectangle:
        return m_awaitingLabel
            ? QStringLiteral("单击放置矩形面积标注")
            : QStringLiteral("矩形测量：选择两个对角点");
    case ToolMode::Polygon:
        return m_awaitingLabel
            ? QStringLiteral("单击放置多边形面积/周长标注")
            : QStringLiteral("多边形测量：选点，单击起点或双击闭合");
    case ToolMode::CircleArea:
        return m_awaitingLabel
            ? QStringLiteral("在圆内单击放置面积与周长标注")
            : QStringLiteral("测量圆：在圆周或圆弧上选择三个不重复点");
    case ToolMode::FilledArea:
        return QStringLiteral("测量填充面积：单击闭合填充或闭合轮廓内部");
    case ToolMode::SideArea:
        return m_awaitingLabel
            ? QStringLiteral("单击放置总长与侧面积标注")
            : QStringLiteral(
                "侧面积：连续选择线段节点或圆弧；右键、Enter、"
                "双击或闭合起点结束，再放置总长与侧面积");
    case ToolMode::Radius:
        return m_awaitingLabel
            ? QStringLiteral("单击放置半径标注")
            : QStringLiteral("半径测量：在圆周上选择三个不重复点");
    case ToolMode::Angle:
        return m_awaitingLabel
            ? QStringLiteral("单击放置角度标注")
            : QStringLiteral("角度测量：依次选择顶点、第一条边、第二条边");
    case ToolMode::ArcLength:
        return m_awaitingLabel
            ? QStringLiteral("单击放置弧长标注")
            : QStringLiteral("弧长测量：选择起点、弧上任意点、终点");
    case ToolMode::PrintArea:
        return QStringLiteral("框选打印区域：选择打印范围的两个对角点");
    case ToolMode::Calibrate:
        return QStringLiteral("比例校准：选择已知长度的两个端点");
    case ToolMode::Text:
        return QStringLiteral("文字注释：在图纸上选择文字放置位置");
    case ToolMode::ZoomWindow:
        return QStringLiteral("窗口缩放：依次选择要放大区域的两个对角点");
    }
    return {};
}

bool CadView::exportSvg(const QString &path, QString *errorMessage)
{
    if (!m_document) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("当前没有打开的图纸。");
        }
        return false;
    }
    QSvgGenerator generator;
    generator.setFileName(path);
    generator.setSize(QSize(1600, 1000));
    generator.setViewBox(QRect(0, 0, 1600, 1000));
    generator.setTitle(QStringLiteral("速看CAD 矢量图纸"));
    generator.setDescription(QStringLiteral("图纸、矢量文字与测量标注"));
    QPainter painter;
    if (!painter.begin(&generator)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("无法创建 SVG 文件。");
        }
        return false;
    }
    const QSizeF oldSize = m_renderSize;
    const double oldScale = m_scale;
    const bool oldPrintStyle = m_printStyle;
    m_renderSize = QSizeF(1600, 1000);
    m_scale = oldScale
        * qMin(1600.0 / qMax(1, width()), 1000.0 / qMax(1, height()));
    m_printStyle = true;
    renderScene(painter, QSize(1600, 1000), true);
    m_renderSize = oldSize;
    m_scale = oldScale;
    m_printStyle = oldPrintStyle;
    painter.end();
    return true;
}

bool CadView::printToPrinter(QPrinter *printer, const QRectF &worldArea,
                             bool blackInk)
{
    return printPagesToPrinter(
        printer, {{m_activeSpace, worldArea}}, blackInk);
}

bool CadView::printPagesToPrinter(
    QPrinter *printer,
    const QVector<CadPrintPage> &pages,
    bool blackInk)
{
    if (!printer || !m_document || pages.isEmpty()) {
        return false;
    }
    for (const CadPrintPage &page : pages) {
        if (!page.worldArea.isValid()
            || page.worldArea.width() <= 0.0
            || page.worldArea.height() <= 0.0) {
            return false;
        }
    }

    const QString oldSpace = m_activeSpace;
    const QSizeF oldRenderSize = m_renderSize;
    const QPointF oldCenter = m_center;
    const double oldScale = m_scale;
    const double oldFitScale = m_fitScale;
    const bool oldPrintStyle = m_printStyle;
    const bool oldPrintBlackInk = m_printBlackInk;
    QHash<CadPdfPage *, QImage> originalPdfImages;

    QPainter painter;
    if (!painter.begin(printer)) {
        return false;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    m_printStyle = true;
    m_printBlackInk = blackInk;
    bool rendered = false;
    for (qsizetype pageIndex = 0; pageIndex < pages.size(); ++pageIndex) {
        const CadPrintPage &printPage = pages[pageIndex];
        const QString targetSpace = printPage.spaceName.isEmpty()
            ? oldSpace : printPage.spaceName;
        const bool spaceExists = m_document->isPdf()
            ? m_document->pdfPage(targetSpace) != nullptr
            : (targetSpace == QStringLiteral("模型")
               || m_document->layoutEntities.contains(targetSpace));
        if (!spaceExists) {
            continue;
        }
        if (pageIndex > 0 && !printer->newPage()) {
            break;
        }
        if (targetSpace != m_activeSpace) {
            setActiveSpace(targetSpace);
        }
        if (m_document->isPdf()) {
            const QRectF devicePage =
                printer->pageRect(QPrinter::DevicePixel);
            for (int index = 0; index < m_document->pdfPages.size();
                 ++index) {
                CadPdfPage &pdfPage = m_document->pdfPages[index];
                if (pdfPage.name != targetSpace) {
                    continue;
                }
                const int requestedPixels = qMin(
                    8192,
                    qMax(3200, int(qMax(devicePage.width(),
                                        devicePage.height()))));
                if (requestedPixels
                    > qMax(pdfPage.image.width(),
                           pdfPage.image.height())) {
                    QImage highResolutionImage;
                    if (renderPdfPage(
                            m_document->path, index,
                            requestedPixels, &highResolutionImage,
                            nullptr)) {
                        if (!originalPdfImages.contains(&pdfPage)) {
                            originalPdfImages.insert(
                                &pdfPage, pdfPage.image);
                        }
                        pdfPage.image = std::move(highResolutionImage);
                        pdfPage.printImage = QImage();
                    }
                }
                break;
            }
        }

        const QRectF devicePage =
            printer->pageRect(QPrinter::DevicePixel);
        double dpiX = printer->logicalDpiX();
        double dpiY = printer->logicalDpiY();
        if (dpiX <= 0.0) {
            dpiX = printer->resolution();
        }
        if (dpiY <= 0.0) {
            dpiY = printer->resolution();
        }
        const double outputScaleX = qMax(1.0, dpiX / 96.0);
        const double outputScaleY = qMax(1.0, dpiY / 96.0);
        const QRectF page(
            0.0, 0.0,
            devicePage.width() / outputScaleX,
            devicePage.height() / outputScaleY);
        const double margin =
            qMin(page.width(), page.height()) * 0.035;
        const double availableWidth =
            qMax(1.0, page.width() - margin * 2.0);
        const double availableHeight =
            qMax(1.0, page.height() - margin * 2.0);
        m_renderSize = page.size();
        m_center = printPage.worldArea.center();
        m_scale = qMin(
            availableWidth / printPage.worldArea.width(),
            availableHeight / printPage.worldArea.height());
        const QRectF contentRect(
            (page.width()
             - printPage.worldArea.width() * m_scale) * 0.5,
            (page.height()
             - printPage.worldArea.height() * m_scale) * 0.5,
            printPage.worldArea.width() * m_scale,
            printPage.worldArea.height() * m_scale);
        painter.save();
        painter.scale(outputScaleX, outputScaleY);
        painter.setClipRect(page);
        painter.setClipRect(contentRect, Qt::IntersectClip);
        renderScene(painter, page.size().toSize(), true);
        painter.restore();
        rendered = true;
    }
    painter.end();

    for (auto iterator = originalPdfImages.cbegin();
         iterator != originalPdfImages.cend(); ++iterator) {
        iterator.key()->image = iterator.value();
        iterator.key()->printImage = QImage();
    }
    if (m_activeSpace != oldSpace) {
        setActiveSpace(oldSpace);
    }
    m_renderSize = oldRenderSize;
    m_center = oldCenter;
    m_scale = oldScale;
    m_fitScale = oldFitScale;
    m_printStyle = oldPrintStyle;
    m_printBlackInk = oldPrintBlackInk;
    invalidateSceneCache();
    emit viewTransformChanged();
    update();
    return rendered;
}
