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
    // Rendering is controlled only by `visible`. The remaining flags are
    // preserved because they have distinct CAD semantics:
    //   off            -> layer is turned off
    //   frozen         -> globally frozen in model/paper space
    //   locked         -> editable-geometry lock; it never hides geometry
    //   frozenInNewVp  -> default freeze state for newly created viewports
    bool off = false;
    bool frozen = false;
    bool visible = true;
    bool locked = false;
    bool frozenInNewViewport = false;
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
    // Hatch style (Autodesk DXF group 75 / DWG hatch->style):
    // 0 = Normal (odd-parity, islands cut holes), 1 = Outer (outermost
    // region only), 2 = Ignore (fill through entire area, no holes).
    int style = 0;
    QVector<CadHatchPatternLine> lines;
    // Target viewer tessellation baseline:
    // a full circle uses 36 samples, explicit tolerance mode is capped at
    // 100 samples per curve. Pattern generation budgets are 100k input
    // boundary segments and 1M output segments.
    int defaultArcSegments = 36;
    int maximumArcSegments = 100;
    double toleranceFactor = 2.0e-11;
    int maximumBoundarySegments = 100000;
    int maximumPatternSegments = 1000000;
};

struct CadViewportDefinition {
    // Paper-space identity and switch state. Keep these separate from
    // revealsModel: CAD viewport status -1 means "switched on but not the
    // active edit viewport", which still displays its model contents.
    int id = 0;
    int status = 0;
    bool on = true;
    bool overallPaperViewport = false;
    QPointF center;
    QSizeF windowSize;
    QPointF modelCenter;
    double viewHeight = 0.0;
    double twist = 0.0;
    bool revealsModel = true;
    QSet<QString> frozenLayers;
    // Indices resolved from the viewport's frozen-layer hard references.
    // Preferred over names: these drawings contain duplicate layer names,
    // which a QSet<QString> cannot disambiguate, so a name-only match can
    // leave a layer visible in a viewport where VPLAYER froze it (or hide
    // an unrelated layer that happens to share the name). Empty when the
    // document came from the serialized cache, where name matching is the
    // fallback.
    QSet<int> frozenLayerIndices;
};

struct CadPdfPage {
    QString name;
    QRectF pageRect;
    QImage image;
    QImage printImage;
};

struct CadExternalReference {
    QString name;
    QString savedPath;
    QString resolvedPath;
    bool fileFound = false;
    bool loaded = false;
};

struct CadEntity {
    CadEntityType type = CadEntityType::Segment;
    quint64 sourceHandle = 0;
    QString layer;
    // Resolved index into CadDocument::layers, captured at parse time from
    // the entity's hard layer reference (handle). Lets visibility/color
    // resolution survive duplicate layer names, which a name lookup cannot
    // disambiguate. -1 means "resolve by name" (legacy/fallback path).
    int layerIndex = -1;
    // Layers of all INSERT ancestors, nearest first. A viewport freeze applies
    // when any of these block layers is frozen, even if the entity's own
    // effective layer is not. Names are retained for cache/recovery paths.
    QVector<int> blockLayerIndices;
    QStringList blockLayerNames;
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
    QVector<CadExternalReference> externalReferences;
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

    bool load(const QString &filePath, QString *errorMessage,
              const QHash<QString, QString> &externalReferencePaths = {});
    bool isPdf() const { return sourceFormat == QStringLiteral("PDF"); }
    const CadPdfPage *pdfPage(const QString &name) const;
    CadPdfPage *pdfPage(const QString &name);
    int layerIndex(const QString &name) const;
    const CadLayer *layer(const QString &name) const;
    CadLayer *layer(const QString &name);
    // Resolve an entity's layer by its precomputed index when available,
    // falling back to a (name-based) lookup. Robust against duplicate
    // layer names that share one entry in layerIndices.
    const CadLayer *layerOf(const CadEntity &entity) const;
    void recountLayers();
    void sanitizeRenderableEntities();
};

bool isRenderableCadEntity(const CadEntity &entity);
QString measuringUnitLabel(MeasuringUnit unit);
QString measuringUnitSuffix(MeasuringUnit unit, bool squared = false);
QColor aciColor(int index,
                const QColor &fallback = QColor(230, 236, 245));
QColor displayColorForDarkCanvas(const QColor &source);
bool isOverallCadViewport(const QPointF &paperCenter,
                          const QPointF &modelCenter,
                          double viewHeight,
                          double paperHeight);
QRectF cadInitialViewBounds(const QVector<CadEntity> &entities,
                           const QRectF &drawingBounds);
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
