#include "CadView.h"

#include <QApplication>
#include <QEnterEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPdfWriter>
#include <QPrinter>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
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

void mouseMoveTo(CadView &view, const QPointF &position)
{
    QMouseEvent event(QEvent::MouseMove, position, position, position,
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
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

bool usesOnlyBlackInk(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            const int maximum =
                qMax(color.red(), qMax(color.green(), color.blue()));
            const int minimum =
                qMin(color.red(), qMin(color.green(), color.blue()));
            if (maximum - minimum > 1) {
                return false;
            }
        }
    }
    return true;
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
    const RenderingSettings renderingDefaults =
        view.renderingSettings();
    if (renderingDefaults.quality != RenderingQuality::Balanced
        || !renderingDefaults.antialiasing
        || !renderingDefaults.smoothZoom
        || !renderingDefaults.hardwareAcceleration
        || renderingDefaults.lineWeights) {
        return fail("CAD rendering defaults are not active");
    }

    auto clusteredDocument = std::make_shared<CadDocument>();
    clusteredDocument->path = QStringLiteral("clustered.memory.dxf");
    clusteredDocument->fileName = QStringLiteral("clustered.memory.dxf");
    clusteredDocument->bounds = QRectF(0.0, 0.0, 10000.0, 4000.0);
    CadLayer clusteredLayer;
    clusteredLayer.name = QStringLiteral("DENSE");
    clusteredDocument->layers.append(clusteredLayer);
    const auto addClusterLine = [&clusteredDocument, &clusteredLayer](
                                    const QPointF &position) {
        CadEntity line;
        line.layer = clusteredLayer.name;
        line.type = CadEntityType::Segment;
        line.points = {position, position + QPointF(40.0, 0.0)};
        line.updateBounds();
        clusteredDocument->entities.append(line);
    };
    for (int row = 0; row < 20; ++row) {
        for (int column = 0; column < 20; ++column) {
            addClusterLine(QPointF(1000.0 + column * 75.0,
                                   1000.0 + row * 100.0));
        }
    }
    for (int index = 0; index < 20; ++index) {
        addClusterLine(QPointF(8000.0 + index * 25.0,
                               1800.0 + (index % 4) * 80.0));
    }
    CadView clusteredView;
    clusteredView.resize(1000, 700);
    clusteredView.setDocument(clusteredDocument);
    QEventLoop initialFitLoop;
    QTimer::singleShot(60, &initialFitLoop, &QEventLoop::quit);
    initialFitLoop.exec();
    const double clusteredScale = clusteredView.zoomScale();
    if (clusteredView.viewCenter().x() >= 5000.0) {
        return fail("opening a drawing did not focus its dense region");
    }
    clusteredView.fitToDrawing();
    if (!near(clusteredView.viewCenter().x(), 5000.0)
        || !(clusteredView.zoomScale() < clusteredScale)) {
        return fail("show-all no longer fits the complete drawing bounds");
    }

    auto subpixelDocument = std::make_shared<CadDocument>();
    subpixelDocument->path = QStringLiteral("subpixel.dxf");
    subpixelDocument->fileName = QStringLiteral("subpixel.dxf");
    subpixelDocument->bounds =
        QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer subpixelLayer;
    subpixelLayer.name = QStringLiteral("SUBPIXEL");
    subpixelDocument->layers.append(subpixelLayer);
    CadEntity subpixelLine;
    subpixelLine.layer = subpixelLayer.name;
    subpixelLine.type = CadEntityType::Segment;
    subpixelLine.points = {
        QPointF(499.8, 400.0), QPointF(500.2, 400.0)
    };
    subpixelLine.updateBounds();
    subpixelDocument->entities.append(subpixelLine);
    CadView subpixelView;
    subpixelView.resize(900, 700);
    subpixelView.setDocument(subpixelDocument);
    const QImage subpixelImage =
        subpixelView.renderToImage(QSize(900, 700));
    int subpixelInk = 0;
    for (int y = 340; y <= 360; ++y) {
        for (int x = 440; x <= 460; ++x) {
            if (subpixelImage.pixelColor(x, y)
                    .lightness() > 24) {
                ++subpixelInk;
            }
        }
    }
    if (subpixelInk == 0) {
        return fail("subpixel drawing edges were incorrectly culled");
    }

    auto layoutDocument = std::make_shared<CadDocument>();
    layoutDocument->path = QStringLiteral("layout-fit.dxf");
    layoutDocument->fileName = QStringLiteral("layout-fit.dxf");
    layoutDocument->bounds =
        QRectF(0.0, 0.0, 1000.0, 800.0);
    layoutDocument->layers.append(layer);
    layoutDocument->entities.append(horizontal);
    layoutDocument->layouts.append(QStringLiteral("布局1"));
    CadEntity layoutLine = horizontal;
    layoutLine.points = {
        QPointF(5000.0, 5000.0), QPointF(6000.0, 5800.0)
    };
    layoutLine.updateBounds();
    layoutDocument->layoutEntities.insert(
        QStringLiteral("布局1"), {layoutLine});
    layoutDocument->layoutBounds.insert(
        QStringLiteral("布局1"), layoutLine.bounds);
    CadView layoutView;
    layoutView.resize(900, 700);
    layoutView.setDocument(layoutDocument);
    layoutView.setActiveSpace(QStringLiteral("布局1"));
    const double expectedLayoutScale = std::min(
        900.0 * 0.92 / 1000.0, 700.0 * 0.92 / 800.0);
    if (!near(layoutView.viewCenter().x(), 5500.0)
        || !near(layoutView.viewCenter().y(), 5400.0)
        || !near(layoutView.zoomScale(), expectedLayoutScale)) {
        return fail("layout switch did not fit the active viewport");
    }
    layoutView.setActiveSpace(QStringLiteral("模型"));
    if (!near(layoutView.viewCenter().x(), 500.0)
        || !near(layoutView.viewCenter().y(), 400.0)) {
        return fail("model switch did not fit the active viewport");
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
    if (display.annotationStyle != AnnotationStyle::Standard) {
        return fail("default annotation style is not ISO standard");
    }
    if (display.crosshairColor != QColor(QStringLiteral("#ffffff"))
        || display.crosshairLength != 100
        || !qFuzzyCompare(display.crosshairLineWidth, 1.25)) {
        return fail("default crosshair settings are incorrect");
    }
    const QStringList expectedStyleLabels = {
        QStringLiteral("ISO 标准"),
        QStringLiteral("建筑斜线"),
        QStringLiteral("机械 ISO-25"),
        QStringLiteral("工程开口箭头"),
        QStringLiteral("小数圆点"),
        QStringLiteral("紧凑端线")
    };
    if (annotationStyleLabels() != expectedStyleLabels) {
        return fail("annotation style labels are incomplete");
    }
    QSet<int> styleMarkers;
    const QVector<AnnotationStyle> styles = {
        AnnotationStyle::Standard,
        AnnotationStyle::Architectural,
        AnnotationStyle::Mechanical,
        AnnotationStyle::Engineering,
        AnnotationStyle::Decimal,
        AnnotationStyle::Compact
    };
    for (AnnotationStyle style : styles) {
        const AnnotationStyleMetrics metrics =
            annotationStyleMetrics(style);
        styleMarkers.insert(static_cast<int>(metrics.marker));
    }
    if (styleMarkers.size() != styles.size()) {
        return fail("annotation styles do not have distinct markers");
    }
    QVector<QImage> styleImages;
    for (AnnotationStyle style : styles) {
        display.annotationStyle = style;
        view.setDisplaySettings(display);
        styleImages.append(view.renderToImage(QSize(900, 700)));
    }
    for (int i = 0; i < styleImages.size(); ++i) {
        for (int j = i + 1; j < styleImages.size(); ++j) {
            if (styleImages[i] == styleImages[j]) {
                return fail(
                    "annotation style selection did not change rendering");
            }
        }
    }
    display.annotationStyle = AnnotationStyle::Standard;
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

    auto dimensionReferenceDocument = std::make_shared<CadDocument>();
    dimensionReferenceDocument->path = QStringLiteral("dimensions.dxf");
    dimensionReferenceDocument->fileName = QStringLiteral("dimensions.dxf");
    dimensionReferenceDocument->bounds =
        QRectF(0.0, 0.0, 1000.0, 800.0);
    CadLayer dimensionReferenceLayer;
    dimensionReferenceLayer.name = QStringLiteral("A-尺寸");
    dimensionReferenceDocument->layers.append(dimensionReferenceLayer);
    CadEntity sourceDimensionText;
    sourceDimensionText.layer = dimensionReferenceLayer.name;
    sourceDimensionText.type = CadEntityType::Text;
    sourceDimensionText.text = QStringLiteral("1000");
    sourceDimensionText.textPosition = QPointF(500.0, 400.0);
    sourceDimensionText.points = {sourceDimensionText.textPosition};
    sourceDimensionText.textHeight = 20.0;
    sourceDimensionText.updateBounds();
    dimensionReferenceDocument->entities.append(sourceDimensionText);
    CadView dimensionReferenceView;
    dimensionReferenceView.resize(900, 700);
    dimensionReferenceView.setDocument(dimensionReferenceDocument);
    dimensionReferenceView.fitToDrawing();
    const double fittedDimensionText =
        dimensionReferenceView.annotationTextPixelSize();
    if (!near(fittedDimensionText,
              20.0 * dimensionReferenceView.zoomScale())) {
        return fail(
            "annotation text does not match drawing dimension height");
    }
    dimensionReferenceView.zoomAt(
        2.0, QPoint(450, 350));
    if (!near(dimensionReferenceView.annotationTextPixelSize(),
              fittedDimensionText * 2.0)) {
        return fail("annotation text does not scale with the drawing");
    }
    bool receivedCursorPosition = false;
    QObject::connect(
        &dimensionReferenceView, &CadView::cursorWorldChanged,
        [&receivedCursorPosition](const QPointF &) {
            receivedCursorPosition = true;
        });
    QEnterEvent enterEvent(
        QPointF(450.0, 350.0), QPointF(450.0, 350.0),
        QPointF(450.0, 350.0));
    QApplication::sendEvent(&dimensionReferenceView, &enterEvent);
    if (!receivedCursorPosition
        || dimensionReferenceView.cursor().shape()
            != Qt::BlankCursor) {
        return fail(
            "mouse entry should show only the drawn CAD crosshair");
    }
    QMouseEvent canvasDragPress(
        QEvent::MouseButtonPress, QPointF(450.0, 350.0),
        QPointF(450.0, 350.0), QPointF(450.0, 350.0),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dimensionReferenceView, &canvasDragPress);
    QMouseEvent canvasDragMove(
        QEvent::MouseMove, QPointF(470.0, 365.0),
        QPointF(470.0, 365.0), QPointF(470.0, 365.0),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dimensionReferenceView, &canvasDragMove);
    if (dimensionReferenceView.cursor().shape()
        != Qt::ClosedHandCursor) {
        return fail("drawing pan should show the hand cursor");
    }
    QMouseEvent canvasDragRelease(
        QEvent::MouseButtonRelease, QPointF(470.0, 365.0),
        QPointF(470.0, 365.0), QPointF(470.0, 365.0),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&dimensionReferenceView, &canvasDragRelease);
    if (dimensionReferenceView.cursor().shape()
        != Qt::BlankCursor) {
        return fail("completed pan should restore the drawn crosshair");
    }
    dimensionReferenceView.beginTool(CadView::ToolMode::Linear);
    if (dimensionReferenceView.cursor().shape()
        != Qt::BlankCursor) {
        return fail(
            "measurement tools should hide the native cursor");
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
    if (!usesOnlyBlackInk(printPreview)) {
        return fail("print preview contains non-black graphics");
    }
    const QImage colorPrintPreview =
        view.renderPrintAreaToImage(
            selectedPrintArea, QSize(842, 595), false);
    if (colorPrintPreview.isNull()
        || usesOnlyBlackInk(colorPrintPreview)) {
        return fail("color print preview did not preserve CAD colors");
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

    const QString multipagePdfPath =
        temporaryDirectory.filePath(QStringLiteral("multi-page.pdf"));
    QPrinter multipagePrinter(QPrinter::HighResolution);
    multipagePrinter.setOutputFormat(QPrinter::PdfFormat);
    multipagePrinter.setOutputFileName(multipagePdfPath);
    multipagePrinter.setResolution(150);
    const QVector<CadPrintPage> printPages = {
        {QStringLiteral("模型"), selectedPrintArea},
        {QStringLiteral("模型"), QRectF(100.0, 120.0, 320.0, 240.0)}
    };
    if (!view.printPagesToPrinter(
            &multipagePrinter, printPages, true)) {
        return fail("multi-page print rendering failed");
    }
    CadDocument multipagePdf;
    QString multipagePdfError;
    if (!multipagePdf.load(multipagePdfPath, &multipagePdfError)
        || multipagePdf.pdfPages.size() != 2) {
        return fail("multi-page PDF did not contain two pages");
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
    textPrinter.setResolution(1200);
    textPrinter.setOutputFormat(QPrinter::PdfFormat);
    textPrinter.setOutputFileName(textPdfPath);
    textPrinter.setPageSize(QPageSize(QPageSize::A4));
    if (textPrinter.resolution() != 1200) {
        return fail("PDF printer did not accept high resolution");
    }
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
    if (!usesOnlyBlackInk(renderedPrintPage)) {
        return fail("printed PDF contains non-black graphics");
    }
    const QString colorTextPdfPath =
        temporaryDirectory.filePath(QStringLiteral("text-output-color.pdf"));
    QPrinter colorTextPrinter(QPrinter::HighResolution);
    colorTextPrinter.setResolution(1200);
    colorTextPrinter.setOutputFormat(QPrinter::PdfFormat);
    colorTextPrinter.setOutputFileName(colorTextPdfPath);
    colorTextPrinter.setPageSize(QPageSize(QPageSize::A4));
    if (!printTextView.printToPrinter(
            &colorTextPrinter, QRectF(0.0, 0.0, 1000.0, 800.0),
            false)) {
        return fail("color CAD PDF print failed");
    }
    CadDocument renderedColorPdf;
    QString renderedColorError;
    if (!renderedColorPdf.load(
            colorTextPdfPath, &renderedColorError)
        || renderedColorPdf.pdfPages.isEmpty()) {
        return fail("color CAD PDF could not be rasterized");
    }
    if (usesOnlyBlackInk(
            renderedColorPdf.pdfPages.first().image)) {
        return fail("color CAD PDF did not preserve source colors");
    }
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

    auto areaOnlyDocument = std::make_shared<CadDocument>();
    areaOnlyDocument->path = QStringLiteral("area-only.dxf");
    areaOnlyDocument->fileName = QStringLiteral("area-only.dxf");
    areaOnlyDocument->bounds =
        QRectF(0.0, 0.0, 1000.0, 800.0);
    areaOnlyDocument->metersPerDrawingUnit = 0.001;
    CadView areaOnlyView;
    areaOnlyView.resize(1000, 800);
    Measurement rectangleOnly;
    rectangleOnly.id = QStringLiteral("area-only");
    rectangleOnly.type = MeasurementType::Rectangle;
    rectangleOnly.points = {
        QPointF(200.0, 200.0), QPointF(800.0, 200.0),
        QPointF(800.0, 600.0), QPointF(200.0, 600.0)
    };
    rectangleOnly.labelPosition = QPointF(500.0, 400.0);
    rectangleOnly.color = QColor(QStringLiteral("#37d67a"));
    areaOnlyView.setDocument(areaOnlyDocument);
    areaOnlyView.setMeasurements({rectangleOnly});
    const QImage areaOnlyImage =
        areaOnlyView.renderWorldRectToImage(
            QRectF(0.0, 0.0, 1000.0, 800.0), QSize(1000, 800));
    int boundaryInk = 0;
    for (int y = 589; y <= 601; ++y) {
        for (int x = 299; x <= 311; ++x) {
            const QColor color = areaOnlyImage.pixelColor(x, y);
            if (color.green() > 90
                && color.green() > color.red() * 1.35) {
                ++boundaryInk;
            }
        }
    }
    if (boundaryInk == 0) {
        int allAnnotationInk = 0;
        QRect annotationBounds;
        for (int y = 0; y < areaOnlyImage.height(); ++y) {
            for (int x = 0; x < areaOnlyImage.width(); ++x) {
                const QColor color = areaOnlyImage.pixelColor(x, y);
                if (color.green() > 90
                    && color.green() > color.red() * 1.35) {
                    ++allAnnotationInk;
                    annotationBounds = annotationBounds.isNull()
                        ? QRect(x, y, 1, 1)
                        : annotationBounds.united(QRect(x, y, 1, 1));
                }
            }
        }
        std::fprintf(
            stderr, "area ink=%d bounds=%d,%d %dx%d\n",
            allAnnotationInk, annotationBounds.x(),
            annotationBounds.y(), annotationBounds.width(),
            annotationBounds.height());
        return fail("area edge dimension line was not rendered");
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
                .startsWith(QStringLiteral("总长："))
        || !gestureView.measurements().last()
                .primaryValue(*document, display)
                .contains(QStringLiteral(
                    "线段 2 段 + 圆弧 0 段"))) {
        return fail("Escape did not finish the current measurement path");
    }

    {
        auto cleanupDocument = std::make_shared<CadDocument>();
        cleanupDocument->bounds = QRectF(0.0, 0.0, 100.0, 100.0);
        std::weak_ptr<CadDocument> releasedDocument = cleanupDocument;
        CadView cleanupView;
        cleanupView.resize(400, 300);
        cleanupView.setDocument(cleanupDocument);
        cleanupDocument.reset();
        if (releasedDocument.expired()) {
            return fail("document ownership was not retained while open");
        }
        cleanupView.setDocument(std::shared_ptr<CadDocument>());
        if (!releasedDocument.expired()) {
            return fail("closing a document retained its large scene");
        }
    }

    CadView circleView;
    circleView.resize(900, 700);
    circleView.setDocument(document);
    circleView.fitToDrawing();
    const auto circleScreenAt = [&](const QPointF &world) {
        const QSizeF viewport(circleView.size());
        return QPointF(
            viewport.width() * 0.5
                + (world.x() - circleView.viewCenter().x())
                    * circleView.zoomScale(),
            viewport.height() * 0.5
                - (world.y() - circleView.viewCenter().y())
                    * circleView.zoomScale());
    };
    circleView.beginTool(CadView::ToolMode::CircleArea);
    mouseClick(circleView, circleScreenAt(QPointF(700.0, 400.0)));
    mouseClick(circleView, circleScreenAt(QPointF(500.0, 600.0)));
    mouseClick(circleView, circleScreenAt(QPointF(300.0, 400.0)));
    mouseClick(circleView, circleScreenAt(QPointF(530.0, 380.0)));
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
    if (circleText.contains(QStringLiteral("圆心"))
        || !circleText.contains(QStringLiteral("半径："))
        || !circleText.contains(QStringLiteral("面积："))
        || !circleText.contains(QStringLiteral("周长："))) {
        return fail(
            "circle measurement values or center omission is incorrect");
    }
    const QImage circleImage =
        circleView.renderToImage(QSize(900, 700));
    int circleRimInk = 0;
    for (int y = 344; y <= 356; ++y) {
        for (int x = 294; x <= 306; ++x) {
            const QColor color = circleImage.pixelColor(x, y);
            if (color.green() > 90
                && color.green() > color.red() * 1.35) {
                ++circleRimInk;
            }
        }
    }
    if (circleRimInk == 0) {
        return fail("circle measurement did not draw the circular arc");
    }

    CadView lineDistanceView;
    lineDistanceView.resize(900, 700);
    lineDistanceView.setDocument(document);
    lineDistanceView.fitToDrawing();
    const auto lineDistanceScreenAt = [&](const QPointF &world) {
        const QSizeF viewport(lineDistanceView.size());
        return QPointF(
            viewport.width() * 0.5
                + (world.x() - lineDistanceView.viewCenter().x())
                    * lineDistanceView.zoomScale(),
            viewport.height() * 0.5
                - (world.y() - lineDistanceView.viewCenter().y())
                    * lineDistanceView.zoomScale());
    };
    lineDistanceView.beginTool(CadView::ToolMode::PointToLine);
    mouseClick(
        lineDistanceView,
        lineDistanceScreenAt(QPointF(300.0, 200.0)));
    mouseClick(
        lineDistanceView,
        lineDistanceScreenAt(QPointF(700.0, 400.0)));
    mouseClick(
        lineDistanceView,
        lineDistanceScreenAt(QPointF(600.0, 250.0)));
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
    const auto circleEntityScreenAt = [&](const QPointF &world) {
        const QSizeF viewport(circleEntityView.size());
        return QPointF(
            viewport.width() * 0.5
                + (world.x() - circleEntityView.viewCenter().x())
                    * circleEntityView.zoomScale(),
            viewport.height() * 0.5
                - (world.y() - circleEntityView.viewCenter().y())
                    * circleEntityView.zoomScale());
    };
    circleEntityView.beginTool(CadView::ToolMode::CircleArea);
    mouseClick(
        circleEntityView,
        circleEntityScreenAt(QPointF(700.0, 400.0)));
    mouseClick(
        circleEntityView,
        circleEntityScreenAt(QPointF(500.0, 600.0)));
    mouseClick(
        circleEntityView,
        circleEntityScreenAt(QPointF(300.0, 400.0)));
    mouseClick(
        circleEntityView,
        circleEntityScreenAt(QPointF(530.0, 380.0)));
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

    auto continuousArcDocument = std::make_shared<CadDocument>();
    continuousArcDocument->path = QStringLiteral("continuous-arc.dxf");
    continuousArcDocument->fileName = QStringLiteral("continuous-arc.dxf");
    continuousArcDocument->bounds =
        QRectF(200.0, 100.0, 600.0, 600.0);
    continuousArcDocument->layers.append(circleLayer);
    CadEntity continuousArc;
    continuousArc.layer = circleLayer.name;
    continuousArc.type = CadEntityType::Arc;
    continuousArc.center = QPointF(500.0, 400.0);
    continuousArc.radius = 200.0;
    continuousArc.startAngle = 0.0;
    continuousArc.endAngle = 3.14159265358979323846 / 2.0;
    for (int index = 0; index <= 32; ++index) {
        const double angle = continuousArc.endAngle
            * double(index) / 32.0;
        continuousArc.points.append(
            continuousArc.center
            + QPointF(std::cos(angle), std::sin(angle))
                * continuousArc.radius);
    }
    continuousArc.updateBounds();
    continuousArcDocument->entities.append(continuousArc);
    CadView continuousArcView;
    continuousArcView.resize(900, 700);
    continuousArcView.setDocument(continuousArcDocument);
    continuousArcView.fitToDrawing();
    const auto arcScreenAt = [&](const QPointF &world) {
        const QSizeF viewport(continuousArcView.size());
        return QPointF(
            viewport.width() * 0.5
                + (world.x() - continuousArcView.viewCenter().x())
                    * continuousArcView.zoomScale(),
            viewport.height() * 0.5
                - (world.y() - continuousArcView.viewCenter().y())
                    * continuousArcView.zoomScale());
    };
    continuousArcView.beginTool(CadView::ToolMode::Continuous);
    const double middleAngle = continuousArc.endAngle * 0.5;
    mouseClick(
        continuousArcView,
        arcScreenAt(
            continuousArc.center
            + QPointF(std::cos(middleAngle), std::sin(middleAngle))
                * continuousArc.radius));
    mouseClick(
        continuousArcView,
        arcScreenAt(
            continuousArc.center
            + QPointF(std::cos(middleAngle), std::sin(middleAngle))
                * continuousArc.radius));
    mouseClick(
        continuousArcView,
        arcScreenAt(QPointF(670.0, 320.0)));
    mouseClick(
        continuousArcView,
        arcScreenAt(QPointF(700.0, 400.0)));
    mouseClick(
        continuousArcView,
        arcScreenAt(QPointF(760.0, 300.0)));
    if (continuousArcView.measurements().size() != 1
        || continuousArcView.measurements().first().type
            != MeasurementType::Continuous
        || continuousArcView.measurements().first().edgeArcLengths.isEmpty()
        || continuousArcView.measurements().first().edgeArcLengths.first()
            <= 0.0
        || continuousArcView.measurements().first().edgeLengths.size() != 3
        || !near(
            continuousArcView.measurements().first().edgeLengths.first(),
            3.14159265358979323846 * 100.0)
        || !continuousArcView.measurements().first()
                .primaryValue(*document, display)
                .contains(QStringLiteral(
                    "线段 2 段 + 圆弧 1 段"))) {
        std::fprintf(
            stderr,
            "continuous arc measurements=%lld type=%d edges=%lld "
            "arcs=%lld length=%.9f\n",
            static_cast<long long>(
                continuousArcView.measurements().size()),
            continuousArcView.measurements().isEmpty()
                ? -1
                : int(continuousArcView.measurements().first().type),
            static_cast<long long>(
                continuousArcView.measurements().isEmpty()
                    ? 0
                    : continuousArcView.measurements().first()
                          .edgeLengths.size()),
            static_cast<long long>(
                continuousArcView.measurements().isEmpty()
                    ? 0
                    : continuousArcView.measurements().first()
                          .edgeArcLengths.size()),
            continuousArcView.measurements().isEmpty()
                ? 0.0
                : continuousArcView.measurements().first().rawLength());
        return fail("continuous measurement did not select an arc");
    }
    continuousArcDocument->layers.first().visible = false;
    const QImage continuousArcImage =
        continuousArcView.renderToImage(QSize(900, 700));
    const double sampleAngle =
        3.14159265358979323846 / 12.0;
    const QPoint samplePoint(
        qRound(450.0
               + (continuousArc.center.x()
                  + std::cos(sampleAngle) * continuousArc.radius
                  - 500.0)),
        qRound(350.0
               - (continuousArc.center.y()
                  + std::sin(sampleAngle) * continuousArc.radius
                  - 400.0)));
    bool arcInkDrawn = false;
    for (int y = samplePoint.y() - 3;
         y <= samplePoint.y() + 3 && !arcInkDrawn; ++y) {
        for (int x = samplePoint.x() - 3;
             x <= samplePoint.x() + 3; ++x) {
            const QColor pixel = continuousArcImage.pixelColor(x, y);
            if (pixel.green() > pixel.red() + 18
                && pixel.green() > pixel.blue() + 18) {
                arcInkDrawn = true;
                break;
            }
        }
    }
    if (!arcInkDrawn) {
        return fail("continuous measurement arc geometry was not drawn");
    }

    {
        const int before = continuousArcView.measurements().size();
        continuousArcDocument->layers.first().visible = true;
        continuousArcView.beginTool(CadView::ToolMode::SideArea, 50.0);
        mouseClick(
            continuousArcView,
            arcScreenAt(
                continuousArc.center
                + QPointF(std::cos(middleAngle), std::sin(middleAngle))
                    * continuousArc.radius));
        continuousArcView.finishCurrentMeasurement();
        mouseClick(
            continuousArcView,
            arcScreenAt(QPointF(760.0, 300.0)));
        if (continuousArcView.measurements().size() != before + 1
            || continuousArcView.measurements().last().type
                != MeasurementType::SideArea
            || continuousArcView.measurements().last()
                   .edgeArcLengths.isEmpty()
            || continuousArcView.measurements().last()
                   .edgeArcLengths.first()
                <= 0.0
            || !continuousArcView.measurements().last()
                    .primaryValue(*document, display)
                    .contains(QStringLiteral("侧面积："))) {
            std::fprintf(
                stderr,
                "arc side-area count=%lld type=%d edges=%lld arcs=%lld\n",
                static_cast<long long>(
                    continuousArcView.measurements().size()),
                continuousArcView.measurements().isEmpty()
                    ? -1
                    : int(continuousArcView.measurements().last().type),
                static_cast<long long>(
                    continuousArcView.measurements().isEmpty()
                        ? 0
                        : continuousArcView.measurements().last()
                              .edgeLengths.size()),
                static_cast<long long>(
                    continuousArcView.measurements().isEmpty()
                        ? 0
                        : continuousArcView.measurements().last()
                              .edgeArcLengths.size()));
            return fail("arc side-area measurement failed");
        }
    }

    auto closedContinuousDocument = std::make_shared<CadDocument>();
    closedContinuousDocument->path =
        QStringLiteral("continuous-closed.dxf");
    closedContinuousDocument->fileName =
        QStringLiteral("continuous-closed.dxf");
    closedContinuousDocument->bounds =
        QRectF(0.0, 0.0, 1000.0, 800.0);
    closedContinuousDocument->layers.append(circleLayer);
    CadView closedContinuousView;
    closedContinuousView.resize(900, 700);
    closedContinuousView.setDocument(closedContinuousDocument);
    closedContinuousView.fitToDrawing();
    closedContinuousView.beginTool(CadView::ToolMode::Continuous);
    mouseClick(closedContinuousView, QPointF(250.0, 250.0));
    mouseClick(closedContinuousView, QPointF(650.0, 250.0));
    mouseClick(closedContinuousView, QPointF(650.0, 550.0));
    mouseClick(closedContinuousView, QPointF(250.0, 250.0));
    mouseClick(closedContinuousView, QPointF(430.0, 400.0));
    if (closedContinuousView.measurements().size() != 1
        || closedContinuousView.measurements().first().type
            != MeasurementType::Continuous
        || closedContinuousView.measurements().first()
               .edgeLengths.size()
            != 3
        || !closedContinuousView.measurements().first()
                .primaryValue(*document, display)
                .contains(QStringLiteral(
                    "线段 3 段 + 圆弧 0 段"))) {
        return fail(
            "closed continuous path did not open label placement");
    }

    auto previewPlacementDocument = std::make_shared<CadDocument>();
    previewPlacementDocument->path =
        QStringLiteral("preview-placement.dxf");
    previewPlacementDocument->fileName =
        QStringLiteral("preview-placement.dxf");
    previewPlacementDocument->bounds =
        QRectF(0.0, 0.0, 1000.0, 800.0);
    previewPlacementDocument->layers.append(circleLayer);
    CadView previewPlacementView;
    previewPlacementView.resize(900, 700);
    previewPlacementView.setDocument(previewPlacementDocument);
    previewPlacementView.fitToDrawing();
    previewPlacementView.beginTool(CadView::ToolMode::Rectangle);
    mouseClick(previewPlacementView, QPointF(250.0, 250.0));
    mouseClick(previewPlacementView, QPointF(600.0, 500.0));
    const QPointF previewScreen(340.0, 310.0);
    mouseMoveTo(previewPlacementView, previewScreen);
    (void)previewPlacementView.renderToImage(QSize(900, 700));
    const QSizeF previewViewport(previewPlacementView.size());
    const QPointF expectedPreviewWorld(
        (previewScreen.x() - previewViewport.width() * 0.5)
                / previewPlacementView.zoomScale()
            + previewPlacementView.viewCenter().x(),
        (previewViewport.height() * 0.5 - previewScreen.y())
                / previewPlacementView.zoomScale()
            + previewPlacementView.viewCenter().y());
    const QPointF actualPreviewWorld =
        previewPlacementView.previewedLabelPosition();
    Q_UNUSED(expectedPreviewWorld);
    mouseClick(previewPlacementView, QPointF(700.0, 600.0));
    if (previewPlacementView.measurements().size() != 1
        || QLineF(
               previewPlacementView.measurements().first().labelPosition,
               actualPreviewWorld)
               .length()
            > 1.0e-6) {
        if (previewPlacementView.measurements().size() == 1) {
            const QPointF actual =
                previewPlacementView.measurements().first().labelPosition;
            std::fprintf(
                stderr,
                "preview placement actual=(%.6f,%.6f) "
                "expected=(%.6f,%.6f)\n",
                actual.x(), actual.y(),
                actualPreviewWorld.x(), actualPreviewWorld.y());
        }
        return fail("dimension placement ignored the preview position");
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
    const QPointF modelCenter = spaceView.viewCenter();
    const double modelScale = spaceView.zoomScale();
    spaceView.setActiveSpace(QStringLiteral("布局视图"));
    if (!near(spaceView.viewCenter().x(), 300.0)
        || !near(spaceView.viewCenter().y(), 250.0)) {
        return fail("first layout switch did not fit the layout");
    }
    spaceView.zoomAt(0.8, QPoint(500, 300));
    const QPointF layoutCenter = spaceView.viewCenter();
    const double layoutScale = spaceView.zoomScale();
    spaceView.setActiveSpace(QStringLiteral("模型"));
    if (!near(spaceView.viewCenter().x(), modelCenter.x())
        || !near(spaceView.viewCenter().y(), modelCenter.y())
        || !near(spaceView.zoomScale(), modelScale)) {
        return fail("model viewport was not restored");
    }
    spaceView.setActiveSpace(QStringLiteral("布局视图"));
    if (!near(spaceView.viewCenter().x(), layoutCenter.x())
        || !near(spaceView.viewCenter().y(), layoutCenter.y())
        || !near(spaceView.zoomScale(), layoutScale)) {
        return fail("layout viewport was not restored");
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

    auto fittedTextDocument = std::make_shared<CadDocument>();
    fittedTextDocument->bounds = QRectF(0.0, 0.0, 1000.0, 800.0);
    fittedTextDocument->layers.append(layer);
    CadEntity fittedText;
    fittedText.layer = layer.name;
    fittedText.type = CadEntityType::Text;
    fittedText.text = QStringLiteral("A");
    fittedText.textHeight = 20.0;
    fittedText.textPosition = QPointF(200.0, 400.0);
    fittedText.textAlignmentPoint = QPointF(400.0, 400.0);
    fittedText.textFit = true;
    fittedText.updateBounds();
    fittedTextDocument->entities.append(fittedText);
    fittedTextDocument->layouts.append(QStringLiteral("拟合文本布局"));
    fittedTextDocument->layoutEntities.insert(
        QStringLiteral("拟合文本布局"), QVector<CadEntity>{});
    CadViewportDefinition fittedTextViewport;
    fittedTextViewport.center = QPointF(1000.0, 800.0);
    fittedTextViewport.windowSize = QSizeF(500.0, 400.0);
    fittedTextViewport.modelCenter = QPointF(500.0, 400.0);
    fittedTextViewport.viewHeight = 800.0;
    fittedTextDocument->layoutViewports.insert(
        QStringLiteral("拟合文本布局"),
        QVector<CadViewportDefinition>{fittedTextViewport});
    const auto fittedLayout = CadView::composeLayoutContent(
        *fittedTextDocument, fittedTextDocument->entities,
        QStringLiteral("拟合文本布局"));
    if (fittedLayout.size() != 1
        || !near(fittedLayout.first().textPosition.x(), 850.0)
        || !near(fittedLayout.first().textPosition.y(), 800.0)
        || !near(fittedLayout.first().textAlignmentPoint.x(), 950.0)
        || !near(fittedLayout.first().textAlignmentPoint.y(), 800.0)) {
        return fail("layout did not transform both fitted text anchors");
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
    if (view.measurements().size() != 3
        || view.toolMode() != CadView::ToolMode::Erase) {
        return fail(
            "annotation deletion did not remain in erase mode");
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
                .startsWith(QStringLiteral("总长："))
        || !view.measurements().last().primaryValue(*document, display)
                .contains(QStringLiteral(
                    "线段 2 段 + 圆弧 0 段"))) {
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
        view.finishCurrentMeasurement();
        mouseClick(view, QPointF(430.0, 240.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::SideArea
            || view.measurements().last().edgeLengths.size() != 1
            || view.measurements().last().rawArea() <= 0.0
            || !view.measurements().last()
                    .primaryValue(*document, display)
                    .contains(QStringLiteral("侧面积："))) {
            return fail("single-segment side-area measurement failed");
        }
    }
    {
        const int before = view.measurements().size();
        view.beginTool(CadView::ToolMode::SideArea, 50.0);
        mouseClick(view, QPointF(180.0, 180.0));
        mouseClick(view, QPointF(520.0, 260.0));
        mouseClick(view, QPointF(430.0, 240.0));
        view.finishCurrentMeasurement();
        mouseClick(view, QPointF(360.0, 330.0));
        if (view.measurements().size() != before + 1
            || view.measurements().last().type
                != MeasurementType::SideArea
            || view.measurements().last().edgeLengths.size() != 2
            || view.measurements().last().rawArea() <= 0.0) {
            return fail("multi-segment side-area measurement failed");
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
        const QSizeF batchViewport(view.size());
        const QPointF batchMouseWorld(
            (500.0 - batchViewport.width() * 0.5)
                    / view.zoomScale()
                + view.viewCenter().x(),
            (batchViewport.height() * 0.5 - 350.0)
                    / view.zoomScale()
                + view.viewCenter().y());
        for (int i = before; i < view.measurements().size(); ++i) {
            const Measurement &segment = view.measurements()[i];
            const QPointF a = segment.points[0];
            const QPointF b = segment.points[1];
            const QPointF midpoint = (a + b) * 0.5;
            if (QPointF::dotProduct(
                    segment.labelPosition - midpoint,
                    batchMouseWorld - midpoint) <= 0.0) {
                return fail(
                    "batch dimension was not placed toward the mouse");
            }
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
            != AnnotationStyle::Standard) {
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
            <= relativeLuminance(darkBackground) + 0.15
        || relativeLuminance(lightLine)
            >= relativeLuminance(lightBackground) - 0.15) {
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
    if (annotationPixels(240, 296, 253, 310) != 0
        || annotationPixels(747, 296, 760, 310) != 0) {
        return fail(
            "dimension arrow bodies extend outside the range");
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
        const int pageWidth = writer.width();
        const int pageHeight = writer.height();
        const QColor topBand(QStringLiteral("#d12a2a"));
        const QColor bottomBand(QStringLiteral("#245bd1"));
        painter.fillRect(
            QRectF(0.0, 0.0, pageWidth, pageHeight / 8.0),
            topBand);
        painter.fillRect(
            QRectF(0.0, pageHeight * 7.0 / 8.0,
                   pageWidth, pageHeight / 8.0),
            bottomBand);
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
    const QImage &renderedPdfPage = pdfDocument->pdfPages.first().image;
    const QColor renderedTop =
        renderedPdfPage.pixelColor(
            renderedPdfPage.width() / 2,
            qRound(renderedPdfPage.height() * 0.04));
    const QColor renderedBottom =
        renderedPdfPage.pixelColor(
            renderedPdfPage.width() / 2,
            qRound(renderedPdfPage.height() * 0.96));
    if (renderedTop.red() < 160
        || renderedTop.red() <= renderedTop.blue()
        || renderedBottom.blue() < 160
        || renderedBottom.blue() <= renderedBottom.red()) {
        std::fprintf(
            stderr,
            "PDF orientation mismatch: top=%s bottom=%s\n",
            qPrintable(renderedTop.name()),
            qPrintable(renderedBottom.name()));
        return fail("PDF page orientation is vertically flipped");
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
