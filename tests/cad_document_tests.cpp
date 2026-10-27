#include "CadModel.h"
#include "LibreDwgReader.h"

#include <QCoreApplication>

#include <cstdio>
#include <limits>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    CadDocument document;
    QString error;
    const QString demo =
        QStringLiteral(CADLENS_SOURCE_DIR)
        + QStringLiteral("/resources/demo/site-plan.dwg");
    if (!document.load(demo, &error)) {
        std::fprintf(stderr, "demo load failed: %s\n",
                     error.toUtf8().constData());
        return 1;
    }
    if (document.entities.isEmpty() || document.layers.isEmpty()
        || document.sourceUnit != MeasuringUnit::Millimeter
        || document.sourceBackend != QStringLiteral("LibreDWG")
        || !document.bounds.isValid()) {
        std::fprintf(stderr,
                     "unexpected demo model: %lld entities, %lld layers\n",
                     static_cast<long long>(document.entities.size()),
                     static_cast<long long>(document.layers.size()));
        return 2;
    }
    CadDocument cachedDocument;
    if (!cachedDocument.load(demo, &error)
        || cachedDocument.entities.size() != document.entities.size()
        || cachedDocument.layers.size() != document.layers.size()
        || cachedDocument.layouts != document.layouts
        || cachedDocument.sourceBackend
            != document.sourceBackend) {
        std::fprintf(stderr,
                     "parsed DWG scene cache did not round-trip\n");
        return 3;
    }
    const QString formatted =
        cleanDwgTextForDisplay(
            QStringLiteral("{\\A1;\\fSimSun|b0;尺寸}\\P"
                           "%%c100 {\\H0.8x;mm}"));
    if (formatted != QStringLiteral("尺寸\n⌀100 mm")) {
        std::fprintf(stderr, "CAD text cleanup failed: %s\n",
                     formatted.toUtf8().constData());
        return 4;
    }
    CadEntity closedPolyline;
    closedPolyline.type = CadEntityType::Polyline;
    closedPolyline.closed = true;
    closedPolyline.points = {
        QPointF(0.0, 0.0), QPointF(10.0, 0.0),
        QPointF(10.0, 10.0), QPointF(0.0, 10.0)
    };
    if (closedPolyline.segments().size() != 4) {
        std::fprintf(stderr, "closed polyline omitted its closing edge\n");
        return 5;
    }
    CadDocument invalidDocument;
    CadLayer invalidLayer;
    invalidLayer.name = QStringLiteral("SANITIZE");
    invalidDocument.layers.append(invalidLayer);
    CadEntity validLine;
    validLine.layer = invalidLayer.name;
    validLine.type = CadEntityType::Segment;
    validLine.points = {QPointF(0.0, 0.0), QPointF(10.0, 0.0)};
    validLine.updateBounds();
    invalidDocument.entities.append(validLine);
    CadEntity zeroLine = validLine;
    zeroLine.points = {QPointF(5.0, 5.0), QPointF(5.0, 5.0)};
    zeroLine.updateBounds();
    invalidDocument.entities.append(zeroLine);
    CadEntity nonFiniteLine = validLine;
    nonFiniteLine.points = {
        QPointF(0.0, 0.0),
        QPointF(std::numeric_limits<double>::quiet_NaN(), 10.0)
    };
    nonFiniteLine.updateBounds();
    invalidDocument.entities.append(nonFiniteLine);
    CadEntity invalidCircle = validLine;
    invalidCircle.type = CadEntityType::Circle;
    invalidCircle.radius = 0.0;
    invalidDocument.entities.append(invalidCircle);
    CadEntity emptyText = validLine;
    emptyText.type = CadEntityType::Text;
    emptyText.text = QStringLiteral("   ");
    emptyText.textPosition = QPointF(1.0, 1.0);
    emptyText.updateBounds();
    invalidDocument.entities.append(emptyText);
    invalidDocument.layoutEntities.insert(
        QStringLiteral("布局"),
        {validLine, nonFiniteLine});
    invalidDocument.sanitizeRenderableEntities();
    if (invalidDocument.entities.size() != 1
        || invalidDocument.layoutEntities.value(QStringLiteral("布局")).size()
            != 1
        || invalidDocument.layers.first().entityCount != 1) {
        std::fprintf(stderr,
                     "invalid CAD entities were not removed from memory\n");
        return 6;
    }
    QVector<QVector<QPointF>> hatchChains;
    appendConnectedPolylinePiece(
        hatchChains,
        {QPointF(0.0, 0.0), QPointF(10.0, 0.0)});
    appendConnectedPolylinePiece(
        hatchChains,
        {QPointF(10.0, 10.0), QPointF(10.0, 0.0)});
    appendConnectedPolylinePiece(
        hatchChains,
        {QPointF(100.0, 100.0), QPointF(110.0, 100.0)});
    if (hatchChains.size() != 2 || hatchChains[0].size() != 3
        || hatchChains[1].size() != 2) {
        std::fprintf(stderr,
                     "disconnected HATCH edges were joined into a false path\n");
        return 7;
    }
    const QColor darkLine = displayColorForDarkCanvas(
        QColor(QStringLiteral("#101010")));
    const QColor darkBlue = displayColorForDarkCanvas(
        QColor(QStringLiteral("#000050")));
    const QColor normalOrange = displayColorForDarkCanvas(
        QColor(QStringLiteral("#ff9f1c")));
    if (darkLine.lightnessF() < 0.45
        || darkBlue.lightnessF() < 0.45
        || normalOrange != QColor(QStringLiteral("#ff9f1c"))) {
        std::fprintf(stderr, "dark-canvas color contrast failed\n");
        return 8;
    }
    std::puts("cad_document_tests passed");
    return 0;
}
