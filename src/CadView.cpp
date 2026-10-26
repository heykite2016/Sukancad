#include "CadView.h"

#include "PdfDocument.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QFontDatabase>
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

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinScreenSegment = 1.0;

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
        return 0.62;
    }
    static const double millimeters[] = {
        0.0, 0.05, 0.09, 0.13, 0.15, 0.18, 0.20, 0.25,
        0.30, 0.35, 0.40, 0.50, 0.53, 0.60, 0.70, 0.80,
        0.90, 1.00, 1.06, 1.20, 1.40, 1.58, 2.00, 2.11
    };
    return 0.55 + millimeters[cadWeight] * 1.35;
}

QVector<QPointF> offsetPolygon(const QVector<QPointF> &polygon,
                              double distance)
{
    if (polygon.size() < 3 || qAbs(distance) < 1.0e-12) {
        return polygon;
    }
    QVector<QPointF> normalized = polygon;
    double twiceArea = 0.0;
    for (int i = 0; i < normalized.size(); ++i) {
        const QPointF a = normalized[i];
        const QPointF b = normalized[(i + 1) % normalized.size()];
        twiceArea += a.x() * b.y() - b.x() * a.y();
    }
    if (twiceArea < 0.0) {
        std::reverse(normalized.begin(), normalized.end());
    }
    QVector<QLineF> offsets;
    offsets.reserve(normalized.size());
    for (int i = 0; i < normalized.size(); ++i) {
        const QPointF a = normalized[i];
        const QPointF b = normalized[(i + 1) % normalized.size()];
        const QPointF delta = b - a;
        const double length = std::hypot(delta.x(), delta.y());
        if (length < 1.0e-12) {
            continue;
        }
        const QPointF normal(delta.y() / length, -delta.x() / length);
        offsets.append(QLineF(a + normal * distance,
                              b + normal * distance));
    }
    QVector<QPointF> result;
    result.reserve(offsets.size());
    for (int i = 0; i < offsets.size(); ++i) {
        const QLineF previous = offsets[(i + offsets.size() - 1)
                                        % offsets.size()];
        const QLineF current = offsets[i];
        QPointF intersection;
        const auto type = previous.intersects(current, &intersection);
        result.append(type == QLineF::NoIntersection ? current.p1()
                                                     : intersection);
    }
    if (twiceArea < 0.0) {
        std::reverse(result.begin(), result.end());
    }
    return result;
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

void appendHatchPatternLine(QVector<QLineF> &result,
                            const QPointF &origin,
                            const QPointF &direction,
                            const QPointF &offset,
                            const QRectF &bounds,
                            const QVector<double> &dashes)
{
    const double directionLength =
        std::hypot(direction.x(), direction.y());
    if (directionLength < 1.0e-9) {
        return;
    }
    const QPointF unit = direction / directionLength;
    const QPointF normal(-unit.y(), unit.x());
    const double offsetNormal =
        offset.x() * normal.x() + offset.y() * normal.y();
    if (qAbs(offsetNormal) < kMinScreenSegment) {
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
            if (line.length() >= kMinScreenSegment) {
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
                        result.append(QLineF(
                            pointAt(cursor),
                            pointAt(cursor + kMinScreenSegment)));
                    }
                } else if (dash > 0.0) {
                    const double dashStart =
                        qMax(start, cursor);
                    const double dashEnd =
                        qMin(end, cursor + length);
                    if (dashEnd > dashStart) {
                        const QLineF line(pointAt(dashStart),
                                          pointAt(dashEnd));
                        if (line.length() >= kMinScreenSegment) {
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

QColor annotationColor(const Measurement &measurement)
{
    return measurement.color.isValid() ? measurement.color
                                       : QColor(QStringLiteral("#37d67a"));
}

QColor colorForPrint(const QColor &source)
{
    if (!source.isValid()) {
        return QColor(Qt::black);
    }
    if (source.lightnessF() > 0.82) {
        return QColor(Qt::black);
    }
    return source.darker(125);
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

} // namespace

CadView::CadView(QWidget *parent) : QOpenGLWidget(parent)
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSamples(8);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSwapInterval(1);
    setFormat(format);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
}

QColor CadView::canvasBackgroundColor() const
{
    return m_drawingTheme == DrawingTheme::CadLight
        ? QColor(QStringLiteral("#f7f7f4"))
        : QColor(QStringLiteral("#000000"));
}

QColor CadView::displayColor(const QColor &source) const
{
    if (!source.isValid()) {
        return m_drawingTheme == DrawingTheme::CadLight
            ? QColor(QStringLiteral("#101820"))
            : QColor(QStringLiteral("#f2f4f5"));
    }
    if (m_drawingTheme == DrawingTheme::CadDark) {
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
    invalidatePanCache();
    m_document = std::move(document);
    m_cadTextPathCache.clear();
    m_measurements.clear();
    m_layoutCache = std::move(preparedLayouts);
    m_spaceViews.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    m_selectedMeasurementId.clear();
    clearDraft();
    if (m_document) {
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
        m_center = m_document->bounds.center();
        m_scale = 1.0;
        m_initialFitPending = true;
        QTimer::singleShot(20, this, [this] {
            if (m_initialFitPending && m_document) {
                fitToDrawing();
            }
        });
    } else {
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
        m_spaceViews.insert(
            m_activeSpace, {m_center, m_scale, m_fitScale});
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
        invalidatePanCache();
        m_spatialIndex = SpatialIndex();
        m_snapEngine.setDocument(nullptr, nullptr);
        m_selectedMeasurementId.clear();
        clearDraft();
        const auto saved = m_spaceViews.constFind(target);
        if (saved != m_spaceViews.cend() && saved->scale > 0.0) {
            m_center = saved->center;
            m_scale = saved->scale;
            m_fitScale = saved->fitScale;
            invalidatePanCache();
            emit viewTransformChanged();
            emit zoomChanged(
                m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
            update();
            schedulePanCache();
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
    invalidatePanCache();
    m_spatialIndex.rebuild(*m_document);
    m_snapEngine.setDocument(m_document.get(), &m_spatialIndex);
    m_selectedMeasurementId.clear();
    clearDraft();
    const auto saved = m_spaceViews.constFind(target);
    if (saved != m_spaceViews.cend() && saved->scale > 0.0) {
        m_center = saved->center;
        m_scale = saved->scale;
        m_fitScale = saved->fitScale;
        invalidatePanCache();
        emit viewTransformChanged();
        emit zoomChanged(
            m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
        update();
        schedulePanCache();
    } else {
        fitToDrawing();
    }
    emit activeSpaceChanged(target);
}

void CadView::setMeasurements(const QVector<Measurement> &measurements)
{
    m_measurements = measurements;
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
    invalidateHatchCache();
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
    setCursor(mode == ToolMode::Select ? Qt::OpenHandCursor
                                       : Qt::CrossCursor);
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
    case ToolMode::Batch:
    case ToolMode::Polygon:
    case ToolMode::AreaOffset:
        finishDraftForLabel();
        break;
    default:
        break;
    }
}

void CadView::cancelCurrentTool()
{
    m_pendingCalibrationRawLength = 0.0;
    m_calibrationPoints.clear();
    clearDraft();
    emit statusMessage(toolPrompt());
    update();
}

void CadView::resetView()
{
    if (!m_document || !m_document->hasSavedView
        || m_document->savedViewHeight <= 0.0) {
        fitToDrawing();
        return;
    }
    const double availableWidth = qMax(80.0, width() - 100.0);
    const double availableHeight = qMax(80.0, height() - 100.0);
    m_scale = qMax(
        1.0e-14,
        qMin(availableWidth
                 / qMax(1.0e-9, m_document->savedViewWidth),
             availableHeight / m_document->savedViewHeight));
    m_fitScale = m_scale;
    m_center = m_document->savedViewCenter;
    m_initialFitPending = false;
    invalidatePanCache();
    emit viewTransformChanged();
    emit zoomChanged(100.0);
    update();
    schedulePanCache();
}

void CadView::clearDraft()
{
    m_draftPoints.clear();
    m_awaitingLabel = false;
    m_polygonClosurePrompted = false;
    m_labelPreview = QPointF();
}

void CadView::fitToDrawing()
{
    if (!m_document || !m_document->bounds.isValid()) {
        return;
    }
    const double availableWidth = qMax(80.0, width() - 100.0);
    const double availableHeight = qMax(80.0, height() - 100.0);
    const double xScale = availableWidth
        / qMax(1.0e-9, m_document->bounds.width());
    const double yScale = availableHeight
        / qMax(1.0e-9, m_document->bounds.height());
    m_scale = qMax(1.0e-12, qMin(xScale, yScale));
    m_fitScale = m_scale;
    m_center = m_document->bounds.center();
    m_initialFitPending = false;
    invalidatePanCache();
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(stderr,
                     "fit bounds=(%.3f,%.3f,%.3f,%.3f) viewport=%dx%d "
                     "scale=%.9g center=(%.3f,%.3f)\n",
                     m_document->bounds.left(), m_document->bounds.top(),
                     m_document->bounds.right(),
                     m_document->bounds.bottom(), width(), height(), m_scale,
                     m_center.x(), m_center.y());
    }
    emit viewTransformChanged();
    emit zoomChanged(100.0);
    update();
    schedulePanCache();
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
    invalidateHatchCache();
    invalidateVectorCache();
    m_zoomPreviewActive = true;
    const quint64 generation = ++m_zoomPreviewGeneration;
    ++m_sceneCacheGeneration;
    emit viewTransformChanged();
    emit zoomChanged(m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
    update();
    QTimer::singleShot(120, this, [this, generation] {
        if (generation != m_zoomPreviewGeneration) {
            return;
        }
        m_zoomPreviewActive = false;
        update();
        schedulePanCache();
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
    invalidatePanCache();
    emit viewTransformChanged();
    emit zoomChanged(m_scale / qMax(1.0e-15, m_fitScale) * 100.0);
    update();
    schedulePanCache();
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
    m_settings.coordinatePrecision =
        qBound(0, m_settings.coordinatePrecision, 6);
    if (m_settings.annotationColor != previousAnnotationColor) {
        for (Measurement &measurement : m_measurements) {
            measurement.color = m_settings.annotationColor;
        }
    }
    emit measurementsChanged();
    update();
}

void CadView::setPrecision(int precision)
{
    m_settings.lengthPrecision = qBound(0, precision, 6);
    m_settings.areaPrecision = m_settings.lengthPrecision;
    m_settings.coordinatePrecision = m_settings.lengthPrecision;
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
    const QSizeF viewport =
        m_renderSize.isValid() ? m_renderSize : QSizeF(size());
    return QPointF((world.x() - m_center.x()) * m_scale
                       + viewport.width() * 0.5,
                   viewport.height() * 0.5
                       - (world.y() - m_center.y()) * m_scale);
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

void CadView::updateSnap(const QPointF &screenPoint)
{
    m_cursorScreen = screenPoint;
    m_cursorWorld = screenToWorld(screenPoint);
    m_snap = {};
    if (m_snapEnabled && m_document && !m_awaitingLabel
        && m_mode != ToolMode::Select) {
        m_snap =
            m_snapEngine.findSnap(m_cursorWorld, snapRadiusWorld(), m_snapModes);
    }
    if (m_snap.valid) {
        m_cursorWorld = m_snap.point;
        emit snapChanged(m_snap.label);
    } else {
        emit snapChanged({});
    }
    if (m_orthoEnabled && !m_awaitingLabel && !m_draftPoints.isEmpty()) {
        const QPointF origin = m_draftPoints.last();
        const QPointF delta = m_cursorWorld - origin;
        m_cursorWorld =
            qAbs(delta.x()) >= qAbs(delta.y())
            ? QPointF(m_cursorWorld.x(), origin.y())
            : QPointF(origin.x(), m_cursorWorld.y());
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
    invalidatePanCache();
    if (m_initialFitPending && m_document) {
        QTimer::singleShot(20, this, [this] {
            if (m_initialFitPending && m_document) {
                fitToDrawing();
            }
        });
    } else {
        schedulePanCache();
    }
}

void CadView::paintGL()
{
    m_renderSize = QSizeF(size());
    m_printStyle = false;
    QPainter painter(this);
    if (m_panning && m_panCacheValid) {
        drawPanCache(painter);
        return;
    }
    renderScene(painter, size());
}

void CadView::drawPanCache(QPainter &painter)
{
    painter.fillRect(rect(), canvasBackgroundColor());
    const QSizeF viewport(size());
    const double dpr = qMax(1.0, m_panCache.devicePixelRatio());
    const QSizeF cacheSize(m_panCache.width() / dpr,
                           m_panCache.height() / dpr);
    const double scaleRatio =
        m_scale / qMax(1.0e-15, m_panCacheScale);
    const QPointF topLeft(
        viewport.width() * 0.5 - cacheSize.width() * scaleRatio * 0.5
            - (m_center.x() - m_panCacheCenter.x()) * m_scale,
        viewport.height() * 0.5 - cacheSize.height() * scaleRatio * 0.5
            + (m_center.y() - m_panCacheCenter.y()) * m_scale);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(
        QRectF(topLeft,
               QSizeF(cacheSize.width() * scaleRatio,
                      cacheSize.height() * scaleRatio)),
        m_panCache);
}

void CadView::schedulePanCache()
{
    const quint64 generation = ++m_sceneCacheGeneration;
    QTimer::singleShot(220, this, [this, generation] {
        if (generation != m_sceneCacheGeneration
            || m_panning || m_zoomPreviewActive || !m_document
            || size().isEmpty()) {
            return;
        }
        rebuildPanCache();
    });
}

void CadView::rebuildPanCache()
{
    if (!m_document || size().isEmpty()) {
        m_panCacheValid = false;
        return;
    }
    // The cache is a motion preview. Rendering it at logical resolution
    // avoids a costly 2x Retina repaint while preserving the full-resolution
    // vector frame whenever the view is stationary.
    constexpr qreal dpr = 1.0;
    const QSize pixelSize(
        qMax(1, size().width()), qMax(1, size().height()));
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(canvasBackgroundColor());
    m_renderSize = QSizeF(size());
    m_printStyle = false;
    m_buildingPanCache = true;
    {
        QPainter painter(&image);
        renderScene(painter, size(), true);
    }
    m_buildingPanCache = false;
    m_panCache = std::move(image);
    m_panCacheCenter = m_center;
    m_panCacheScale = m_scale;
    m_panCacheValid = true;
}

void CadView::invalidatePanCache()
{
    m_panCacheValid = false;
    m_panCache = QImage();
    invalidateHatchCache();
    invalidateVectorCache();
}

void CadView::invalidateHatchCache()
{
    m_hatchCacheValid = false;
    m_hatchCache = QImage();
    m_hatchCacheSize = QSize();
    m_hatchCacheSegments = 0;
}

void CadView::invalidateVectorCache()
{
    m_vectorCacheValid = false;
    m_vectorCache = QImage();
    m_vectorCacheSize = QSize();
}

void CadView::invalidateDrawingCache()
{
    invalidatePanCache();
    invalidateHatchCache();
    update();
    schedulePanCache();
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
                                       const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (!m_document || size.isEmpty() || !area.isValid()) {
        return image;
    }
    const QSizeF previousSize = m_renderSize;
    const QPointF previousCenter = m_center;
    const double previousScale = m_scale;
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
    return image;
}

QImage CadView::renderPrintAreaToImage(const QRectF &area,
                                       const QSize &size)
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
    {
        QPainter painter(&image);
        renderScene(painter, size, true);
    }
    m_renderSize = previousSize;
    m_center = previousCenter;
    m_scale = previousScale;
    m_printStyle = previousPrintStyle;
    return image;
}

void CadView::renderScene(QPainter &painter, const QSize &viewport,
                          bool exporting)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
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

    if (!exporting && m_haveCursor && m_mode != ToolMode::Select
        && !m_panning) {
        painter.save();
        painter.setClipRect(QRect(QPoint(0, 0), viewport));
        QPen crosshairPen(QColor(103, 218, 238, 155), 0.65);
        crosshairPen.setCosmetic(true);
        painter.setPen(crosshairPen);
        painter.drawLine(QPointF(0.0, m_cursorScreen.y()),
                         QPointF(viewport.width(), m_cursorScreen.y()));
        painter.drawLine(QPointF(m_cursorScreen.x(), 0.0),
                         QPointF(m_cursorScreen.x(), viewport.height()));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(150, 241, 255, 210), 0.8));
        painter.drawRect(
            QRectF(m_cursorScreen.x() - 5.0,
                   m_cursorScreen.y() - 5.0, 10.0, 10.0));
        if (m_snap.valid) {
            drawSnapMarker(painter, worldToScreen(m_snap.point), m_snap.kind);
        }
        painter.restore();
    }
}

void CadView::drawPdfPage(QPainter &painter)
{
    if (!m_document || !m_document->isPdf()) {
        return;
    }
    const CadPdfPage *page = m_document->pdfPage(m_activeSpace);
    if (!page || page->image.isNull()) {
        return;
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
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(target, page->image);
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
    painter.setPen(QPen(
        m_drawingTheme == DrawingTheme::CadLight
            ? QColor(110, 122, 128, 64)
            : QColor(44, 50, 56, 95),
        0.55));
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
    if (!m_document || m_spatialIndex.isEmpty()) {
        return;
    }
    const bool interactivePreview =
        m_panning || m_zoomPreviewActive || m_buildingPanCache;
    if (!interactivePreview && !m_printStyle && !m_buildingVectorCache) {
        if (!m_vectorCacheValid || m_vectorCacheSize != viewport) {
            const qreal dpr = qMax<qreal>(1.0, devicePixelRatioF());
            QImage image(
                QSize(qMax(1, int(std::ceil(viewport.width() * dpr))),
                      qMax(1, int(std::ceil(viewport.height() * dpr)))),
                QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            {
                QPainter cachePainter(&image);
                cachePainter.setRenderHint(QPainter::Antialiasing, true);
                m_buildingVectorCache = true;
                drawCadEntities(cachePainter, viewport);
                m_buildingVectorCache = false;
            }
            m_vectorCache = std::move(image);
            m_vectorCacheSize = viewport;
            m_vectorCacheValid = true;
        }
        painter.drawImage(QPointF(0.0, 0.0), m_vectorCache);
        if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
            std::fprintf(stderr,
                         "geometry-cache viewport=%dx%d elapsed=%.2fms\n",
                         viewport.width(), viewport.height(),
                         renderTimer.nsecsElapsed() / 1.0e6);
        }
        return;
    }
    const QRectF world = visibleWorldRect();
    const QVector<int> visible =
        m_spatialIndex.query(world, m_document->entities.size());
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(stderr,
                     "paint viewport=%dx%d scale=%.9g world=(%.3f,%.3f,"
                     "%.3f,%.3f) visible=%lld\n",
                     viewport.width(), viewport.height(), m_scale,
                     world.left(), world.top(), world.right(),
                     world.bottom(), static_cast<long long>(visible.size()));
    }
    painter.setClipRect(QRect(QPoint(0, 0), viewport));

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
    QHash<LineBatchKey, LineBatch> batches;
    QHash<ClipKey, TextBatch> textBatches;
    QHash<int, QVector<int>> hatchGroups;
    QVector<int> solids;
    QVector<QPointF> points;
    solids.reserve(qMin(visible.size(), 256));
    batches.reserve(qMin(visible.size(), 64));
    textBatches.reserve(qMin(visible.size(), 32));

    for (int index : visible) {
        const CadEntity &entity = m_document->entities[index];
        const CadLayer *layer = m_document->layer(entity.layer);
        if (layer && !layer->visible) {
            continue;
        }
        if (entity.type == CadEntityType::Text) {
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
            continue;
        }
        if (entity.type == CadEntityType::Point && !entity.points.isEmpty()) {
            points.append(worldToScreen(entity.points.first()));
            continue;
        }
        if (entity.hatchGroup >= 0) {
            hatchGroups[entity.hatchGroup].append(index);
        }
        if (entity.type == CadEntityType::Solid && entity.filled
            && entity.hatchGroup < 0 && entity.points.size() >= 3) {
            solids.append(index);
            continue;
        }
        const double lineWidth =
            lineWidthPixels(entity.lineWeight, m_lineWeightsVisible);
        const QColor strokeColor =
            m_printStyle ? colorForPrint(entity.color)
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
        QPointF previous = worldToScreen(entity.points.first());
        for (qsizetype i = 1; i < entity.points.size(); ++i) {
            const QPointF current = worldToScreen(entity.points[i]);
            // Each source edge is tested independently. Sub-pixel edges are
            // omitted for display, but the previous endpoint always advances,
            // so disconnected short edges can never be joined into a false
            // diagonal.
            if (QLineF(previous, current).length()
                >= kMinScreenSegment) {
                batch.lines.append(QLineF(previous, current));
            }
            previous = current;
        }
        if (entity.closed && !entity.points.isEmpty()) {
            const QPointF first = worldToScreen(entity.points.first());
            if (QLineF(previous, first).length()
                >= kMinScreenSegment) {
                batch.lines.append(QLineF(previous, first));
            }
        }
    }

    qsizetype renderedHatchPatterns = 0;
    const auto paintHatches = [&](QPainter &target) {
        for (const QVector<int> &group : std::as_const(hatchGroups)) {
            if (group.isEmpty()) {
                continue;
            }
            QPainterPath path;
            QRectF clip;
            QColor color;
            std::shared_ptr<CadHatchPattern> pattern;
            bool fillShape = false;
            for (int index : group) {
                const CadEntity &entity = m_document->entities[index];
                if (entity.points.size() < 3) {
                    continue;
                }
                QPolygonF polygon;
                polygon.reserve(entity.points.size());
                for (const QPointF &point : entity.points) {
                    polygon.append(worldToScreen(point));
                }
                path.addPolygon(polygon);
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
            path.setFillRule(Qt::OddEvenFill);
            target.save();
            if (clip.isValid()) {
                const QPointF a = worldToScreen(clip.topLeft());
                const QPointF b = worldToScreen(clip.bottomRight());
                target.setClipRect(QRectF(a, b).normalized(),
                                   Qt::IntersectClip);
            }
            const QColor hatchColor =
                m_printStyle ? colorForPrint(color) : displayColor(color);
            if (pattern->solid && fillShape) {
                target.fillPath(path, hatchColor);
                ++renderedHatchPatterns;
            } else if (!pattern->lines.isEmpty() && !interactivePreview) {
                const QRectF bounds =
                    path.boundingRect()
                        .adjusted(-4.0, -4.0, 4.0, 4.0)
                        .intersected(
                            QRectF(QPointF(0.0, 0.0),
                                   QSizeF(viewport)));
                if (!bounds.isValid()) {
                    target.restore();
                    continue;
                }
                target.setClipPath(path, Qt::IntersectClip);
                QVector<QLineF> lines;
                lines.reserve(pattern->lines.size() * 32);
                for (const CadHatchPatternLine &definition :
                     std::as_const(pattern->lines)) {
                    const QPointF origin =
                        worldToScreen(definition.origin);
                    const QPointF direction =
                        worldToScreen(definition.origin
                                      + definition.direction)
                        - origin;
                    const QPointF offset =
                        worldToScreen(definition.origin
                                      + definition.offset)
                        - origin;
                    appendHatchPatternLine(
                        lines, origin, direction, offset, bounds,
                        definition.dashes);
                }
                QPen patternPen(hatchColor, 0.62);
                patternPen.setCosmetic(!m_printStyle);
                target.setPen(patternPen);
                target.drawLines(lines);
                renderedHatchPatterns += lines.size();
            } else if (pattern->gradient) {
                target.fillPath(
                    path, QColor(hatchColor.red(), hatchColor.green(),
                                 hatchColor.blue(), 55));
                ++renderedHatchPatterns;
            }
            target.restore();
        }
    };
    if (!interactivePreview) {
        if (m_printStyle || m_buildingVectorCache) {
            paintHatches(painter);
        } else {
            if (!m_hatchCacheValid || m_hatchCacheSize != viewport) {
                const qreal dpr = qMax<qreal>(1.0, devicePixelRatioF());
                QImage hatchImage(
                    QSize(qMax(1, int(std::ceil(viewport.width() * dpr))),
                          qMax(1, int(std::ceil(viewport.height() * dpr)))),
                    QImage::Format_ARGB32_Premultiplied);
                hatchImage.setDevicePixelRatio(dpr);
                hatchImage.fill(Qt::transparent);
                renderedHatchPatterns = 0;
                {
                    QPainter hatchPainter(&hatchImage);
                    hatchPainter.setRenderHint(QPainter::Antialiasing, true);
                    paintHatches(hatchPainter);
                }
                m_hatchCache = std::move(hatchImage);
                m_hatchCacheSize = viewport;
                m_hatchCacheSegments = renderedHatchPatterns;
                m_hatchCacheValid = true;
            }
            painter.drawImage(QPointF(0.0, 0.0), m_hatchCache);
            renderedHatchPatterns = m_hatchCacheSegments;
        }
    }
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
            painter.setClipRect(QRectF(a, b).normalized());
        }
        painter.setPen(pen);
        painter.drawLines(batch.lines);
        painter.restore();
    }
    for (int index : solids) {
        const CadEntity &entity = m_document->entities[index];
        QVector<QPointF> screenPoints;
        screenPoints.reserve(entity.points.size());
        for (const QPointF &point : entity.points) {
            screenPoints.append(worldToScreen(point));
        }
        QColor fill =
            m_printStyle ? colorForPrint(entity.color)
                         : displayColor(entity.color);
        fill.setAlpha(90);
        const QColor stroke =
            m_printStyle ? colorForPrint(entity.color)
                         : displayColor(entity.color);
        painter.save();
        if (entity.clipRect.isValid()) {
            const QPointF a = worldToScreen(entity.clipRect.topLeft());
            const QPointF b = worldToScreen(entity.clipRect.bottomRight());
            painter.setClipRect(QRectF(a, b).normalized());
        }
        painter.setPen(QPen(stroke,
                            lineWidthPixels(entity.lineWeight,
                                            m_lineWeightsVisible)));
        painter.setBrush(entity.filled ? QBrush(fill) : QBrush(Qt::NoBrush));
        painter.drawPath(
            polygonPath(painter, screenPoints, entity.closed));
        painter.restore();
    }
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
    painter.setBrush(Qt::NoBrush);
    qsizetype renderedTexts = 0;
    for (const TextBatch &batch : std::as_const(textBatches)) {
        painter.save();
        if (batch.clip.isValid()) {
            const QPointF a = worldToScreen(batch.clip.topLeft());
            const QPointF b = worldToScreen(batch.clip.bottomRight());
            painter.setClipRect(QRectF(a, b).normalized());
        }
        for (const CadEntity *entity : batch.entities) {
            drawCadText(painter, *entity);
        }
        renderedTexts += batch.entities.size();
        painter.restore();
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
    if (pixelHeight < kMinScreenSegment || pixelHeight > 4000.0) {
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
        if (m_cadTextPathCache.size() >= 8192) {
            m_cadTextPathCache.clear();
        }
        QFont font = cadFont(basePixelHeight);
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
    painter.setBrush(m_printStyle ? colorForPrint(entity.color)
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
    line.setLength(metrics.arrowSize);
    const double angle = std::atan2(line.dy(), line.dx());
    if (metrics.tickArrow) {
        const QPointF direction = tip - toward;
        const double length =
            std::hypot(direction.x(), direction.y());
        if (length <= 1.0e-9) {
            return;
        }
        const QPointF unit = direction / length;
        const QPointF tick =
            (unit + QPointF(-unit.y(), unit.x()))
            * (metrics.arrowSize * 0.48);
        painter.save();
        painter.setPen(
            QPen(color, 1.15, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(tip - tick, tip + tick);
        painter.restore();
        return;
    }
    const double spread = 0.42;
    QPainterPath arrow;
    arrow.moveTo(tip);
    arrow.lineTo(tip - QPointF(std::cos(angle - spread),
                              std::sin(angle - spread))
                            * metrics.arrowSize);
    arrow.lineTo(tip - QPointF(std::cos(angle + spread),
                              std::sin(angle + spread))
                            * metrics.arrowSize);
    arrow.closeSubpath();
    painter.save();
    if (metrics.openArrow) {
        painter.setPen(
            QPen(color, 1.15, Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin));
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
    QFont font = cadFont(emphasized ? metrics.textSize + 1.0
                                    : metrics.textSize,
                         emphasized ? QFont::DemiBold : QFont::Medium);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const double lineHeight =
        emphasized ? metrics.textSize + 5.0 : metrics.textSize + 4.0;
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
    const QColor ink = m_printStyle ? colorForPrint(color) : color;
    painter.translate(anchor);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPath(textPath);
    painter.restore();
    if (hitRect) {
        *hitRect = screenBounds.adjusted(-3.0, -3.0, 3.0, 3.0);
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
    QFont font = cadFont(emphasized ? metrics.textSize + 1.0
                                    : metrics.textSize,
                         emphasized ? QFont::DemiBold : QFont::Medium);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const double lineHeight =
        emphasized ? metrics.textSize + 5.0 : metrics.textSize + 4.0;
    QPainterPath textPath;
    for (int i = 0; i < lines.size(); ++i) {
        textPath.addText(
            QPointF(0.0, lineHeight * i), font, lines[i]);
    }
    const QRectF textBounds = textPath.boundingRect();
    textPath.translate(-textBounds.center().x(),
                       -textBounds.center().y());
    painter.save();
    painter.translate(anchor);
    painter.rotate(angleDegrees);
    painter.setPen(Qt::NoPen);
    const QColor ink = m_printStyle ? colorForPrint(color) : color;
    painter.setBrush(ink);
    painter.drawPath(textPath);
    if (hitRect) {
        QTransform transform;
        transform.translate(anchor.x(), anchor.y());
        transform.rotate(angleDegrees);
        *hitRect = transform.mapRect(
            textPath.boundingRect().adjusted(-2.0, -2.0, 2.0, 2.0));
    }
    painter.restore();
}

void CadView::drawMeasurements(QPainter &painter)
{
    m_labelHitRects.clear();
    for (const Measurement &measurement : std::as_const(m_measurements)) {
        if (!measurement.visible || measurement.space != m_activeSpace) {
            continue;
        }
        drawMeasurement(
            painter, measurement,
            measurement.id == m_selectedMeasurementId
                || (m_mode == ToolMode::Erase
                    && measurement.id == m_hoveredMeasurementId));
    }
}

void CadView::drawMeasurement(QPainter &painter,
                              const Measurement &measurement, bool selected)
{
    if (!m_document) {
        return;
    }
    QColor color = annotationColor(measurement);
    if (m_printStyle) {
        color = colorForPrint(color);
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
    QPen pen(color,
             selected ? styleMetrics.lineWidth + 0.65
                      : styleMetrics.lineWidth,
             Qt::SolidLine, Qt::RoundCap,
             Qt::RoundJoin);
    pen.setCosmetic(!m_printStyle);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (measurement.type == MeasurementType::Rectangle
        || measurement.type == MeasurementType::Polygon
        || measurement.type == MeasurementType::FilledArea
        || measurement.type == MeasurementType::AreaOffset
        || measurement.type == MeasurementType::SideArea) {
        if (points.size() >= 3) {
            // Measurement overlays must never cover drawing entities inside
            // the measured boundary, including the filled-area tool.
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(polygonPath(painter, points, true));
            painter.setBrush(Qt::NoBrush);
            const QPointF center =
                std::accumulate(points.cbegin(), points.cend(),
                                QPointF(0.0, 0.0))
                / qMax(1, points.size());
            if (!measurement.edgeLengths.isEmpty()
                && measurement.edgeMidpoints.size()
                    == measurement.edgeLengths.size()) {
                for (int i = 0; i < measurement.edgeLengths.size(); ++i) {
                    const QPointF midpoint =
                        worldToScreen(measurement.edgeMidpoints[i]);
                    QPointF outward = midpoint - center;
                    const double length =
                        std::hypot(outward.x(), outward.y());
                    outward = length > 1.0e-9 ? outward / length
                                              : QPointF(0.0, -1.0);
                    const QPointF leaderStart = midpoint + outward * 5.0;
                    const QPointF leaderEnd = midpoint + outward * 22.0;
                    painter.setPen(QPen(color, 1.0));
                    painter.drawLine(leaderStart, leaderEnd);
                    drawArrow(painter, leaderStart,
                              leaderEnd, color);
                    drawDimensionText(
                        painter,
                        formatDimensionValue(
                            measurement.edgeLengths[i], *m_document,
                            m_settings.lengthUnit,
                            m_settings.lengthPrecision),
                        midpoint + outward * 30.0, color, 0.0, nullptr,
                        false);
                }
            } else {
                for (int i = 0; i < points.size(); ++i) {
                    const int next = (i + 1) % points.size();
                    const QPointF a = points[i];
                    const QPointF b = points[next];
                    const QPointF midpoint = (a + b) * 0.5;
                    QPointF outward = midpoint - center;
                    const double length =
                        std::hypot(outward.x(), outward.y());
                    outward = length > 1.0e-9 ? outward / length
                                              : QPointF(0.0, -1.0);
                    const QPointF dimensionA = a + outward * 15.0;
                    const QPointF dimensionB = b + outward * 15.0;
                    painter.setPen(QPen(color, 1.0));
                    painter.drawLine(
                        a + outward * styleMetrics.extensionOffset,
                        dimensionA
                            + outward * styleMetrics.extensionOverrun);
                    painter.drawLine(
                        b + outward * styleMetrics.extensionOffset,
                        dimensionB
                            + outward * styleMetrics.extensionOverrun);
                    painter.drawLine(dimensionA, dimensionB);
                    if (QLineF(dimensionA, dimensionB).length() > 24.0) {
                        drawArrow(painter, dimensionA, dimensionB, color);
                        drawArrow(painter, dimensionB, dimensionA, color);
                    }
                    const double angle =
                        std::atan2(b.y() - a.y(), b.x() - a.x())
                        * 180.0 / kPi;
                    drawDimensionText(
                        painter,
                        formatDimensionValue(
                            QLineF(measurement.points[i],
                                   measurement.points[next])
                                .length(),
                            *m_document, m_settings.lengthUnit,
                            m_settings.lengthPrecision),
                        midpoint + outward * 29.0, color, angle, nullptr,
                        false);
                }
            }
        }
    } else if (measurement.type == MeasurementType::Continuous
               && points.size() >= 2) {
        QPen pathPen(pen);
        pathPen.setWidthF(selected ? 2.3 : 1.8);
        painter.setPen(pathPen);
        painter.setBrush(Qt::NoBrush);
        for (int i = 1; i < points.size(); ++i) {
            painter.drawLine(points[i - 1], points[i]);
        }
        for (const QPointF &point : std::as_const(points)) {
            painter.drawEllipse(point, 2.2, 2.2);
        }

        QPointF arrowTip = points.first();
        double bestDistance = std::numeric_limits<double>::max();
        for (int i = 1; i < points.size(); ++i) {
            const QPointF segment = points[i] - points[i - 1];
            const double lengthSquared =
                QPointF::dotProduct(segment, segment);
            if (lengthSquared < 1.0e-12) {
                continue;
            }
            const double t = qBound(
                0.0,
                QPointF::dotProduct(label - points[i - 1], segment)
                    / lengthSquared,
                1.0);
            const QPointF candidate = points[i - 1] + segment * t;
            const double distance = QLineF(label, candidate).length();
            if (distance < bestDistance) {
                bestDistance = distance;
                arrowTip = candidate;
            }
        }
        if (QLineF(label, arrowTip).length() > 2.0) {
            painter.setPen(QPen(color, selected ? 1.6 : 1.2));
            painter.drawLine(label, arrowTip);
            drawArrow(painter, arrowTip, label, color);
        }
    } else if (measurement.type == MeasurementType::Radius) {
        if (points.size() >= 2) {
            const QPointF center = points[0];
            const QPointF rim = points[1];
            painter.drawLine(center, rim);
            drawArrow(painter, rim, center, color);
            painter.drawLine(center + QPointF(-6.0, 0.0),
                             center + QPointF(6.0, 0.0));
            painter.drawLine(center + QPointF(0.0, -6.0),
                             center + QPointF(0.0, 6.0));
            painter.drawEllipse(center, 2.4, 2.4);
            if (QLineF(rim, label).length() > 3.0) {
                painter.setPen(QPen(color, 0.9, Qt::DashLine));
                painter.drawLine(rim, label);
            }
        }
    } else if (measurement.type == MeasurementType::CircleArea
               && points.size() >= 2) {
        const QPointF center = points[0];
        const QPointF rim = points[1];
        const double radius = QLineF(center, rim).length();
        painter.drawEllipse(center, radius, radius);
        painter.drawLine(center, rim);
        drawArrow(painter, rim, center, color);
        painter.drawLine(center + QPointF(-8.0, 0.0),
                         center + QPointF(8.0, 0.0));
        painter.drawLine(center + QPointF(0.0, -8.0),
                         center + QPointF(0.0, 8.0));
        painter.drawEllipse(center, 2.6, 2.6);
        QPointF radial = rim - center;
        const double radialLength =
            std::hypot(radial.x(), radial.y());
        if (radius > 0.0 && radialLength > 1.0e-9) {
            radial /= radialLength;
            const QPointF radiusLabel = rim + radial * 13.0;
            drawDimensionText(
                painter,
                QStringLiteral("R%1").arg(formatDimensionValue(
                    QLineF(measurement.points[0],
                           measurement.points[1]).length(),
                    *m_document, m_settings.lengthUnit,
                    m_settings.lengthPrecision)),
                radiusLabel, color,
                std::atan2(radial.y(), radial.x()) * 180.0 / kPi,
                nullptr, false);
        }
    } else if (measurement.type == MeasurementType::Diameter) {
        if (points.size() >= 2) {
            QPointF start = points[0];
            QPointF end = points[1];
            start = points[0] - (points[1] - points[0]);
            painter.drawLine(start, end);
            drawArrow(painter, end, start, color);
            drawArrow(painter, start, end, color);
            painter.drawEllipse(points[0], 2.5, 2.5);
        }
    } else if (measurement.type == MeasurementType::Angle
               && points.size() >= 3) {
        const QPointF vertex = points[0];
        const double angleA = std::atan2(points[1].y() - vertex.y(),
                                         points[1].x() - vertex.x());
        const double angleB = std::atan2(points[2].y() - vertex.y(),
                                         points[2].x() - vertex.x());
        QRectF arcBox(vertex.x() - 28.0, vertex.y() - 28.0, 56.0, 56.0);
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
                    origin + direction * styleMetrics.extensionOffset,
                    dimensionPoint
                        + direction * styleMetrics.extensionOverrun);
            };
            drawExtension(points[0], a);
            drawExtension(points[1], b);
            painter.drawLine(a, b);
            drawArrow(painter, a, b, color);
            drawArrow(painter, b, a, color);
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
                origin + direction * styleMetrics.extensionOffset,
                dimensionPoint
                    + direction * styleMetrics.extensionOverrun);
        };
        drawExtension(points[0], a);
        drawExtension(points[1], b);
        painter.drawLine(a, b);
        drawArrow(painter, a, b, color);
        drawArrow(painter, b, a, color);
    } else if (measurement.type == MeasurementType::PointToLine
               && points.size() >= 3) {
        const QPointF delta = points[2] - points[1];
        const double lengthSquared = QPointF::dotProduct(delta, delta);
        if (lengthSquared > 1.0e-12) {
            const QPointF foot =
                points[1]
                + delta
                    * (QPointF::dotProduct(points[0] - points[1], delta)
                       / lengthSquared);
            painter.setPen(
                QPen(color, selected ? 2.0 : 1.35,
                     Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(points[0], foot);
            drawArrow(painter, points[0], foot, color);
            drawArrow(painter, foot, points[0], color);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(foot.x() - 3.0, foot.y() - 3.0,
                                    6.0, 6.0));
            const QPointF dimensionMidpoint =
                (points[0] + foot) * 0.5;
            if (QLineF(label, dimensionMidpoint).length() > 14.0) {
                painter.setPen(QPen(color, selected ? 1.4 : 1.0));
                painter.drawLine(label, dimensionMidpoint);
                drawArrow(painter, dimensionMidpoint, label, color);
            }
        }
    } else if (measurement.type == MeasurementType::Coordinate
               && !points.isEmpty()) {
        painter.drawLine(points.first() + QPointF(-7, 0),
                         points.first() + QPointF(7, 0));
        painter.drawLine(points.first() + QPointF(0, -7),
                         points.first() + QPointF(0, 7));
    }

    QString labelText = value;
    if (measurement.type == MeasurementType::CircleArea
        && measurement.points.size() >= 2) {
        const QString centerX = formatDimensionValue(
            measurement.points[0].x(), *m_document,
            m_settings.coordinateUnit, m_settings.coordinatePrecision);
        const QString centerY = formatDimensionValue(
            measurement.points[0].y(), *m_document,
            m_settings.coordinateUnit, m_settings.coordinatePrecision);
        labelText =
            QStringLiteral("圆心：（%1，%2）\n面积：%3\n周长：%4")
                .arg(centerX, centerY,
                     formatAreaValue(
                         measurement.rawArea(), *m_document,
                         m_settings.areaUnit, m_settings.areaPrecision),
                     formatDimensionValue(
                         measurement.rawLength(), *m_document,
                         m_settings.lengthUnit,
                         m_settings.lengthPrecision));
    }
    if (!measurement.note.isEmpty()
        && measurement.type != MeasurementType::Text) {
        labelText += QStringLiteral("\n") + measurement.note;
    }
    if (measurement.type == MeasurementType::Text) {
        labelText = measurement.note;
    }
    const bool areaCallout =
        measurement.type == MeasurementType::Rectangle
        || measurement.type == MeasurementType::Polygon
        || measurement.type == MeasurementType::FilledArea
        || measurement.type == MeasurementType::AreaOffset
        || measurement.type == MeasurementType::SideArea
        || measurement.type == MeasurementType::CircleArea;
    if (areaCallout
        && measurement.type != MeasurementType::CircleArea
        && !points.isEmpty()) {
        QPointF target = points.first();
        double bestDistance = std::numeric_limits<double>::max();
        if ((measurement.type == MeasurementType::CircleArea
             || measurement.type == MeasurementType::FilledArea)
            && points.size() >= 2
            && measurement.type == MeasurementType::CircleArea) {
            const QPointF radial = label - points[0];
            const double radialLength =
                std::hypot(radial.x(), radial.y());
            if (radialLength > 1.0e-9) {
                target = points[0]
                    + radial / radialLength
                        * QLineF(points[0], points[1]).length();
            }
        } else {
            for (int i = 0; i < points.size(); ++i) {
                const QPointF a = points[i];
                const QPointF b = points[(i + 1) % points.size()];
                const QPointF delta = b - a;
                const double lengthSquared =
                    QPointF::dotProduct(delta, delta);
                const double t = lengthSquared > 1.0e-12
                    ? qBound(0.0,
                             QPointF::dotProduct(label - a, delta)
                                 / lengthSquared,
                             1.0)
                    : 0.0;
                const QPointF candidate = a + delta * t;
                const double distance =
                    QLineF(label, candidate).length();
                if (distance < bestDistance) {
                    bestDistance = distance;
                    target = candidate;
                }
            }
        }
        if (QLineF(label, target).length() > 14.0) {
            painter.setPen(QPen(color, selected ? 1.7 : 1.2));
            painter.drawLine(label, target);
            drawArrow(painter, target, label, color);
        }
    }
    QRectF hitRect;
    const bool boxedAnnotation = measurement.type == MeasurementType::Text;
    double textAngle = 0.0;
    QPointF textAnchor = label;
    if ((measurement.type == MeasurementType::Aligned
         || measurement.type == MeasurementType::FixedDistance)
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
            textAnchor = lineCenter + normal * side * 11.0;
        }
    } else if (measurement.type == MeasurementType::Linear
               && points.size() >= 2) {
        textAngle = measurement.auxiliaryValue == 2.0 ? -90.0 : 0.0;
        if (measurement.auxiliaryValue == 1.0) {
            const double side = label.y() >= points[0].y() ? 1.0 : -1.0;
            textAnchor = QPointF((points[0].x() + points[1].x()) * 0.5,
                                 label.y() + side * 11.0);
        } else {
            const double side = label.x() >= points[0].x() ? 1.0 : -1.0;
            textAnchor = QPointF(label.x() + side * 11.0,
                                 (points[0].y() + points[1].y()) * 0.5);
        }
    } else if (measurement.type == MeasurementType::Radius) {
        textAnchor = label;
        textAngle = 0.0;
    } else if (measurement.type == MeasurementType::Diameter) {
        if (points.size() >= 2) {
            textAngle = std::atan2(points[1].y() - points[0].y(),
                                   points[1].x() - points[0].x())
                * 180.0 / kPi;
            QPointF normal(-(points[1].y() - points[0].y()),
                           points[1].x() - points[0].x());
            const double normalLength =
                std::hypot(normal.x(), normal.y());
            if (normalLength > 1.0e-9) {
                normal /= normalLength;
                const QPointF lineCenter =
                    (points[0] + points[1]) * 0.5;
                const double side =
                    QPointF::dotProduct(label - lineCenter, normal) >= 0.0
                    ? 1.0
                    : -1.0;
                textAnchor = lineCenter + normal * side * 11.0;
            }
        }
    }
    if (boxedAnnotation) {
        drawVectorLabel(painter, labelText, label, color, &hitRect,
                        selected);
    } else {
        drawDimensionText(painter, labelText, textAnchor, color, textAngle,
                          &hitRect, selected);
    }
    if (hitRect.isValid()) {
        m_labelHitRects.insert(measurement.id, hitRect);
    }
}

std::optional<Measurement> CadView::draftMeasurement(
    const QPointF &labelWorld) const
{
    if (m_draftPoints.isEmpty()) {
        return std::nullopt;
    }
    if (m_mode == ToolMode::Radius && m_draftPoints.size() < 3) {
        return std::nullopt;
    }
    Measurement measurement;
    measurement.id = QStringLiteral("draft");
    measurement.labelPosition = labelWorld;
    measurement.points = m_draftPoints;
    switch (m_mode) {
    case ToolMode::Aligned:
        measurement.type = MeasurementType::Aligned;
        break;
    case ToolMode::Linear:
        measurement.type = MeasurementType::Linear;
        measurement.auxiliaryValue =
            qAbs(m_draftPoints.last().x() - m_draftPoints.first().x())
                >= qAbs(m_draftPoints.last().y()
                        - m_draftPoints.first().y())
            ? 1.0
            : 2.0;
        break;
    case ToolMode::Continuous:
        measurement.type = MeasurementType::Continuous;
        break;
    case ToolMode::Batch:
        measurement.type = MeasurementType::Batch;
        break;
    case ToolMode::Rectangle:
        measurement.type = MeasurementType::Rectangle;
        break;
    case ToolMode::Polygon:
        measurement.type = MeasurementType::Polygon;
        if (m_draftPoints.size() >= 3) {
            QVector<QPointF> traced;
            traced.reserve(m_draftPoints.size() * 8);
            for (int i = 0; i < m_draftPoints.size(); ++i) {
                const QPointF start = m_draftPoints[i];
                const QPointF end =
                    m_draftPoints[(i + 1) % m_draftPoints.size()];
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
                } else {
                    traced.append(end);
                    measurement.edgeLengths.append(
                        QLineF(start, end).length());
                    measurement.edgeMidpoints.append((start + end) * 0.5);
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
        break;
    case ToolMode::FilledArea:
        measurement.type = MeasurementType::FilledArea;
        break;
    case ToolMode::SideArea:
        measurement.type = MeasurementType::SideArea;
        measurement.auxiliaryValue = m_toolAuxiliaryValue;
        break;
    case ToolMode::AreaOffset:
        measurement.type = MeasurementType::AreaOffset;
        measurement.auxiliaryValue = m_toolAuxiliaryValue;
        measurement.points =
            offsetPolygon(m_draftPoints, m_toolAuxiliaryValue);
        for (int i = 0; i < measurement.points.size(); ++i) {
            const QPointF start = measurement.points[i];
            const QPointF end =
                measurement.points[(i + 1) % measurement.points.size()];
            measurement.edgeLengths.append(QLineF(start, end).length());
            measurement.edgeMidpoints.append((start + end) * 0.5);
        }
        break;
    case ToolMode::Radius:
        measurement.type = MeasurementType::Radius;
        break;
    case ToolMode::Diameter:
        measurement.type = MeasurementType::Diameter;
        break;
    case ToolMode::Angle:
        measurement.type = MeasurementType::Angle;
        break;
    case ToolMode::ArcLength:
        measurement.type = MeasurementType::ArcLength;
        break;
    case ToolMode::Coordinate:
        measurement.type = MeasurementType::Coordinate;
        break;
    case ToolMode::PointToLine:
        measurement.type = MeasurementType::PointToLine;
        break;
    case ToolMode::FixedDistance:
        measurement.type = MeasurementType::FixedDistance;
        measurement.auxiliaryValue = m_toolAuxiliaryValue;
        break;
    default:
        return std::nullopt;
    }
    updateAnnotationGeometryFromDraft(measurement);
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
        const QColor previewColor =
            m_mode == ToolMode::ZoomWindow
            ? QColor(QStringLiteral("#44ef9a"))
            : QColor(QStringLiteral("#e1b65a"));
        painter.setPen(QPen(previewColor, 1.1, Qt::DashLine));
        QColor fill = previewColor;
        fill.setAlpha(28);
        painter.setBrush(fill);
        painter.drawRect(
            QRectF(worldToScreen(m_draftPoints.first()), m_cursorScreen)
                .normalized());
        painter.restore();
        return;
    }
    if ((m_draftPoints.size() >= 2
         && !(m_mode == ToolMode::CircleArea
              && m_draftPoints.size() < 3))
        || (m_mode == ToolMode::Coordinate && m_draftPoints.size() == 1)) {
        std::optional<Measurement> preview =
            draftMeasurement(m_awaitingLabel ? m_labelPreview : m_cursorWorld);
        if (preview) {
            preview->color = QColor(QStringLiteral("#58a6d6"));
            preview->id = QStringLiteral("preview");
            drawMeasurement(painter, *preview, false);
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
    if (!m_draftPoints.isEmpty() && !m_awaitingLabel) {
        painter.setPen(QPen(QColor(98, 181, 223, 190), 1.0, Qt::DashLine));
        painter.drawLine(
            worldToScreen(m_draftPoints.last()),
            m_polygonClosurePrompted ? worldToScreen(m_draftPoints.first())
                                     : m_cursorScreen);
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
        if (m_mode == ToolMode::Continuous || m_mode == ToolMode::Batch
            || m_mode == ToolMode::Polygon
            || m_mode == ToolMode::AreaOffset) {
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
            setCursor(Qt::CrossCursor);
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
    if (m_mode == ToolMode::PointToLine && m_draftPoints.size() == 1) {
        const std::optional<QLineF> selectedLine =
            lineEntityAt(screenPoint);
        if (selectedLine) {
            m_draftPoints.append(selectedLine->p1());
            m_draftPoints.append(selectedLine->p2());
            finishDraftForLabel();
            update();
            return;
        }
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
        || (m_mode == ToolMode::SideArea && count >= 2)
        || (m_mode == ToolMode::Radius && count >= 3)
        || (m_mode == ToolMode::Diameter && count >= 2)
        || (m_mode == ToolMode::Angle && count >= 3)
        || (m_mode == ToolMode::ArcLength && count >= 3)
        || (m_mode == ToolMode::Coordinate && count >= 1)
        || (m_mode == ToolMode::PointToLine && count >= 3)
        || (m_mode == ToolMode::FixedDistance && count >= 2);
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
    case ToolMode::Batch:
        valid = m_draftPoints.size() >= 2;
        break;
    case ToolMode::Polygon:
    case ToolMode::AreaOffset:
        valid = m_draftPoints.size() >= 3;
        break;
    case ToolMode::Coordinate:
        valid = m_draftPoints.size() >= 1;
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
    emit statusMessage(toolPrompt());
    update();
}

bool CadView::commitDraft(const QPointF &labelWorld)
{
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
            const QPointF direction = nodes[i] - nodes[i - 1];
            QPointF normal(-direction.y(), direction.x());
            const double length =
                std::hypot(normal.x(), normal.y());
            if (length > 1.0e-12) {
                normal /= length;
            }
            segment.labelPosition =
                (nodes[i - 1] + nodes[i]) * 0.5
                + normal * (labelWorld - nodes.first()).manhattanLength()
                    * 0.08;
            segment.color = m_settings.annotationColor;
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
    std::optional<Measurement> measurement = draftMeasurement(labelWorld);
    if (!measurement) {
        return false;
    }
    measurement->id = defaultMeasurementId();
    measurement->space = m_activeSpace;
    measurement->color = m_settings.annotationColor;
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
        update();
        return;
    }
    if (m_dragLabel && !m_dragMeasurementId.isEmpty()) {
        const int index = measurementIndex(m_dragMeasurementId);
        if (index >= 0) {
            m_cursorWorld = screenToWorld(event->position());
            m_measurements[index].labelPosition = m_cursorWorld;
            update();
        }
        return;
    }
    if (m_eraseDragging) {
        m_eraseScreenRect =
            QRectF(m_eraseStartScreen, event->position()).normalized();
        update();
        return;
    }
    updateSnap(event->position());
    if (m_mode == ToolMode::Erase) {
        const QString hovered =
            hitMeasurementAt(event->position());
        if (hovered != m_hoveredMeasurementId) {
            m_hoveredMeasurementId = hovered;
            setCursor(hovered.isEmpty() ? Qt::CrossCursor
                                        : Qt::PointingHandCursor);
        }
    } else if (!m_hoveredMeasurementId.isEmpty()) {
        m_hoveredMeasurementId.clear();
        setCursor(Qt::CrossCursor);
    }
    // Selection mode has no canvas-following overlay. Re-rendering the whole
    // drawing for every pointer move makes large plans appear frozen.
    if (m_mode != ToolMode::Select) {
        update();
    }
}

void CadView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_leftPressPending && event->button() == Qt::LeftButton) {
        const bool wasPanning = m_panning;
        m_leftPressPending = false;
        if (wasPanning) {
            m_panning = false;
            invalidatePanCache();
            setCursor(m_spaceDown || m_mode == ToolMode::Select
                          ? Qt::OpenHandCursor : Qt::CrossCursor);
            update();
            schedulePanCache();
            return;
        }
        leftClick(event->position());
        return;
    }
    if (m_panning
        && (event->button() == Qt::MiddleButton
            || event->button() == Qt::LeftButton)) {
        m_panning = false;
        invalidatePanCache();
        setCursor(m_spaceDown || m_mode == ToolMode::Select
                      ? Qt::OpenHandCursor : Qt::CrossCursor);
        update();
        schedulePanCache();
        return;
    }
    if (m_dragLabel && event->button() == Qt::LeftButton) {
        m_dragLabel = false;
        m_dragMeasurementId.clear();
        m_draftPoints.clear();
        emit measurementsChanged();
        setCursor(Qt::OpenHandCursor);
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
            || m_mode == ToolMode::Polygon
            || m_mode == ToolMode::AreaOffset)
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
            event->accept();
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

void CadView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (!m_awaitingLabel
            && (m_mode == ToolMode::Continuous
                || m_mode == ToolMode::Batch
                || m_mode == ToolMode::Polygon
                || m_mode == ToolMode::AreaOffset)
            && (m_mode == ToolMode::Polygon
                    || m_mode == ToolMode::AreaOffset
                ? m_draftPoints.size() >= 3
                : m_draftPoints.size() >= 2)) {
            finishDraftForLabel();
            return;
        }
        if (!m_draftPoints.isEmpty() || m_awaitingLabel) {
            cancelCurrentTool();
        } else if (m_mode != ToolMode::Select) {
            beginTool(ToolMode::Select);
        }
        m_hoveredMeasurementId.clear();
        return;
    }
    if (event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        if ((m_mode == ToolMode::Continuous
             || m_mode == ToolMode::Batch
             || m_mode == ToolMode::Polygon
             || m_mode == ToolMode::AreaOffset)
            && !m_awaitingLabel) {
            finishDraftForLabel();
            return;
        }
    }
    if (event->key() == Qt::Key_Delete) {
        if (!m_selectedMeasurementId.isEmpty()) {
            removeMeasurement(m_selectedMeasurementId);
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
            setCursor(m_mode == ToolMode::Select
                          ? Qt::OpenHandCursor : Qt::CrossCursor);
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
    update();
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
    for (int index = m_measurements.size() - 1; index >= 0; --index) {
        const Measurement &measurement = m_measurements[index];
        if (!measurement.visible || measurement.space != m_activeSpace) {
            continue;
        }
        const auto labelHit = m_labelHitRects.constFind(measurement.id);
        if (labelHit != m_labelHitRects.cend()
            && labelHit.value().contains(screenPoint)) {
            return measurement.id;
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
            || measurement.type == MeasurementType::FilledArea
            || measurement.type == MeasurementType::AreaOffset;
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
        if ((measurement.type == MeasurementType::Aligned
             || measurement.type == MeasurementType::FixedDistance)
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
        if (measurement.type == MeasurementType::PointToLine
            && points.size() >= 3
            && distanceToSegment(QLineF(points[1], points[2]),
                                 screenPoint)
                <= 7.0) {
            return measurement.id;
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
             || measurement.type == MeasurementType::Diameter
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
    for (const CadViewportDefinition &viewport : viewports) {
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
        const QRectF modelWindow(
            viewport.modelCenter.x()
                - viewport.windowSize.width() * 0.5 / viewScale,
            viewport.modelCenter.y() - viewport.viewHeight * 0.5,
            viewport.windowSize.width() / viewScale,
            viewport.viewHeight);
        const auto mapPoint = [&](const QPointF &source) {
            const QPointF delta =
                (source - viewport.modelCenter) * viewScale;
            return viewport.center
                + QPointF(delta.x() * cosine - delta.y() * sine,
                          delta.x() * sine + delta.y() * cosine);
        };
        for (const CadEntity &source : modelEntities) {
            if (viewport.frozenLayers.contains(source.layer)) {
                continue;
            }
            if (source.bounds.isValid()
                && !source.bounds.intersects(modelWindow)) {
                continue;
            }
            CadEntity entity = source;
            for (QPointF &point : entity.points) {
                point = mapPoint(point);
            }
            entity.center = mapPoint(entity.center);
            entity.textPosition = mapPoint(entity.textPosition);
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
    }
    if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_RENDER")) {
        std::fprintf(stderr, "layout=%s viewports=%lld content=%lld\n",
                     name.toUtf8().constData(),
                     static_cast<long long>(viewports.size()),
                     static_cast<long long>(content.size()));
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

std::optional<QLineF> CadView::lineEntityAt(
    const QPointF &screenPoint) const
{
    if (!m_document) {
        return std::nullopt;
    }
    const QPointF worldPoint = screenToWorld(screenPoint);
    const double tolerance = 9.0 / qMax(1.0e-12, m_scale);
    const QVector<int> candidates = m_spatialIndex.query(
        QRectF(worldPoint.x() - tolerance, worldPoint.y() - tolerance,
               tolerance * 2.0, tolerance * 2.0),
        1024);
    double bestDistance = 9.0;
    QLineF bestLine;
    bool found = false;
    for (int index : candidates) {
        const CadEntity &entity = m_document->entities[index];
        const CadLayer *layer = m_document->layer(entity.layer);
        if ((layer && !layer->visible)
            || (entity.type != CadEntityType::Segment
                && entity.type != CadEntityType::Polyline)
            || entity.points.size() < 2) {
            continue;
        }
        const int segmentCount =
            entity.points.size() - (entity.closed ? 0 : 1);
        for (int i = 0; i < segmentCount; ++i) {
            const QPointF a = entity.points[i];
            const QPointF b =
                entity.points[(i + 1) % entity.points.size()];
            const QLineF screenLine(worldToScreen(a), worldToScreen(b));
            const double distance =
                distanceToSegment(screenLine, screenPoint);
            if (distance <= bestDistance) {
                bestDistance = distance;
                bestLine = QLineF(a, b);
                found = true;
            }
        }
    }
    return found ? std::optional<QLineF>(bestLine) : std::nullopt;
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
        const CadLayer *layer = m_document->layer(entity.layer);
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
        const CadLayer *layer = m_document->layer(entity.layer);
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
        const CadLayer *layer = m_document->layer(entity.layer);
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
        double total = 0.0;
        for (int i = 1; i < path->size(); ++i) {
            total += QLineF(path->at(i - 1), path->at(i)).length();
        }
        *length = total;
        *midpoint = path->at(path->size() / 2);
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
            ? QStringLiteral("单击放置侧面积标注")
            : QStringLiteral("计算侧面积：选择底面长度线段的两端");
    case ToolMode::AreaOffset:
        return m_awaitingLabel
            ? QStringLiteral("单击放置偏移面积标注")
            : QStringLiteral(
                "面积偏移：按设定偏移距离绘制闭合边界，单击起点闭合");
    case ToolMode::Radius:
        return m_awaitingLabel
            ? QStringLiteral("单击放置半径标注")
            : QStringLiteral("半径测量：在圆周上选择三个不重复点");
    case ToolMode::Diameter:
        return m_awaitingLabel
            ? QStringLiteral("单击放置直径标注")
            : QStringLiteral("直径测量：选择圆心和圆周点");
    case ToolMode::Angle:
        return m_awaitingLabel
            ? QStringLiteral("单击放置角度标注")
            : QStringLiteral("角度测量：依次选择顶点、第一条边、第二条边");
    case ToolMode::ArcLength:
        return m_awaitingLabel
            ? QStringLiteral("单击放置弧长标注")
            : QStringLiteral("弧长测量：选择起点、弧上任意点、终点");
    case ToolMode::Coordinate:
        return m_awaitingLabel
            ? QStringLiteral("单击放置坐标标注")
            : QStringLiteral("坐标标注：选择要标注的点");
    case ToolMode::PointToLine:
        return m_awaitingLabel
            ? QStringLiteral("单击放置点到直线距离标注")
            : QStringLiteral("点到直线：先选点，再选直线上的两点");
    case ToolMode::FixedDistance:
        return m_awaitingLabel
            ? QStringLiteral("单击放置定距标注")
            : QStringLiteral("定距测量：选择起点和方向点，长度使用设定值");
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
    generator.setTitle(QStringLiteral("CAD Lens 矢量图纸"));
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

bool CadView::printToPrinter(QPrinter *printer, const QRectF &worldArea)
{
    if (!printer || !m_document || !worldArea.isValid()
        || worldArea.width() <= 0.0 || worldArea.height() <= 0.0) {
        return false;
    }
    QPainter painter;
    if (!painter.begin(printer)) {
        return false;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
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
    const double availableWidth = qMax(1.0, page.width() - margin * 2.0);
    const double availableHeight =
        qMax(1.0, page.height() - margin * 2.0);
    const QSizeF oldSize = m_renderSize;
    const QPointF oldCenter = m_center;
    const double oldScale = m_scale;
    const bool oldPrintStyle = m_printStyle;

    m_renderSize = page.size();
    m_center = worldArea.center();
    m_scale = qMin(availableWidth / worldArea.width(),
                   availableHeight / worldArea.height());
    m_printStyle = true;
    painter.save();
    painter.scale(outputScaleX, outputScaleY);
    painter.setClipRect(page);
    renderScene(painter, page.size().toSize(), true);
    painter.restore();

    m_renderSize = oldSize;
    m_center = oldCenter;
    m_scale = oldScale;
    m_printStyle = oldPrintStyle;
    painter.end();
    return true;
}
