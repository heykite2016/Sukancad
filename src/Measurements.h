#ifndef CADLENS_MEASUREMENTS_H
#define CADLENS_MEASUREMENTS_H

#include "CadModel.h"

#include <QColor>
#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <QVector>

enum class MeasurementType {
    Aligned,
    Linear,
    Continuous,
    Batch,
    Rectangle,
    Polygon,
    CircleArea,
    FilledArea,
    SideArea,
    Radius,
    Angle,
    ArcLength,
    Text
};

enum class AnnotationStyle {
    Standard,
    Architectural,
    Mechanical,
    Engineering,
    Decimal,
    Compact
};

enum class AnnotationMarker {
    FilledArrow,
    ArchitecturalTick,
    NarrowFilledArrow,
    OpenArrow,
    Dot,
    EndBar
};

enum class MeasurementSegmentKind {
    Line,
    Arc
};

// Canonical annotation geometry. A continuous/multi-segment measurement is
// stored as an ordered chain of line and arc primitives instead of relying on
// a screen-space polyline. Closed polygon annotations reuse the same chain
// with `closed == true`.
struct MeasurementSegment {
    MeasurementSegmentKind kind = MeasurementSegmentKind::Line;
    QPointF start;
    QPointF end;
    QPointF midpoint;
    QPointF center;
    double radius = 0.0;
    double startAngle = 0.0;
    double sweepAngle = 0.0;
    double length = 0.0;
};

struct AnnotationStyleMetrics {
    double textSize = 12.0;
    double arrowSize = 9.0;
    double lineWidth = 1.35;
    double extensionOffset = 4.0;
    double extensionOverrun = 12.0;
    double textGap = 11.0;
    AnnotationMarker marker = AnnotationMarker::FilledArrow;
};

struct DisplaySettings {
    // 默认建筑斜线起止符号（GB/T 50001 建筑制图标准：尺寸起止符号
    // 采用中粗斜短线），可在标注设置中切换为 ISO 箭头等其他样式。
    AnnotationStyle annotationStyle = AnnotationStyle::Standard;
    MeasuringUnit lengthUnit = MeasuringUnit::Millimeter;
    MeasuringUnit areaUnit = MeasuringUnit::Meter;
    int lengthPrecision = 0;
    int areaPrecision = 2;
    // Multiplier applied to the drawing's native dimension-text height.
    // 1.0 keeps annotation text visually identical to source dimensions.
    double dimensionTextScale = 1.0;
    // Screen-space line width shared by dimension lines, extension lines,
    // leaders and annotation markers.
    double annotationLineWidth = 1.1;
    bool annotationScaleWithDrawing = true;
    QColor annotationColor = QColor(QStringLiteral("#37d67a"));
    QColor crosshairColor = QColor(QStringLiteral("#ffffff"));
    int crosshairLength = 100;
    double crosshairLineWidth = 1.25;
    bool crosshairCenterBoxVisible = true;
    double crosshairCenterBoxSize = 10.0;
};

struct Measurement {
    QString id;
    QString space = QStringLiteral("模型");
    MeasurementType type = MeasurementType::Linear;
    QString commentLayer = QStringLiteral("默认标注");
    int styleVersion = 2;
    double textHeight = 13.0;
    double dimensionScale = 1.0;
    bool closed = false;
    QVector<QPointF> points;
    QVector<MeasurementSegment> segments;
    QPointF labelPosition;
    QString note;
    QString labelOverride;
    QVector<double> edgeArcLengths;
    QVector<double> edgeLengths;
    QVector<QPointF> edgeMidpoints;
    QVector<QPointF> edgeStarts;
    QVector<QPointF> edgeEnds;
    QColor color = QColor(QStringLiteral("#37d67a"));
    bool visible = true;
    double auxiliaryValue = 0.0;

    QString typeLabel() const;
    QString primaryValue(const CadDocument &document,
                         const DisplaySettings &settings) const;
    double rawLength() const;
    double rawArea() const;
    void normalizeGeometry();
    QJsonObject toJson() const;
    static Measurement fromJson(const QJsonObject &object);
};

QString defaultMeasurementId();
QString measurementTypeLabel(MeasurementType type);
QStringList annotationStyleLabels();
QString annotationStyleKey(AnnotationStyle style);
AnnotationStyle annotationStyleFromKey(const QString &key,
                                       AnnotationStyle fallback =
                                           AnnotationStyle::Standard);
AnnotationStyleMetrics annotationStyleMetrics(
    AnnotationStyle style, double lineWidthOverride = 0.0);
QString annotationSidecarPath(const QString &drawingPath);
QString annotationAutosavePath(const QString &drawingPath);

bool saveAnnotations(const QString &drawingPath,
                     const CadDocument &document,
                     const DisplaySettings &settings,
                     const QVector<Measurement> &measurements,
                     QString *errorMessage);
bool saveAnnotationsAutosave(const QString &drawingPath,
                             const CadDocument &document,
                             const DisplaySettings &settings,
                             const QVector<Measurement> &measurements,
                             QString *errorMessage);
bool loadAnnotations(const QString &drawingPath,
                     CadDocument &document,
                     DisplaySettings *settings,
                     QVector<Measurement> *measurements,
                     QString *errorMessage);
bool saveAnnotationsToFile(const QString &annotationPath,
                           const QString &drawingPath,
                           const CadDocument &document,
                           const DisplaySettings &settings,
                           const QVector<Measurement> &measurements,
                           QString *errorMessage);

#endif
