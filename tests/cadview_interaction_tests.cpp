#include "CadView.h"

#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QTemporaryDir>
#include <QWheelEvent>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {

void mouseClick(CadView &view, const QPointF &position,
                QEvent::Type type = QEvent::MouseButtonPress)
{
    QMouseEvent event(type, position, position, position, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
    if (type == QEvent::MouseButtonPress) {
        QMouseEvent release(QEvent::MouseButtonRelease, position, position,
                            position, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(&view, &release);
    }
}

bool near(double a, double b)
{
    return std::fabs(a - b) < 1.0e-9;
}

int fail(const char *message)
{
    std::fprintf(stderr, "cadview_interaction_tests: %s\n", message);
    return 1;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);

    auto document = std::make_shared<CadDocument>();
    document->path = QStringLiteral("memory.dxf");
    document->fileName = QStringLiteral("interaction.dxf");
    document->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    document->metersPerDrawingUnit = 0.001;
    CadLayer layer;
    layer.name = QStringLiteral("WALL");
    document->layers.append(layer);

    CadEntity horizontal;
    horizontal.layer = layer.name;
    horizontal.type = CadEntityType::Segment;
    horizontal.points = {QPointF(100.0, 400.0), QPointF(900.0, 400.0)};
    horizontal.updateBounds();
    document->entities.append(horizontal);

    CadEntity vertical;
    vertical.layer = layer.name;
    vertical.type = CadEntityType::Segment;
    vertical.points = {QPointF(500.0, 100.0), QPointF(500.0, 700.0)};
    vertical.updateBounds();
    document->entities.append(vertical);

    SpatialIndex index;
    index.rebuild(*document);
    SnapEngine snap;
    snap.setDocument(document.get(), &index);
    SnapModes modes;
    const SnapResult intersection = snap.findSnap(QPointF(500.0, 400.0), 8.0,
                                                  modes);
    if (!intersection.valid || intersection.kind != SnapKind::Intersection
        || !near(intersection.point.x(), 500.0)
        || !near(intersection.point.y(), 400.0)) {
        return fail("intersection snap failed");
    }
    const SnapResult endpoint = snap.findSnap(QPointF(103.0, 402.0), 8.0,
                                              modes);
    if (!endpoint.valid || endpoint.kind != SnapKind::Endpoint
        || !near(endpoint.point.x(), 100.0)) {
        return fail("endpoint priority failed");
    }

    CadView view;
    view.resize(900, 700);
    if (view.isGridVisible()) {
        return fail("grid is visible by default");
    }
    view.setDocument(document);
    view.fitToDrawing();
    const double zoomBefore = view.zoomScale();
    QWheelEvent wheelZoom(QPointF(450.0, 350.0), QPointF(450.0, 350.0),
                          QPoint(), QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &wheelZoom);
    if (!(view.zoomScale() > zoomBefore)) {
        return fail("wheel zoom failed");
    }
    const QPointF centerBeforePan = view.viewCenter();
    view.beginTool(CadView::ToolMode::Select);
    QMouseEvent dragPress(QEvent::MouseButtonPress,
                          QPointF(420.0, 330.0),
                          QPointF(420.0, 330.0),
                          QPointF(420.0, 330.0), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &dragPress);
    QMouseEvent dragMove(QEvent::MouseMove, QPointF(470.0, 380.0),
                         QPointF(470.0, 380.0), QPointF(470.0, 380.0),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &dragMove);
    QMouseEvent dragRelease(QEvent::MouseButtonRelease,
                            QPointF(470.0, 380.0),
                            QPointF(470.0, 380.0),
                            QPointF(470.0, 380.0), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &dragRelease);
    const QPointF centerAfterPan = view.viewCenter();
    if (!(centerAfterPan.x() < centerBeforePan.x()
          && centerAfterPan.y() > centerBeforePan.y())) {
        return fail("pan direction does not follow the cursor");
    }
    view.beginTool(CadView::ToolMode::Linear);
    mouseClick(view, QPointF(200.0, 250.0));
    mouseClick(view, QPointF(600.0, 250.0));
    mouseClick(view, QPointF(420.0, 350.0));
    if (view.measurements().size() != 1) {
        return fail("linear measurement interaction failed");
    }
    const Measurement &linear = view.measurements().first();
    if (linear.type != MeasurementType::Linear
        || linear.points.size() != 2 || linear.rawLength() <= 0.0) {
        return fail("linear measurement result invalid");
    }

    view.beginTool(CadView::ToolMode::Rectangle);
    mouseClick(view, QPointF(200.0, 200.0));
    mouseClick(view, QPointF(500.0, 400.0));
    mouseClick(view, QPointF(350.0, 300.0));
    if (view.measurements().size() != 2
        || view.measurements().last().rawArea() <= 0.0) {
        return fail("rectangle area interaction failed");
    }

    view.beginTool(CadView::ToolMode::Polygon);
    mouseClick(view, QPointF(240.0, 220.0));
    mouseClick(view, QPointF(520.0, 220.0));
    mouseClick(view, QPointF(520.0, 420.0));
    mouseClick(view, QPointF(241.0, 221.0));
    mouseClick(view, QPointF(380.0, 320.0));
    if (view.measurements().size() != 3
        || view.measurements().last().type != MeasurementType::Polygon) {
        return fail("polygon closure or commit failed");
    }

    view.beginTool(CadView::ToolMode::Angle);
    mouseClick(view, QPointF(350.0, 350.0));
    mouseClick(view, QPointF(500.0, 350.0));
    mouseClick(view, QPointF(350.0, 200.0));
    mouseClick(view, QPointF(430.0, 300.0));
    if (view.measurements().size() != 4
        || view.measurements().last().type != MeasurementType::Angle) {
        return fail("angle interaction failed");
    }

    if (!view.undo() || view.measurements().size() != 3) {
        return fail("undo failed");
    }
    if (!view.redo() || view.measurements().size() != 4) {
        return fail("redo failed");
    }
    DisplaySettings display = view.displaySettings();
    if (display.annotationColor != QColor(QStringLiteral("#37d67a"))) {
        return fail("default annotation color is not green");
    }
    if (display.annotationStyle != AnnotationStyle::Architectural) {
        return fail("default annotation style is not architectural");
    }
    display.annotationStyle = AnnotationStyle::Architectural;
    display.lengthUnit = MeasuringUnit::Millimeter;
    display.lengthPrecision = 0;
    display.annotationColor = QColor(QStringLiteral("#33cc99"));
    view.setDisplaySettings(display);
    for (const Measurement &measurement : view.measurements()) {
        if (measurement.color != display.annotationColor) {
            return fail("annotation line/text color was not applied");
        }
    }
    const QString linearText =
        view.measurements().first().primaryValue(*document, display);
    const QString rectangleText =
        view.measurements()[1].primaryValue(*document, display);
    if (linearText.contains(QStringLiteral("mm"))
        || !rectangleText.startsWith(QStringLiteral("面积："))
        || !rectangleText.contains(QStringLiteral("周长："))) {
        return fail("CAD measurement text format failed");
    }

    QRectF selectedPrintArea;
    QObject::connect(
        &view, &CadView::printAreaSelected,
        [&selectedPrintArea](const QRectF &area) {
            selectedPrintArea = area;
        });
    view.beginTool(CadView::ToolMode::PrintArea);
    mouseClick(view, QPointF(150.0, 180.0));
    mouseClick(view, QPointF(650.0, 520.0));
    if (!selectedPrintArea.isValid()) {
        return fail("print area selection failed");
    }
    const QImage printPreview =
        view.renderPrintAreaToImage(
            selectedPrintArea, QSize(842, 595));
    if (printPreview.isNull()
        || printPreview.pixelColor(20, 20) != QColor(Qt::white)) {
        return fail("print preview page rendering failed");
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid()) {
        return fail("temporary directory failed");
    }
    const QString pdfPath =
        temporaryDirectory.filePath(QStringLiteral("selection.pdf"));
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(pdfPath);
    if (!view.printToPrinter(&printer, selectedPrintArea)) {
        return fail("selected print rendering failed");
    }
    const QFileInfo pdfInfo(pdfPath);
    if (!pdfInfo.exists() || pdfInfo.size() < 1000) {
        return fail("printed PDF was not produced");
    }

    auto printTextDocument = std::make_shared<CadDocument>();
    printTextDocument->path = QStringLiteral("print-text.dxf");
    printTextDocument->fileName = QStringLiteral("print-text.dxf");
    printTextDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer printTextLayer;
    printTextLayer.name = QStringLiteral("PRINT_TEXT");
    printTextDocument->layers.append(printTextLayer);
    CadEntity printableText;
    printableText.layer = printTextLayer.name;
    printableText.type = CadEntityType::Text;
    printableText.text = QStringLiteral("PRINT TEXT 12345");
    printableText.textPosition = QPointF(500.0, 400.0);
    printableText.textHeight = 32.0;
    printableText.updateBounds();
    printTextDocument->entities.append(printableText);
    CadView printTextView;
    printTextView.resize(900, 700);
    printTextView.setDocument(printTextDocument);
    Measurement printedDimension;
    printedDimension.id = QStringLiteral("printed-dimension");
    printedDimension.type = MeasurementType::Linear;
    printedDimension.points = {
        QPointF(250.0, 250.0), QPointF(750.0, 250.0)
    };
    printedDimension.labelPosition = QPointF(500.0, 180.0);
    printedDimension.color = QColor(QStringLiteral("#37d67a"));
    printTextView.setMeasurements({printedDimension});
    const QString textPdfPath =
        temporaryDirectory.filePath(QStringLiteral("text-output.pdf"));
    QPrinter textPrinter(QPrinter::HighResolution);
    textPrinter.setResolution(300);
    textPrinter.setOutputFormat(QPrinter::PdfFormat);
    textPrinter.setOutputFileName(textPdfPath);
    textPrinter.setPageSize(QPageSize(QPageSize::A4));
    if (!printTextView.printToPrinter(
            &textPrinter, QRectF(0.0, 0.0, 1000.0, 800.0))) {
        return fail("text and dimension PDF print failed");
    }
    CadDocument renderedPrintPdf;
    QString renderedPrintError;
    if (!renderedPrintPdf.load(textPdfPath, &renderedPrintError)
        || renderedPrintPdf.pdfPages.isEmpty()
        || renderedPrintPdf.pdfPages.first().image.isNull()) {
        return fail("printed PDF text could not be rasterized for verification");
    }
    const QImage renderedPrintPage =
        renderedPrintPdf.pdfPages.first().image;
    if (qEnvironmentVariableIsSet("CADLENS_TEST_IMAGE_DIR")) {
        const QString testDirectory =
            qEnvironmentVariable("CADLENS_TEST_IMAGE_DIR");
        QFile::copy(
            textPdfPath,
            testDirectory + QStringLiteral("/cad-text-print.pdf"));
        renderedPrintPage.save(
            testDirectory + QStringLiteral("/cad-text-print.png"));
    }
    int printedInkPixels = 0;
    QRect printedInkBounds;
    for (int y = 0; y < renderedPrintPage.height(); ++y) {
        for (int x = 0; x < renderedPrintPage.width(); ++x) {
            const QColor color = renderedPrintPage.pixelColor(x, y);
            if (color.red() < 250 || color.green() < 250
                || color.blue() < 250) {
                ++printedInkPixels;
                printedInkBounds = printedInkBounds.isNull()
                    ? QRect(x, y, 1, 1)
                    : printedInkBounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    if (printedInkPixels < 500) {
        std::fprintf(stderr, "printed ink pixels=%d bounds=%d,%d %dx%d\n",
                     printedInkPixels, printedInkBounds.x(),
                     printedInkBounds.y(), printedInkBounds.width(),
                     printedInkBounds.height());
        return fail("printed PDF omitted text or dimension graphics");
    }

    CadView noFillView;
    noFillView.resize(900, 700);
    noFillView.setDocument(document);
    noFillView.fitToDrawing();
    const QImage sceneBeforeRectangle =
        noFillView.renderToImage(QSize(900, 700));
    noFillView.beginTool(CadView::ToolMode::Rectangle);
    mouseClick(noFillView, QPointF(225.0, 500.0));
    mouseClick(noFillView, QPointF(675.0, 200.0));
    mouseClick(noFillView, QPointF(750.0, 575.0));
    const QImage sceneAfterRectangle =
        noFillView.renderToImage(QSize(900, 700));
    for (int y = 235; y < 465; ++y) {
        for (int x = 260; x < 640; ++x) {
            if (sceneBeforeRectangle.pixel(x, y)
                != sceneAfterRectangle.pixel(x, y)) {
                return fail("rectangle measurement filled its interior");
            }
        }
    }

    CadView gestureView;
    gestureView.resize(900, 700);
    gestureView.setDocument(document);
    gestureView.fitToDrawing();
    gestureView.beginTool(CadView::ToolMode::Linear);
    const QPointF centerBeforeToolPan = gestureView.viewCenter();
    QMouseEvent toolPanPress(QEvent::MouseButtonPress,
                             QPointF(300.0, 300.0),
                             QPointF(300.0, 300.0),
                             QPointF(300.0, 300.0), Qt::LeftButton,
                             Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&gestureView, &toolPanPress);
    QMouseEvent toolPanMove(QEvent::MouseMove, QPointF(380.0, 350.0),
                            QPointF(380.0, 350.0),
                            QPointF(380.0, 350.0), Qt::NoButton,
                            Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&gestureView, &toolPanMove);
    QMouseEvent toolPanRelease(QEvent::MouseButtonRelease,
                               QPointF(380.0, 350.0),
                               QPointF(380.0, 350.0),
                               QPointF(380.0, 350.0), Qt::LeftButton,
                               Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&gestureView, &toolPanRelease);
    if (gestureView.viewCenter() == centerBeforeToolPan
        || !gestureView.measurements().isEmpty()) {
        return fail("left-drag pan during measurement failed");
    }
    const auto worldAtScreen = [](const CadView &view,
                                  const QPointF &screen) {
        const QSizeF viewport(view.size());
        return QPointF(
            (screen.x() - viewport.width() * 0.5) / view.zoomScale()
                + view.viewCenter().x(),
            (viewport.height() * 0.5 - screen.y()) / view.zoomScale()
                + view.viewCenter().y());
    };
    const QPointF zoomAnchor(315.0, 265.0);
    const QPointF worldUnderCursorBefore =
        worldAtScreen(gestureView, zoomAnchor);
    const double toolZoomBefore = gestureView.zoomScale();
    QWheelEvent measurementZoom(
        zoomAnchor, zoomAnchor, QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&gestureView, &measurementZoom);
    const QPointF worldUnderCursorAfter =
        worldAtScreen(gestureView, zoomAnchor);
    if (!(gestureView.zoomScale() > toolZoomBefore)
        || !near(worldUnderCursorBefore.x(),
                 worldUnderCursorAfter.x())
        || !near(worldUnderCursorBefore.y(),
                 worldUnderCursorAfter.y())) {
        return fail("wheel zoom did not preserve the measurement cursor");
    }
    mouseClick(gestureView, QPointF(320.0, 260.0));
    mouseClick(gestureView, QPointF(520.0, 260.0));
    mouseClick(gestureView, QPointF(420.0, 330.0));
    if (gestureView.measurements().size() != 1
        || gestureView.measurements().first().type
            != MeasurementType::Linear) {
        return fail("measurement clicks broke after pan and zoom");
    }
    gestureView.beginTool(CadView::ToolMode::Continuous);
    mouseClick(gestureView, QPointF(160.0, 170.0));
    mouseClick(gestureView, QPointF(360.0, 170.0));
    mouseClick(gestureView, QPointF(360.0, 320.0));
    QKeyEvent escapeMeasurement(QEvent::KeyPress, Qt::Key_Escape,
                                Qt::NoModifier);
    QApplication::sendEvent(&gestureView, &escapeMeasurement);
    mouseClick(gestureView, QPointF(500.0, 260.0));
    if (gestureView.measurements().size() != 2
        || gestureView.measurements().last().type
            != MeasurementType::Continuous
        || !gestureView.measurements().last()
                .primaryValue(*document, display)
                .startsWith(QStringLiteral("总长："))) {
        return fail("Escape did not finish the current measurement path");
    }

    CadView circleView;
    circleView.resize(900, 700);
    circleView.setDocument(document);
    circleView.fitToDrawing();
    circleView.beginTool(CadView::ToolMode::CircleArea);
    mouseClick(circleView, QPointF(600.0, 350.0));
    mouseClick(circleView, QPointF(450.0, 200.0));
    mouseClick(circleView, QPointF(300.0, 350.0));
    mouseClick(circleView, QPointF(470.0, 330.0));
    if (circleView.measurements().size() != 1) {
        return fail("circle measurement failed");
    }
    const Measurement &threePointCircle =
        circleView.measurements().first();
    if (!near(threePointCircle.points[0].x(), 500.0)
        || !near(threePointCircle.points[0].y(), 400.0)
        || !near(threePointCircle.rawLength(),
                 2.0 * 3.14159265358979323846 * 200.0)
        || QLineF(threePointCircle.points[0],
                  threePointCircle.labelPosition).length() > 111.0) {
        return fail("three-point circle center or circumference is incorrect");
    }
    const QString circleText =
        circleView.measurements().first().primaryValue(
            *document, circleView.displaySettings());
    if (!circleText.contains(QStringLiteral("圆心："))
        || !circleText.contains(QStringLiteral("半径："))
        || !circleText.contains(QStringLiteral("面积："))
        || !circleText.contains(QStringLiteral("周长："))) {
        return fail("circle measurement omitted required values");
    }

    CadView lineDistanceView;
    lineDistanceView.resize(900, 700);
    lineDistanceView.setDocument(document);
    lineDistanceView.fitToDrawing();
    lineDistanceView.beginTool(CadView::ToolMode::PointToLine);
    mouseClick(lineDistanceView, QPointF(450.0, 500.0));
    mouseClick(lineDistanceView, QPointF(600.0, 350.0));
    mouseClick(lineDistanceView, QPointF(650.0, 250.0));
    if (lineDistanceView.measurements().size() != 1) {
        return fail("point-to-line entity selection failed");
    }
    const Measurement &pointToLine =
        lineDistanceView.measurements().first();
    if (pointToLine.points.size() != 3
        || !near(pointToLine.rawLength(), 200.0)) {
        return fail("point-to-line perpendicular distance is incorrect");
    }

    auto circleDocument = std::make_shared<CadDocument>();
    circleDocument->path = QStringLiteral("circle.dxf");
    circleDocument->fileName = QStringLiteral("circle.dxf");
    circleDocument->bounds = QRectF(200.0, 100.0, 600.0, 600.0);
    circleDocument->metersPerDrawingUnit = 0.001;
    CadLayer circleLayer;
    circleLayer.name = QStringLiteral("CIRCLE");
    circleDocument->layers.append(circleLayer);
    CadEntity circleEntity;
    circleEntity.layer = circleLayer.name;
    circleEntity.type = CadEntityType::Circle;
    circleEntity.center = QPointF(500.0, 400.0);
    circleEntity.radius = 200.0;
    circleEntity.closed = true;
    for (int i = 0; i <= 64; ++i) {
        const double angle = 2.0 * 3.14159265358979323846
            * double(i) / 64.0;
        circleEntity.points.append(
            circleEntity.center
            + QPointF(std::cos(angle), std::sin(angle)) * 200.0);
    }
    circleEntity.updateBounds();
    circleDocument->entities.append(circleEntity);
    CadView circleEntityView;
    circleEntityView.resize(900, 700);
    circleEntityView.setDocument(circleDocument);
    circleEntityView.fitToDrawing();
    circleEntityView.beginTool(CadView::ToolMode::CircleArea);
    mouseClick(circleEntityView, QPointF(650.0, 350.0));
    mouseClick(circleEntityView, QPointF(450.0, 150.0));
    mouseClick(circleEntityView, QPointF(250.0, 350.0));
    mouseClick(circleEntityView, QPointF(470.0, 330.0));
    if (circleEntityView.measurements().size() != 1) {
        return fail("circle entity automatic center detection failed");
    }
    const Measurement &detectedCircle =
        circleEntityView.measurements().first();
    if (std::fabs(detectedCircle.points[0].x() - 500.0) > 1.5
        || std::fabs(detectedCircle.points[0].y() - 400.0) > 1.5
        || std::fabs(detectedCircle.rawArea()
                     - 3.14159265358979323846 * 200.0 * 200.0)
            > 1000.0) {
        std::fprintf(stderr,
                     "circle center=(%.6f, %.6f) rim=(%.6f, %.6f) "
                     "radius=%.6f area=%.6f\n",
                     detectedCircle.points[0].x(),
                     detectedCircle.points[0].y(),
                     detectedCircle.points[1].x(),
                     detectedCircle.points[1].y(),
                     QLineF(detectedCircle.points[0],
                            detectedCircle.points[1]).length(),
                     detectedCircle.rawArea());
        return fail("circle entity radius or area is incorrect");
    }

    auto spaceDocument = std::make_shared<CadDocument>();
    spaceDocument->path = QStringLiteral("spaces.dxf");
    spaceDocument->fileName = QStringLiteral("spaces.dxf");
    spaceDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer spaceLayer;
    spaceLayer.name = QStringLiteral("SPACE");
    spaceDocument->layers.append(spaceLayer);
    CadEntity spaceLine;
    spaceLine.layer = spaceLayer.name;
    spaceLine.type = CadEntityType::Segment;
    spaceLine.points = {
        QPointF(100.0, 400.0), QPointF(900.0, 400.0)
    };
    spaceLine.updateBounds();
    spaceDocument->entities.append(spaceLine);
    spaceDocument->layouts.append(QStringLiteral("布局视图"));
    spaceDocument->layoutEntities.insert(
        QStringLiteral("布局视图"), QVector<CadEntity>{spaceLine});
    spaceDocument->layoutBounds.insert(
        QStringLiteral("布局视图"), QRectF(0.0, 0.0, 600.0, 500.0));
    CadView spaceView;
    spaceView.resize(900, 700);
    spaceView.setDocument(spaceDocument);
    spaceView.fitToDrawing();
    spaceView.zoomAt(1.25, QPoint(350, 280));
    const QPointF modelCenter =
        spaceView.viewCenter();
    const double modelScale = spaceView.zoomScale();
    spaceView.setActiveSpace(QStringLiteral("布局视图"));
    spaceView.zoomAt(0.8, QPoint(500, 300));
    const QPointF layoutCenter = spaceView.viewCenter();
    const double layoutScale = spaceView.zoomScale();
    spaceView.setActiveSpace(QStringLiteral("模型"));
    if (!near(spaceView.viewCenter().x(), modelCenter.x())
        || !near(spaceView.viewCenter().y(), modelCenter.y())
        || !near(spaceView.zoomScale(), modelScale)) {
        return fail("model view was reset while switching layouts");
    }
    spaceView.setActiveSpace(QStringLiteral("布局视图"));
    if (!near(spaceView.viewCenter().x(), layoutCenter.x())
        || !near(spaceView.viewCenter().y(), layoutCenter.y())
        || !near(spaceView.zoomScale(), layoutScale)) {
        return fail("layout view was reset while switching spaces");
    }

    auto frozenLayoutDocument = std::make_shared<CadDocument>();
    frozenLayoutDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer frozenLayer;
    frozenLayer.name = QStringLiteral("FROZEN_FILL");
    frozenLayoutDocument->layers.append(frozenLayer);
    CadEntity frozenEntity;
    frozenEntity.layer = frozenLayer.name;
    frozenEntity.type = CadEntityType::Polyline;
    frozenEntity.closed = true;
    frozenEntity.points = {
        QPointF(100.0, 100.0), QPointF(500.0, 100.0),
        QPointF(500.0, 500.0), QPointF(100.0, 500.0)
    };
    frozenEntity.updateBounds();
    frozenLayoutDocument->entities.append(frozenEntity);
    frozenLayoutDocument->layouts.append(QStringLiteral("冻结布局"));
    frozenLayoutDocument->layoutEntities.insert(
        QStringLiteral("冻结布局"), QVector<CadEntity>{});
    CadViewportDefinition frozenViewport;
    frozenViewport.center = QPointF(500.0, 400.0);
    frozenViewport.windowSize = QSizeF(1000.0, 800.0);
    frozenViewport.modelCenter = QPointF(500.0, 400.0);
    frozenViewport.viewHeight = 800.0;
    frozenViewport.frozenLayers.insert(frozenLayer.name);
    frozenLayoutDocument->layoutViewports.insert(
        QStringLiteral("冻结布局"),
        QVector<CadViewportDefinition>{frozenViewport});
    if (!CadView::composeLayoutContent(
             *frozenLayoutDocument, frozenLayoutDocument->entities,
             QStringLiteral("冻结布局")).isEmpty()) {
        return fail("layout ignored frozen-layer visibility");
    }

    CadView orthoView;
    orthoView.resize(900, 700);
    orthoView.setDocument(document);
    orthoView.fitToDrawing();
    orthoView.setOrthoEnabled(true);
    orthoView.beginTool(CadView::ToolMode::Linear);
    mouseClick(orthoView, QPointF(200.0, 220.0));
    mouseClick(orthoView, QPointF(520.0, 280.0));
    mouseClick(orthoView, QPointF(420.0, 340.0));
    if (orthoView.measurements().size() != 1
        || !near(orthoView.measurements().first().points[0].y(),
                 orthoView.measurements().first().points[1].y())) {
        return fail("CAD ortho measurement failed");
    }
    orthoView.setGridVisible(false);
    if (orthoView.isGridVisible()) {
        return fail("CAD grid toggle failed");
    }

    CadView zoomWindowView;
    zoomWindowView.resize(900, 700);
    zoomWindowView.setDocument(document);
    zoomWindowView.fitToDrawing();
    const double zoomWindowScaleBefore = zoomWindowView.zoomScale();
    zoomWindowView.beginTool(CadView::ToolMode::ZoomWindow);
    mouseClick(zoomWindowView, QPointF(280.0, 220.0));
    mouseClick(zoomWindowView, QPointF(520.0, 410.0));
    if (!(zoomWindowView.zoomScale() > zoomWindowScaleBefore)
        || zoomWindowView.toolMode() != CadView::ToolMode::Select) {
        return fail("CAD window zoom failed");
    }

    view.beginTool(CadView::ToolMode::Erase);
    mouseClick(view, QPointF(400.0, 350.0));
    if (view.measurements().size() != 3) {
        return fail("annotation geometry click deletion failed");
    }

    view.beginTool(CadView::ToolMode::Continuous);
    mouseClick(view, QPointF(150.0, 150.0));
    mouseClick(view, QPointF(350.0, 150.0));
    mouseClick(view, QPointF(350.0, 300.0));
    view.finishCurrentMeasurement();
    mouseClick(view, QPointF(500.0, 250.0));
    if (view.measurements().size() != 4
        || view.measurements().last().type
            != MeasurementType::Continuous
        || !view.measurements().last().primaryValue(*document, display)
                .startsWith(QStringLiteral("总长："))) {
        return fail("continuous measurement finish flow failed");
    }

    const auto runTwoPointTool = [&](CadView::ToolMode mode,
                                     MeasurementType expected) {
        const int before = view.measurements().size();
        view.beginTool(mode);
        mouseClick(view, QPointF(180.0, 180.0));
        mouseClick(view, QPointF(520.0, 260.0));
        mouseClick(view, QPointF(430.0, 240.0));
        return view.measurements().size() == before + 1
            && view.measurements().last().type == expected;
    };
    if (!runTwoPointTool(CadView::ToolMode::Aligned,
                         MeasurementType::Aligned)
        || !runTwoPointTool(CadView::ToolMode::Diameter,
                            MeasurementType::Diameter)) {
        return fail("two-point measurement tool failed");
    }

    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::Radius);
        mouseClick(view, QPointF(550.0, 350.0));
        mouseClick(view, QPointF(450.0, 250.0));
        mouseClick(view, QPointF(350.0, 350.0));
        mouseClick(view, QPointF(610.0, 250.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::Radius
            || view.measurements().last().points.size() != 2
            || !near(view.measurements().last().rawLength(),
                     100.0 / view.zoomScale())) {
            return fail("three-point radius measurement failed");
        }
    }

    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::ArcLength);
        mouseClick(view, QPointF(180.0, 500.0));
        mouseClick(view, QPointF(360.0, 430.0));
        mouseClick(view, QPointF(540.0, 500.0));
        mouseClick(view, QPointF(360.0, 520.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::ArcLength) {
            return fail("arc length measurement failed");
        }
    }
    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::Coordinate);
        mouseClick(view, QPointF(420.0, 420.0));
        mouseClick(view, QPointF(500.0, 380.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::Coordinate) {
            return fail("coordinate measurement failed");
        }
    }
    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::PointToLine);
        mouseClick(view, QPointF(300.0, 300.0));
        mouseClick(view, QPointF(400.0, 470.0));
        mouseClick(view, QPointF(700.0, 500.0));
        mouseClick(view, QPointF(560.0, 360.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::PointToLine) {
            return fail("point-to-line measurement failed");
        }
    }
    view.beginTool(CadView::ToolMode::FixedDistance, 100.0);
    mouseClick(view, QPointF(180.0, 180.0));
    mouseClick(view, QPointF(520.0, 260.0));
    mouseClick(view, QPointF(430.0, 240.0));
    if (view.measurements().last().type
            != MeasurementType::FixedDistance
        || view.measurements().last().rawLength() <= 0.0) {
        return fail("fixed-distance measurement failed");
    }
    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::SideArea, 50.0);
        mouseClick(view, QPointF(180.0, 180.0));
        mouseClick(view, QPointF(520.0, 260.0));
        mouseClick(view, QPointF(430.0, 240.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::SideArea
            || view.measurements().last().rawArea() <= 0.0) {
            return fail("side-area measurement failed");
        }
    }
    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::Batch);
        mouseClick(view, QPointF(160.0, 200.0));
        mouseClick(view, QPointF(320.0, 250.0));
        mouseClick(view, QPointF(480.0, 300.0));
        view.finishCurrentMeasurement();
        mouseClick(view, QPointF(500.0, 350.0));
        if (view.measurements().size() != before + 2) {
            return fail("batch measurement failed");
        }
    }
    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::AreaOffset, 20.0);
        mouseClick(view, QPointF(190.0, 210.0));
        mouseClick(view, QPointF(500.0, 210.0));
        mouseClick(view, QPointF(500.0, 450.0));
        view.finishCurrentMeasurement();
        mouseClick(view, QPointF(400.0, 330.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::AreaOffset
            || view.measurements().last().rawArea() <= 0.0) {
            return fail("offset-area measurement failed");
        }
    }

    CadEntity filledBoundary;
    filledBoundary.type = CadEntityType::Polyline;
    filledBoundary.layer = layer.name;
    filledBoundary.closed = true;
    filledBoundary.filled = true;
    filledBoundary.points = {
        QPointF(100.0, 100.0), QPointF(700.0, 100.0),
        QPointF(700.0, 600.0), QPointF(100.0, 600.0)
    };
    filledBoundary.updateBounds();
    document->entities.append(filledBoundary);
    document->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    view.setDocument(document);
    view.fitToDrawing();
    view.beginTool(CadView::ToolMode::FilledArea);
    mouseClick(view, QPointF(400.0, 350.0));
    if (view.measurements().size() != 1
        || view.measurements().last().type
            != MeasurementType::FilledArea
        || view.measurements().last().rawArea() <= 0.0) {
        return fail("filled-area measurement failed");
    }
    view.addTextAnnotation(QPointF(430.0, 430.0),
                           QStringLiteral("现场复核"));
    if (view.measurements().last().type != MeasurementType::Text) {
        return fail("text annotation failed");
    }
    view.hideAllMeasurements();
    for (const Measurement &measurement : view.measurements()) {
        if (measurement.visible) {
            return fail("hide-all annotations failed");
        }
    }
    view.showAllMeasurements();
    for (const Measurement &measurement : view.measurements()) {
        if (!measurement.visible) {
            return fail("show-all annotations failed");
        }
    }
    view.beginTool(CadView::ToolMode::Calibrate);
    mouseClick(view, QPointF(100.0, 100.0));
    mouseClick(view, QPointF(500.0, 100.0));
    view.applyCalibration(1234.0, MeasuringUnit::Millimeter);
    if (!document->calibrated
        || !(document->metersPerDrawingUnit > 0.0)) {
        return fail("drawing calibration failed");
    }
    const double calibratedMeters =
        document->metersPerDrawingUnit;
    const QString annotationDrawing =
        temporaryDirectory.filePath(QStringLiteral("annotations.dwg"));
    QString annotationError;
    if (!saveAnnotations(annotationDrawing, *document,
                         view.displaySettings(), view.measurements(),
                         &annotationError)) {
        return fail("annotation save failed");
    }
    CadDocument annotationDocument;
    DisplaySettings loadedSettings;
    QVector<Measurement> loadedMeasurements;
    if (!loadAnnotations(annotationDrawing, annotationDocument,
                         &loadedSettings, &loadedMeasurements,
                         &annotationError)
        || loadedMeasurements.size() != view.measurements().size()
        || !annotationDocument.calibrated
        || !near(annotationDocument.metersPerDrawingUnit,
                 calibratedMeters)
        || loadedSettings.annotationStyle
            != AnnotationStyle::Architectural) {
        return fail("annotation restore failed");
    }
    if (QFileInfo::exists(
            annotationDrawing + QStringLiteral(".cadlens.json"))
        || !QFileInfo::exists(
            annotationSidecarPath(annotationDrawing))) {
        return fail("annotations were not stored in the system cache");
    }
    const QString autosaveDrawing =
        temporaryDirectory.filePath(QStringLiteral("autosave.dwg"));
    if (!saveAnnotationsAutosave(
            autosaveDrawing, *document, view.displaySettings(),
            view.measurements(), &annotationError)) {
        return fail("autosave annotation write failed");
    }
    CadDocument autosaveDocument;
    QVector<Measurement> autosaveMeasurements;
    if (!loadAnnotations(autosaveDrawing, autosaveDocument,
                         nullptr, &autosaveMeasurements,
                         &annotationError)
        || autosaveMeasurements.size() != view.measurements().size()
        || !autosaveDocument.calibrated
        || !near(autosaveDocument.metersPerDrawingUnit,
                 calibratedMeters)) {
        return fail("autosave annotation restore failed");
    }

    document->hasSavedView = true;
    document->savedViewCenter = QPointF(420.0, 360.0);
    document->savedViewHeight = 200.0;
    document->savedViewWidth = 400.0;
    view.resize(900, 700);
    view.setDocument(document);
    view.resetView();
    if (!near(view.viewCenter().x(), 420.0)
        || !near(view.viewCenter().y(), 360.0)) {
        return fail("saved DWG model view was not restored");
    }

    // A long run of sub-pixel source edges must not collapse into a line
    // between the first and last points. This is a regression guard for the
    // false diagonals seen around dense fixture/furniture geometry.
    auto denseDocument = std::make_shared<CadDocument>();
    denseDocument->path = QStringLiteral("dense.dxf");
    denseDocument->fileName = QStringLiteral("dense.dxf");
    denseDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer denseLayer;
    denseLayer.name = QStringLiteral("DENSE");
    denseDocument->layers.append(denseLayer);
    CadEntity densePolyline;
    densePolyline.type = CadEntityType::Polyline;
    densePolyline.layer = denseLayer.name;
    densePolyline.points.append(QPointF(200.0, 400.0));
    for (int i = 1; i <= 1500; ++i) {
        densePolyline.points.append(
            QPointF(200.0 + (i % 2 == 0 ? 0.1 : -0.1),
                    400.0 + i * 0.2));
    }
    for (int i = 1; i <= 3000; ++i) {
        densePolyline.points.append(
            QPointF(200.0 + i * 0.2,
                    700.0 + (i % 2 == 0 ? 0.1 : -0.1)));
    }
    for (int i = 1; i <= 1500; ++i) {
        densePolyline.points.append(
            QPointF(800.0 + (i % 2 == 0 ? 0.1 : -0.1),
                    700.0 - i * 0.2));
    }
    densePolyline.updateBounds();
    denseDocument->entities.append(densePolyline);
    CadView denseView;
    denseView.resize(1000, 800);
    denseView.setGridVisible(false);
    denseView.setDocument(denseDocument);
    const QImage denseImage = denseView.renderWorldRectToImage(
        QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    const QColor bridgePixel = denseImage.pixelColor(QPoint(500, 400));
    if (bridgePixel != QColor(Qt::black)) {
        return fail("dense polyline rendered a false bridge diagonal");
    }

    auto hatchDocument = std::make_shared<CadDocument>();
    hatchDocument->path = QStringLiteral("hatch.dxf");
    hatchDocument->fileName = QStringLiteral("hatch.dxf");
    hatchDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer hatchLayer;
    hatchLayer.name = QStringLiteral("HATCH");
    hatchLayer.color = QColor(QStringLiteral("#ff5533"));
    hatchDocument->layers.append(hatchLayer);
    CadEntity solidHatch;
    solidHatch.type = CadEntityType::Solid;
    solidHatch.layer = hatchLayer.name;
    solidHatch.color = hatchLayer.color;
    solidHatch.closed = true;
    solidHatch.filled = true;
    solidHatch.hatchGroup = 1;
    solidHatch.hatchPattern = std::make_shared<CadHatchPattern>();
    solidHatch.hatchPattern->solid = true;
    solidHatch.points = {
        QPointF(300.0, 250.0), QPointF(700.0, 250.0),
        QPointF(700.0, 550.0), QPointF(300.0, 550.0)
    };
    solidHatch.updateBounds();
    hatchDocument->entities.append(solidHatch);
    CadView hatchView;
    hatchView.resize(1000, 800);
    hatchView.setGridVisible(false);
    hatchView.setDocument(hatchDocument);
    const QImage hatchImage = hatchView.renderWorldRectToImage(
        QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    const QColor hatchCenter = hatchImage.pixelColor(QPoint(500, 400));
    if (hatchCenter.red() < 200 || hatchCenter.green() > 120
        || hatchCenter.blue() > 120) {
        return fail("DWG solid HATCH was not rendered");
    }

    auto dotDocument = std::make_shared<CadDocument>();
    dotDocument->path = QStringLiteral("dot-hatch.dxf");
    dotDocument->fileName = QStringLiteral("dot-hatch.dxf");
    dotDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer dotLayer;
    dotLayer.name = QStringLiteral("DOT_HATCH");
    dotLayer.color = QColor(QStringLiteral("#ff5533"));
    dotDocument->layers.append(dotLayer);
    CadEntity dotHatch;
    dotHatch.type = CadEntityType::Solid;
    dotHatch.layer = dotLayer.name;
    dotHatch.color = dotLayer.color;
    dotHatch.closed = true;
    dotHatch.hatchGroup = 2;
    dotHatch.hatchPattern = std::make_shared<CadHatchPattern>();
    CadHatchPatternLine dots;
    dots.origin = QPointF(300.0, 400.0);
    dots.direction = QPointF(1.0, 0.0);
    dots.offset = QPointF(0.0, 20.0);
    dots.dashes = {0.0, -20.0};
    dotHatch.hatchPattern->lines.append(dots);
    dotHatch.points = {
        QPointF(300.0, 250.0), QPointF(700.0, 250.0),
        QPointF(700.0, 550.0), QPointF(300.0, 550.0)
    };
    dotHatch.updateBounds();
    dotDocument->entities.append(dotHatch);
    CadView dotView;
    dotView.resize(1000, 800);
    dotView.setGridVisible(false);
    dotView.setDocument(dotDocument);
    const QImage dotImage = dotView.renderWorldRectToImage(
        QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    int dotPixels = 0;
    for (int y = 320; y < 480; ++y) {
        for (int x = 350; x < 650; ++x) {
            const QColor color = dotImage.pixelColor(x, y);
            if (color.red() > 90 && color.red() > color.green() * 1.8) {
                ++dotPixels;
            }
        }
    }
    if (dotPixels < 10) {
        return fail("DWG zero-length hatch dashes were not drawn as dots");
    }

    denseDocument->layouts.append(QStringLiteral("密集布局"));
    denseDocument->layoutEntities.insert(
        QStringLiteral("密集布局"), QVector<CadEntity>{});
    denseDocument->layoutBounds.insert(
        QStringLiteral("密集布局"), QRectF(0.0, 0.0, 1000.0, 800.0));
    CadViewportDefinition denseViewport;
    denseViewport.center = QPointF(500.0, 400.0);
    denseViewport.windowSize = QSizeF(1000.0, 800.0);
    denseViewport.modelCenter = QPointF(500.0, 400.0);
    denseViewport.viewHeight = 800.0;
    denseDocument->layoutViewports.insert(
        QStringLiteral("密集布局"),
        QVector<CadViewportDefinition>{denseViewport});
    CadView denseLayoutView;
    denseLayoutView.resize(1000, 800);
    denseLayoutView.setGridVisible(false);
    denseLayoutView.setDocument(denseDocument);
    denseLayoutView.setActiveSpace(QStringLiteral("密集布局"));
    const QImage denseLayoutImage =
        denseLayoutView.renderWorldRectToImage(
            QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    if (denseLayoutImage.pixelColor(QPoint(500, 400))
        != QColor(Qt::black)) {
        return fail("layout viewport rendered a false bridge diagonal");
    }

    auto themeDocument = std::make_shared<CadDocument>();
    themeDocument->path = QStringLiteral("theme.dxf");
    themeDocument->fileName = QStringLiteral("theme.dxf");
    themeDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer themeLayer;
    themeLayer.name = QStringLiteral("THEME");
    themeLayer.color = QColor(QStringLiteral("#f7f7f7"));
    themeDocument->layers.append(themeLayer);
    CadEntity themeLine;
    themeLine.layer = themeLayer.name;
    themeLine.color = themeLayer.color;
    themeLine.lineWeight = 18;
    themeLine.points = {
        QPointF(150.0, 400.0), QPointF(850.0, 400.0)
    };
    themeLine.updateBounds();
    themeDocument->entities.append(themeLine);

    const auto relativeLuminance = [](const QColor &color) {
        return 0.2126 * color.redF() + 0.7152 * color.greenF()
            + 0.0722 * color.blueF();
    };
    const auto lineExtremum = [&relativeLuminance](const QImage &image,
                                                  bool brightest) {
        QColor result = image.pixelColor(20, 20);
        for (int y = 370; y <= 430; ++y) {
            for (int x = 300; x <= 700; ++x) {
                const QColor candidate = image.pixelColor(x, y);
                if ((brightest
                         && relativeLuminance(candidate)
                             > relativeLuminance(result))
                    || (!brightest
                        && relativeLuminance(candidate)
                            < relativeLuminance(result))) {
                    result = candidate;
                }
            }
        }
        return result;
    };
    CadView themeView;
    themeView.resize(1000, 800);
    themeView.setGridVisible(false);
    themeView.setDocument(themeDocument);
    themeView.setDrawingTheme(CadView::DrawingTheme::CadDark);
    const QImage cadDarkImage = themeView.renderWorldRectToImage(
        QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    themeView.setDrawingTheme(CadView::DrawingTheme::CadLight);
    const QImage cadLightImage = themeView.renderWorldRectToImage(
        QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    const QColor darkLine = lineExtremum(cadDarkImage, true);
    const QColor darkBackground = cadDarkImage.pixelColor(20, 20);
    const QColor lightLine = lineExtremum(cadLightImage, false);
    const QColor lightBackground = cadLightImage.pixelColor(20, 20);
    if (darkBackground != QColor(Qt::black)
        || lightBackground == QColor(Qt::black)
        || relativeLuminance(darkLine)
            <= relativeLuminance(darkBackground) + 0.25
        || relativeLuminance(lightLine)
            >= relativeLuminance(lightBackground) - 0.25) {
        std::fprintf(
            stderr,
            "theme colors dark bg=%s line=%s light bg=%s line=%s\n",
            qPrintable(darkBackground.name()),
            qPrintable(darkLine.name()),
            qPrintable(lightBackground.name()),
            qPrintable(lightLine.name()));
        return fail("CAD theme did not adapt drawing colors");
    }

    auto dimensionDocument = std::make_shared<CadDocument>();
    dimensionDocument->path = QStringLiteral("dimension.dxf");
    dimensionDocument->fileName = QStringLiteral("dimension.dxf");
    dimensionDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    dimensionDocument->metersPerDrawingUnit = 0.001;
    CadLayer dimensionLayer;
    dimensionLayer.name = QStringLiteral("DIMENSION");
    dimensionDocument->layers.append(dimensionLayer);
    CadView dimensionView;
    dimensionView.resize(1000, 800);
    dimensionView.setGridVisible(false);
    dimensionView.setDocument(dimensionDocument);
    Measurement dimension;
    dimension.id = QStringLiteral("dimension-test");
    dimension.type = MeasurementType::Aligned;
    dimension.points = {
        QPointF(250.0, 300.0), QPointF(750.0, 300.0)
    };
    dimension.labelPosition = QPointF(500.0, 500.0);
    dimension.color = QColor(QStringLiteral("#37d67a"));
    dimensionView.setMeasurements({dimension});
    const QImage dimensionImage =
        dimensionView.renderWorldRectToImage(
            QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    const auto annotationPixels = [&dimensionImage](int left, int top,
                                                     int right, int bottom) {
        int count = 0;
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                const QColor color = dimensionImage.pixelColor(x, y);
                if (color.green() > 90
                    && color.green() > color.red() * 1.35
                    && color.green() > color.blue() * 1.15) {
                    ++count;
                }
            }
        }
        return count;
    };
    if (annotationPixels(251, 350, 262, 470) < 30
        || annotationPixels(738, 350, 749, 470) < 30
        || annotationPixels(330, 298, 670, 308) < 80
        || annotationPixels(255, 296, 270, 310) < 8
        || annotationPixels(730, 296, 745, 310) < 8) {
        return fail("standard dimension extension/line/arrows are incomplete");
    }
    if (qEnvironmentVariableIsSet("CADLENS_TEST_IMAGE_DIR")) {
        const QString directory =
            qEnvironmentVariable("CADLENS_TEST_IMAGE_DIR");
        if (!cadDarkImage.save(directory + "/cad-theme-dark.png")
            || !cadLightImage.save(directory + "/cad-theme-light.png")
            || !dimensionImage.save(
                directory + "/cad-dimension-standard.png")) {
            return fail("test snapshot output failed");
        }
    }

    const QString pdfDrawingPath =
        temporaryDirectory.filePath(QStringLiteral("measured-plan.pdf"));
    {
        QPdfWriter writer(pdfDrawingPath);
        writer.setResolution(144);
        writer.setTitle(QStringLiteral("CAD Lens PDF Test"));
        QPainter painter(&writer);
        painter.setPen(QPen(Qt::black, 2.0));
        painter.drawRect(QRectF(60.0, 60.0, 420.0, 280.0));
        painter.drawLine(QPointF(60.0, 340.0),
                         QPointF(480.0, 340.0));
        painter.drawText(QPointF(80.0, 110.0),
                         QStringLiteral("PDF 施工图"));
        writer.newPage();
        painter.drawEllipse(QRectF(100.0, 100.0, 260.0, 260.0));
        painter.end();
    }
    auto pdfDocument = std::make_shared<CadDocument>();
    QString pdfError;
    if (!pdfDocument->load(pdfDrawingPath, &pdfError)
        || !pdfDocument->isPdf()
        || pdfDocument->pdfPages.size() != 2
        || pdfDocument->pdfPages.first().image.isNull()) {
        std::fprintf(stderr, "PDF load failed: %s\n",
                     qPrintable(pdfError));
        return fail("PDF drawing was not loaded or rendered");
    }
    CadView pdfView;
    pdfView.resize(900, 700);
    pdfView.setGridVisible(false);
    pdfView.setDocument(pdfDocument);
    pdfView.fitToDrawing();
    const QImage pdfFirstPage = pdfView.renderToImage(QSize(900, 700));
    if (pdfFirstPage.isNull()
        || pdfFirstPage.pixelColor(450, 350) == QColor(Qt::black)) {
        return fail("PDF page did not render into the drawing canvas");
    }
    pdfView.setActiveSpace(pdfDocument->pdfPages[1].name);
    if (pdfView.activeSpace() != pdfDocument->pdfPages[1].name
        || pdfDocument->pdfPages[1].image.isNull()) {
        return fail("PDF page was not rendered lazily on page switch");
    }
    pdfView.beginTool(CadView::ToolMode::Linear);
    mouseClick(pdfView, QPointF(260.0, 280.0));
    mouseClick(pdfView, QPointF(620.0, 280.0));
    mouseClick(pdfView, QPointF(430.0, 350.0));
    if (pdfView.measurements().isEmpty()
        || pdfView.measurements().last().type
            != MeasurementType::Linear) {
        return fail("PDF drawing measurement failed");
    }
    if (qEnvironmentVariableIsSet("CADLENS_TEST_IMAGE_DIR")) {
        const QString testDirectory =
            qEnvironmentVariable("CADLENS_TEST_IMAGE_DIR");
        pdfFirstPage.save(
            testDirectory + QStringLiteral("/cad-pdf-measurement.png"));
        QFile::copy(
            pdfDrawingPath,
            testDirectory + QStringLiteral("/cad-pdf-sample.pdf"));
    }

    auto calibrationDocument =
        std::make_shared<CadDocument>();
    calibrationDocument->path = QStringLiteral("calibration.dxf");
    calibrationDocument->fileName = QStringLiteral("calibration.dxf");
    calibrationDocument->bounds =
        QRectF(0.0, 0.0, 1000.0, 800.0);
    calibrationDocument->metersPerDrawingUnit = 0.001;
    CadLayer calibrationLayer;
    calibrationLayer.name = QStringLiteral("CALIBRATION");
    calibrationDocument->layers.append(calibrationLayer);
    CadEntity calibrationLine;
    calibrationLine.layer = calibrationLayer.name;
    calibrationLine.points = {
        QPointF(100.0, 400.0), QPointF(900.0, 400.0)
    };
    calibrationLine.updateBounds();
    calibrationDocument->entities.append(calibrationLine);
    CadView calibrationView;
    calibrationView.resize(900, 700);
    calibrationView.setDocument(calibrationDocument);
    calibrationView.fitToDrawing();
    QObject::connect(
        &calibrationView, &CadView::calibrationRequested,
        &calibrationView, [&calibrationView](double) {
            calibrationView.applyCalibration(
                1200.0, MeasuringUnit::Millimeter);
        });
    calibrationView.beginTool(CadView::ToolMode::Calibrate);
    mouseClick(calibrationView, QPointF(220.0, 350.0));
    mouseClick(calibrationView, QPointF(680.0, 350.0));
    if (calibrationView.measurements().size() != 1
        || calibrationView.measurements().first().type
            != MeasurementType::Aligned
        || calibrationView.toolMode() != CadView::ToolMode::Select
        || calibrationView.measurements().first()
                .primaryValue(*calibrationDocument,
                              calibrationView.displaySettings())
            != QStringLiteral("1200")) {
        return fail("calibration dimension or hand-tool return failed");
    }
    calibrationView.removeAllMeasurements();
    if (!calibrationView.measurements().isEmpty()
        || !calibrationView.undo()
        || calibrationView.measurements().size() != 1) {
        return fail("delete-all annotations or undo failed");
    }

    std::puts("cadview_interaction_tests passed");
    return 0;
}
