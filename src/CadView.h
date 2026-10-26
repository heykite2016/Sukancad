#ifndef CADLENS_CADVIEW_H
#define CADLENS_CADVIEW_H

#include "Measurements.h"
#include "SnapEngine.h"
#include "SpatialIndex.h"

#include <QHash>
#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPainterPath>
#include <QTransform>
#include <QVector>

#include <memory>

class QMouseEvent;
class QEnterEvent;
class QPainter;
class QPrinter;
class QWheelEvent;
class QKeyEvent;

class CadView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    enum class ToolMode {
        Select,
        Erase,
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
        PrintArea,
        Calibrate,
        Text,
        ZoomWindow
    };
    Q_ENUM(ToolMode)

    enum class DrawingTheme {
        CadDark,
        CadLight
    };
    Q_ENUM(DrawingTheme)

    explicit CadView(QWidget *parent = nullptr);

    void setDocument(
        std::shared_ptr<CadDocument> document,
        std::shared_ptr<SpatialIndex> preparedIndex = {},
        QHash<QString, QVector<CadEntity>> preparedLayouts = {});
    std::shared_ptr<CadDocument> document() const { return m_document; }
    QString activeSpace() const { return m_activeSpace; }
    void setActiveSpace(const QString &spaceName);
    QPointF viewCenter() const { return m_center; }
    double zoomScale() const { return m_scale; }

    const QVector<Measurement> &measurements() const { return m_measurements; }
    void setMeasurements(const QVector<Measurement> &measurements);
    void addTextAnnotation(const QPointF &position, const QString &text);
    void removeMeasurement(const QString &id);
    void removeAllMeasurements();
    void setMeasurementVisible(const QString &id, bool visible);
    void setMeasurementNote(const QString &id, const QString &note);
    void focusMeasurement(const QString &id);
    void selectMeasurement(const QString &id);

    ToolMode toolMode() const { return m_mode; }
    void beginTool(ToolMode mode, double auxiliaryValue = 0.0);
    void finishCurrentMeasurement();
    void cancelCurrentTool();
    void resetView();
    void fitToDrawing();
    void zoomAt(qreal factor, const QPoint &anchor);
    void zoomToWorldRect(const QRectF &area);
    void setDisplaySettings(const DisplaySettings &settings);
    const DisplaySettings &displaySettings() const { return m_settings; }
    double annotationTextPixelSize() const;
    void setDrawingTheme(DrawingTheme theme);
    DrawingTheme drawingTheme() const { return m_drawingTheme; }
    QColor displayColor(const QColor &source) const;
    QColor canvasBackgroundColor() const;
    void setDisplayUnit(MeasuringUnit unit);
    MeasuringUnit displayUnit() const { return m_settings.lengthUnit; }
    void setPrecision(int precision);
    void setLineWeightsVisible(bool visible);
    void setSnapModes(const SnapModes &modes);
    SnapModes snapModes() const { return m_snapModes; }
    void setSnapEnabled(bool enabled);
    bool isSnapEnabled() const { return m_snapEnabled; }
    void setGridVisible(bool visible);
    bool isGridVisible() const { return m_gridVisible; }
    void setOrthoEnabled(bool enabled);
    bool isOrthoEnabled() const { return m_orthoEnabled; }
    void applyCalibration(double knownLength, MeasuringUnit entryUnit);

    bool undo();
    bool redo();
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

    bool exportSvg(const QString &path, QString *errorMessage);
    QImage renderToImage(const QSize &size);
    QImage renderWorldRectToImage(const QRectF &area, const QSize &size);
    QImage renderPrintAreaToImage(const QRectF &area, const QSize &size,
                                  bool blackInk = true);
    static QVector<CadEntity>
    composeLayoutContent(const CadDocument &document,
                         const QVector<CadEntity> &modelEntities,
                         const QString &name);
    static QHash<QString, QVector<CadEntity>>
    prepareAllLayoutContents(const CadDocument &document);
    bool printToPrinter(QPrinter *printer, const QRectF &worldArea,
                        bool blackInk = true);
    void invalidateDrawingCache();
    void showAllMeasurements();
    void hideAllMeasurements();

signals:
    void statusMessage(const QString &message);
    void cursorWorldChanged(const QPointF &worldPoint);
    void snapChanged(const QString &snapLabel);
    void calibrationRequested(double rawLength);
    void textAnnotationRequested(const QPointF &worldPosition);
    void measurementsChanged();
    void measurementAdded(const QString &id);
    void selectionChanged(const QString &id);
    void toolChanged(CadView::ToolMode mode);
    void viewTransformChanged();
    void zoomChanged(double percent);
    void activeSpaceChanged(const QString &spaceName);
    void printAreaSelected(const QRectF &worldArea);

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QPointF worldToScreen(const QPointF &world) const;
    QPointF screenToWorld(const QPointF &screen) const;
    QRectF visibleWorldRect() const;
    double snapRadiusWorld() const;
    void updateSnap(const QPointF &screenPoint);
    void leftClick(const QPointF &screenPoint);
    void appendDraftPoint(const QPointF &worldPoint);
    void finishDraftForLabel();
    bool commitDraft(const QPointF &labelWorld);
    void updateAnnotationGeometryFromDraft(Measurement &measurement) const;
    std::optional<Measurement> draftMeasurement(
        const QPointF &labelWorld) const;
    int measurementIndex(const QString &id) const;
    QString hitMeasurementLabel(const QPointF &screenPoint) const;
    QString hitMeasurementAt(const QPointF &screenPoint) const;
    void pushUndo();
    void clearDraft();
    QString toolPrompt() const;

    void renderScene(QPainter &painter, const QSize &viewport,
                     bool exporting = false);
    void drawBackground(QPainter &painter, const QSize &viewport,
                        bool exporting);
    void drawPdfPage(QPainter &painter);
    void drawCadEntities(QPainter &painter, const QSize &viewport);
    void drawCadText(QPainter &painter, const CadEntity &entity);
    void drawDraft(QPainter &painter);
    void drawEraseRectangle(QPainter &painter);
    void drawMeasurements(QPainter &painter);
    void drawMeasurement(QPainter &painter,
                         const Measurement &measurement, bool selected);
    void drawVectorLabel(QPainter &painter, const QString &text,
                         const QPointF &anchor, const QColor &color,
                         QRectF *hitRect = nullptr,
                         bool emphasized = false);
    void drawDimensionText(QPainter &painter, const QString &text,
                           const QPointF &anchor, const QColor &color,
                           double angleDegrees = 0.0,
                           QRectF *hitRect = nullptr,
                           bool emphasized = false);
    void drawArrow(QPainter &painter, const QPointF &tip,
                   const QPointF &toward, const QColor &color);
    QColor printColor(const QColor &source) const;
    QColor printColor(const CadEntity &entity) const;
    void drawSnapMarker(QPainter &painter, const QPointF &screenPoint,
                        SnapKind kind);
    void drawDeleteHoverMarker(QPainter &painter);
    void rebuildPanCache();
    void schedulePanCache();
    void drawPanCache(QPainter &painter);
    void invalidatePanCache();
    void invalidateHatchCache();
    void invalidateVectorCache();
    std::optional<Measurement> closedEntityMeasurementAt(
        const QPointF &screenPoint, MeasurementType type) const;
    std::optional<QLineF> lineEntityAt(
        const QPointF &screenPoint) const;
    std::optional<QLineF> circularRadiusAt(
        const QPointF &screenPoint) const;
    QVector<CadEntity> buildLayoutContent(const QString &name) const;
    bool traceArcBetween(const QPointF &start, const QPointF &end,
                         QVector<QPointF> *path, double *length,
                         QPointF *midpoint) const;
    QPointF areaInteriorPoint(const QVector<QPointF> &vertices) const;
    double dimensionReferenceTextHeightWorld() const;
    double annotationGeometryScale() const;

    std::shared_ptr<CadDocument> m_document;
    SpatialIndex m_spatialIndex;
    SnapEngine m_snapEngine;
    SnapModes m_snapModes;
    bool m_snapEnabled = true;
    bool m_gridVisible = false;
    bool m_orthoEnabled = false;
    QPointF m_center;
    double m_scale = 1.0;
    double m_fitScale = 1.0;
    QPointF m_cursorWorld;
    QPointF m_cursorScreen;
    SnapResult m_snap;
    bool m_haveCursor = false;
    bool m_initialFitPending = true;
    QString m_activeSpace = QStringLiteral("模型");
    QVector<CadEntity> m_modelEntities;
    QRectF m_modelBounds;
    QHash<QString, QVector<CadEntity>> m_layoutCache;
    struct SavedSpaceView {
        QPointF center;
        double scale = 0.0;
        double fitScale = 1.0;
    };
    QHash<QString, SavedSpaceView> m_spaceViews;

    ToolMode m_mode = ToolMode::Select;
    QVector<QPointF> m_draftPoints;
    bool m_awaitingLabel = false;
    bool m_polygonClosurePrompted = false;
    QPointF m_labelPreview;

    bool m_panning = false;
    bool m_leftPressPending = false;
    QPoint m_leftPressScreen;
    QPoint m_lastMousePosition;
    bool m_dragLabel = false;
    QString m_dragMeasurementId;
    QString m_hoveredMeasurementId;
    QPointF m_dragStartWorld;
    bool m_spaceDown = false;
    bool m_eraseDragging = false;
    QPointF m_eraseStartScreen;
    QRectF m_eraseScreenRect;

    DisplaySettings m_settings;
    DrawingTheme m_drawingTheme = DrawingTheme::CadDark;
    double m_toolAuxiliaryValue = 0.0;
    bool m_lineWeightsVisible = true;
    bool m_printStyle = false;
    bool m_printBlackInk = true;
    QSizeF m_renderSize;
    double m_pendingCalibrationRawLength = 0.0;
    QVector<QPointF> m_calibrationPoints;
    QString m_selectedMeasurementId;
    QVector<Measurement> m_measurements;
    QHash<QString, QRectF> m_labelHitRects;
    QImage m_panCache;
    QImage m_hatchCache;
    QSize m_hatchCacheSize;
    qsizetype m_hatchCacheSegments = 0;
    bool m_hatchCacheValid = false;
    QImage m_vectorCache;
    QSize m_vectorCacheSize;
    bool m_vectorCacheValid = false;
    bool m_buildingVectorCache = false;
    QHash<QString, QPainterPath> m_cadTextPathCache;
    mutable QHash<QString, double> m_dimensionTextHeights;
    QPointF m_panCacheCenter;
    double m_panCacheScale = 1.0;
    bool m_panCacheValid = false;
    bool m_buildingPanCache = false;
    bool m_zoomPreviewActive = false;
    quint64 m_zoomPreviewGeneration = 0;
    quint64 m_sceneCacheGeneration = 0;
    QVector<QVector<Measurement>> m_undoStack;
    QVector<QVector<Measurement>> m_redoStack;
};

#endif
