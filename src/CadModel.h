#ifndef CADLENS_CADMODEL_H
#define CADLENS_CADMODEL_H

#include <QColor>
#include <QHash>
#include <QImage>
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

enum class CadEntityType {
    Segment,
    Polyline,
    Circle,
    Arc,
    Ellipse,
    Text,
    Point,
    Solid
};

enum class MeasuringUnit {
    None,
    Millimeter,
    Centimeter,
    Meter,
    Kilometer,
    Inch,
    Foot
};

struct CadLayer {
    QString name;
    QColor color = QColor(QStringLiteral("#d9e1ea"));
    QColor sourceColor;
    bool visible = true;
    bool locked = false;
    int lineWeight = 31;
    int entityCount = 0;
};

struct CadDimensionEdge {
    QPointF midpoint;
    double length = 0.0;
    bool arc = false;
};

struct CadHatchPatternLine {
    QPointF origin;
    QPointF direction = QPointF(1.0, 0.0);
    QPointF offset;
    QVector<double> dashes;
};

struct CadHatchPattern {
    QString name;
    bool solid = false;
    bool gradient = false;
    QVector<CadHatchPatternLine> lines;
};

struct CadViewportDefinition {
    QPointF center;
    QSizeF windowSize;
    QPointF modelCenter;
    double viewHeight = 0.0;
    double twist = 0.0;
    bool revealsModel = true;
    QSet<QString> frozenLayers;
};

struct CadPdfPage {
    QString name;
    QRectF pageRect;
    QImage image;
    QImage printImage;
};

struct CadEntity {
    CadEntityType type = CadEntityType::Segment;
    quint64 sourceHandle = 0;
    QString layer;
    QColor color = QColor(QStringLiteral("#d9e1ea"));
    QColor sourceColor;
    int lineWeight = 31;
    QVector<QPointF> points;
    bool closed = false;
    bool filled = false;
    QPointF center;
    double radius = 0.0;
    double startAngle = 0.0;
    double endAngle = 0.0;
    QString text;
    QPointF textPosition;
    QPointF textAlignmentPoint;
    bool textFit = false;
    double textHeight = 2.5;
    double textRotation = 0.0;
    double textWidthScale = 1.0;
    int textHAlign = 0;
    int textVAlign = 0;
    int textAttachment = 0;
    bool textMirrored = false;
    int hatchGroup = -1;
    std::shared_ptr<CadHatchPattern> hatchPattern;
    QVector<CadDimensionEdge> dimensionEdges;
    QRectF clipRect;
    QRectF bounds;

    void updateBounds();
    const QVector<QLineF> &segments() const;
    mutable QVector<QLineF> segmentCache;
    mutable bool segmentCacheValid = false;
};

struct CadDocument {
    QString path;
    QString fileName;
    QString formatVersion;
    QString sourceFormat;
    QString sourceBackend;
    QVector<CadEntity> entities;
    QVector<CadLayer> layers;
    QHash<QString, int> layerIndices;
    QStringList layouts;
    QHash<QString, QVector<CadEntity>> layoutEntities;
    QHash<QString, QVector<CadViewportDefinition>> layoutViewports;
    QHash<QString, QRectF> layoutBounds;
    QVector<CadPdfPage> pdfPages;
    QRectF computedBounds;
    QRectF declaredBounds;
    QRectF bounds;
    QPointF savedViewCenter;
    double savedViewHeight = 0.0;
    double savedViewWidth = 0.0;
    double savedViewTwist = 0.0;
    bool hasSavedView = false;
    int attributeMode = -1;
    int sourceAttributeCount = 0;
    int sourceVisibleAttributeCount = 0;
    int sourceHiddenAttributeCount = 0;
    int renderedAttributeVisits = 0;
    int suppressedAttributeVisits = 0;
    int overriddenHiddenAttributeVisits = 0;
    MeasuringUnit sourceUnit = MeasuringUnit::Millimeter;
    double metersPerDrawingUnit = 0.001;
    bool calibrated = false;
    qint64 loadMilliseconds = 0;

    bool load(const QString &filePath, QString *errorMessage);
    bool isPdf() const { return sourceFormat == QStringLiteral("PDF"); }
    const CadPdfPage *pdfPage(const QString &name) const;
    CadPdfPage *pdfPage(const QString &name);
    int layerIndex(const QString &name) const;
    const CadLayer *layer(const QString &name) const;
    CadLayer *layer(const QString &name);
    void recountLayers();
};

QString measuringUnitLabel(MeasuringUnit unit);
QString measuringUnitSuffix(MeasuringUnit unit, bool squared = false);
QColor displayColorForDarkCanvas(const QColor &source);
QString drawingParsedCachePath(const QString &drawingPath);
double measuringUnitMeters(MeasuringUnit unit);
MeasuringUnit measuringUnitFromInsUnits(int insUnits);
QString formatMeasuredLength(double drawingLength, const CadDocument &document,
                             MeasuringUnit displayUnit, int precision = 3);
QString formatDimensionValue(double drawingLength,
                             const CadDocument &document,
                             MeasuringUnit displayUnit, int precision = 0);
QString formatMeasuredArea(double drawingArea, const CadDocument &document,
                           MeasuringUnit displayUnit, int precision = 3);
QString formatAreaValue(double drawingArea, const CadDocument &document,
                        MeasuringUnit displayUnit, int precision = 2);
void appendConnectedPolylinePiece(
    QVector<QVector<QPointF>> &paths, QVector<QPointF> piece,
    double tolerance = 1.0e-6);

#endif
