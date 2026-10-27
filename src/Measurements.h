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
    AreaOffset,
    Radius,
    Diameter,
    Angle,
    ArcLength,
    Coordinate,
    PointToLine,
    FixedDistance,
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
    AnnotationStyle annotationStyle = AnnotationStyle::Standard;
    MeasuringUnit lengthUnit = MeasuringUnit::Millimeter;
    MeasuringUnit areaUnit = MeasuringUnit::Meter;
    MeasuringUnit coordinateUnit = MeasuringUnit::Millimeter;
    int lengthPrecision = 0;
    int areaPrecision = 2;
    int coordinatePrecision = 0;
    QColor annotationColor = QColor(QStringLiteral("#37d67a"));
};

struct Measurement {
    QString id;
    QString space = QStringLiteral("模型");
    MeasurementType type = MeasurementType::Linear;
    QVector<QPointF> points;
    QPointF labelPosition;
    QString note;
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
AnnotationStyleMetrics annotationStyleMetrics(AnnotationStyle style);
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

#endif
