#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileOpenEvent>
#include <QFontDatabase>
#include <QIcon>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <utility>

class CadLensApplication : public QApplication {
    Q_OBJECT

public:
    using QApplication::QApplication;

signals:
    void fileOpenRequested(const QString &path);

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            auto *fileEvent = static_cast<QFileOpenEvent *>(event);
            const QString path = fileEvent->file();
            if (!path.isEmpty()) {
                emit fileOpenRequested(path);
            }
            return true;
        }
        return QApplication::event(event);
    }
};

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSamples(8);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);

    CadLensApplication application(argc, argv);
    const int technicalFontId =
        QFontDatabase::addApplicationFont(
            QStringLiteral(":/fonts/osifont-lgpl3fe.ttf"));
    const int cjkFontId =
        QFontDatabase::addApplicationFont(
            QStringLiteral(":/fonts/NotoSansCJKsc-Regular.otf"));
    QStringList uiFamilies;
    const QStringList technicalFamilies =
        QFontDatabase::applicationFontFamilies(technicalFontId);
    const QStringList cjkFamilies =
        QFontDatabase::applicationFontFamilies(cjkFontId);
    uiFamilies.append(cjkFamilies);
    uiFamilies.append(technicalFamilies);
    QFont uiFont;
    uiFont.setFamilies(uiFamilies);
    uiFont.setPointSizeF(11.5);
    application.setFont(uiFont);
    application.setApplicationName(QStringLiteral("CAD Lens"));
    application.setApplicationDisplayName(QStringLiteral("速看CAD 工程施工看图"));
    application.setOrganizationName(QStringLiteral("CAD Lens"));
    application.setApplicationVersion(QStringLiteral(CADLENS_VERSION));
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/logo.svg")));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("DWG/DXF/PDF 施工测量与矢量标注"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption inspectOption(
        QStringLiteral("inspect"),
        QStringLiteral("读取图纸并输出模型、图层和布局统计"));
    parser.addOption(inspectOption);
    QCommandLineOption inspectEntitiesOption(
        QStringLiteral("inspect-entities"),
        QStringLiteral("输出模型空间每个图元的句柄、图层和有效范围"));
    parser.addOption(inspectEntitiesOption);
    QCommandLineOption snapshotOption(
        QStringLiteral("snapshot"),
        QStringLiteral("加载图纸后保存模型视图截图"),
        QStringLiteral("png"));
    parser.addOption(snapshotOption);
    QCommandLineOption snapshotSpaceOption(
        QStringLiteral("snapshot-space"),
        QStringLiteral("指定截图空间：模型或布局名称"),
        QStringLiteral("space"));
    parser.addOption(snapshotSpaceOption);
    QCommandLineOption snapshotSizeOption(
        QStringLiteral("snapshot-size"),
        QStringLiteral("截图尺寸，例如 1600x900 或 12000x800"),
        QStringLiteral("WxH"));
    parser.addOption(snapshotSizeOption);
    QCommandLineOption snapshotRectOption(
        QStringLiteral("snapshot-rect"),
        QStringLiteral("仅截取世界坐标区域：x,y,width,height"),
        QStringLiteral("rect"));
    parser.addOption(snapshotRectOption);
    QCommandLineOption snapshotLayerOption(
        QStringLiteral("snapshot-layer"),
        QStringLiteral("截图时只显示指定图层"),
        QStringLiteral("layer"));
    parser.addOption(snapshotLayerOption);
    parser.addPositionalArgument(QStringLiteral("drawing"),
                                 QStringLiteral("要打开的 DWG/DXF/PDF 文件"));
    parser.process(application);

    if (parser.isSet(inspectOption)
        || parser.isSet(inspectEntitiesOption)) {
        const QStringList files = parser.positionalArguments();
        if (files.isEmpty()) {
            QTextStream(stderr) << "--inspect requires a drawing path\n";
            return 2;
        }
        CadDocument document;
        QString error;
        if (!document.load(files.first(), &error)) {
            QTextStream(stderr) << error << '\n';
            return 1;
        }
        int hiddenLayers = 0;
        for (const CadLayer &layer : document.layers) {
            hiddenLayers += layer.visible ? 0 : 1;
        }
        QTextStream output(stdout);
        output.setRealNumberPrecision(15);
        if (parser.isSet(inspectEntitiesOption)) {
            const bool dumpPoints =
                qEnvironmentVariableIsSet("CADLENS_DUMP_POINTS");
            const auto writeEntity = [&](const QString &prefix,
                                         const QString &space,
                                         const CadEntity &entity) {
                output << prefix << '=' << entity.sourceHandle << '|';
                if (prefix == QStringLiteral("spaceEntity")) {
                    output << space << '|';
                }
                output
                       << static_cast<int>(entity.type) << '|'
                       << entity.layer << '|' << entity.bounds.left()
                       << '|' << entity.bounds.top() << '|'
                       << entity.bounds.right() << '|'
                       << entity.bounds.bottom() << '|'
                       << entity.points.size() << '|'
                       << entity.closed << '|' << entity.filled << '|'
                       << entity.text << '|' << entity.textHeight << '|'
                       << entity.textWidthScale << '|'
                       << entity.textRotation << '|' << entity.textFit
                       << '|' << entity.textHAlign << '|'
                       << entity.textVAlign << '|'
                       << entity.textAttachment << '|'
                       << entity.textPosition.x() << '|'
                       << entity.textPosition.y() << '|'
                       << entity.textAlignmentPoint.x() << '|'
                       << entity.textAlignmentPoint.y();
                if (dumpPoints) {
                    output << '|';
                    for (qsizetype i = 0; i < entity.points.size(); ++i) {
                        if (i != 0) {
                            output << ';';
                        }
                        output << entity.points[i].x() << ','
                               << entity.points[i].y();
                    }
                }
                output << '\n';
            };
            for (const CadEntity &entity : document.entities) {
                writeEntity(QStringLiteral("entity"), QStringLiteral("模型"),
                            entity);
            }
            for (auto it = document.layoutEntities.cbegin();
                 it != document.layoutEntities.cend(); ++it) {
                for (const CadEntity &entity : it.value()) {
                    writeEntity(QStringLiteral("spaceEntity"), it.key(),
                                entity);
                }
            }
        }
        output << "format=" << document.sourceFormat << ' '
               << document.formatVersion << " backend="
               << document.sourceBackend << '\n'
               << "entities=" << document.entities.size() << '\n'
               << "layers=" << document.layers.size()
               << " hidden=" << hiddenLayers << '\n'
               << "attributeAudit=mode=" << document.attributeMode
               << " source=" << document.sourceAttributeCount
               << " visible=" << document.sourceVisibleAttributeCount
               << " hidden=" << document.sourceHiddenAttributeCount
               << " rendered=" << document.renderedAttributeVisits
               << " suppressed=" << document.suppressedAttributeVisits
               << " overriddenHidden="
               << document.overriddenHiddenAttributeVisits << '\n'
               << "bounds=" << document.bounds.left() << ','
               << document.bounds.top() << ','
               << document.bounds.right() << ','
               << document.bounds.bottom() << '\n';
        if (document.declaredBounds.isValid()) {
            output << "declaredBounds=" << document.declaredBounds.left()
                   << ',' << document.declaredBounds.top() << ','
                   << document.declaredBounds.right() << ','
                   << document.declaredBounds.bottom() << '\n';
        }
        if (document.hasSavedView) {
            output << "savedView=" << document.savedViewCenter.x()
                   << ',' << document.savedViewCenter.y() << ','
                   << document.savedViewWidth << ','
                   << document.savedViewHeight << ','
                   << document.savedViewTwist << '\n';
        }
        if (document.computedBounds.isValid()) {
            output << "computedBounds=" << document.computedBounds.left()
                   << ',' << document.computedBounds.top() << ','
                   << document.computedBounds.right() << ','
                   << document.computedBounds.bottom() << '\n';
        }
        QVector<double> centersX;
        QVector<double> centersY;
        centersX.reserve(document.entities.size());
        centersY.reserve(document.entities.size());
        for (const CadEntity &entity : document.entities) {
            if (entity.bounds.isValid()) {
                centersX.append(entity.bounds.center().x());
                centersY.append(entity.bounds.center().y());
            }
        }
        if (!centersX.isEmpty()) {
            std::sort(centersX.begin(), centersX.end());
            std::sort(centersY.begin(), centersY.end());
            const int low = centersX.size() / 20;
            const int mid = centersX.size() / 2;
            const int high = centersX.size() * 19 / 20;
            output << "entityCenterP05P50P95=x["
                   << centersX[low] << ',' << centersX[mid] << ','
                   << centersX[high] << "] y[" << centersY[low] << ','
                   << centersY[mid] << ',' << centersY[high] << "]\n";
        }
        int malformedText = 0;
        for (const CadEntity &entity : document.entities) {
            if (entity.type == CadEntityType::Text
                && entity.text.contains(QLatin1Char('\\'))) {
                ++malformedText;
            }
        }
        output << "malformedText=" << malformedText << '\n';
        for (const QString &layout : document.layouts) {
            output << "layout=" << layout << ' '
                   << document.layoutEntities.value(layout).size()
                   << '\n';
            const QRectF bounds = document.layoutBounds.value(layout);
            output << "layoutBounds=" << layout << '|'
                   << bounds.left() << '|' << bounds.top() << '|'
                   << bounds.right() << '|' << bounds.bottom() << '\n';
            const auto viewports =
                document.layoutViewports.value(layout);
            for (qsizetype i = 0; i < viewports.size(); ++i) {
                const CadViewportDefinition &viewport = viewports[i];
                output << "layoutViewport=" << layout << '|' << i << '|'
                       << viewport.revealsModel << '|'
                       << viewport.center.x() << '|'
                       << viewport.center.y() << '|'
                       << viewport.windowSize.width() << '|'
                       << viewport.windowSize.height() << '|'
                       << viewport.modelCenter.x() << '|'
                       << viewport.modelCenter.y() << '|'
                       << viewport.viewHeight << '|' << viewport.twist
                       << '|' << viewport.frozenLayers.size() << '\n';
            }
        }
        for (const CadExternalReference &reference :
             document.externalReferences) {
            output << "externalReference=" << reference.name << '|'
                   << reference.savedPath << '|'
                   << reference.resolvedPath << '|'
                   << reference.fileFound << '|'
                   << reference.loaded << '\n';
        }
        for (const CadLayer &layer : document.layers) {
            QRectF layerBounds;
            int count = 0;
            for (const CadEntity &entity : document.entities) {
                if (entity.layer != layer.name) {
                    continue;
                }
                ++count;
                if (entity.bounds.isValid()) {
                    layerBounds = layerBounds.isNull()
                        ? entity.bounds
                        : layerBounds.united(entity.bounds);
                }
            }
            output << "layerBounds=" << layer.name << '|'
                   << count << '|' << layerBounds.left() << '|'
                   << layerBounds.top() << '|' << layerBounds.right()
                   << '|' << layerBounds.bottom() << '\n'
                   << "layerColor=" << layer.name << '|'
                   << layer.color.name(QColor::HexArgb) << '|'
                   << layer.visible << '|' << layer.lineWeight << '\n';
        }
        return 0;
    }

    MainWindow window;
    bool windowReady = false;
    QStringList pendingFiles;
    const auto openFile = [&window, &windowReady,
                           &pendingFiles](const QString &path) {
        if (path.isEmpty()) {
            return;
        }
        if (!windowReady) {
            if (!pendingFiles.contains(path)) {
                pendingFiles.append(path);
            }
            return;
        }
        QTimer::singleShot(0, &window, [&window, path] {
            window.openDrawing(path);
        });
    };
    QObject::connect(&application, &CadLensApplication::fileOpenRequested,
                     &window, openFile);
    windowReady = true;
    window.show();
    for (const QString &path : std::as_const(pendingFiles)) {
        window.openDrawing(path);
    }
    pendingFiles.clear();
    if (!parser.positionalArguments().isEmpty()) {
        window.openDrawing(parser.positionalArguments().first());
    } else if (parser.isSet(snapshotOption)) {
        const QString demoPath =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation)
            + QStringLiteral("/cadlens-site-plan.dwg");
        QFile::remove(demoPath);
        if (QFile::copy(QStringLiteral(":/demo/site-plan.dwg"), demoPath)) {
            window.openDrawing(demoPath);
        }
    }
    if (parser.isSet(snapshotOption)) {
        const QString snapshotPath = parser.value(snapshotOption);
        const QString snapshotSpace = parser.value(snapshotSpaceOption);
        const QString snapshotLayer = parser.value(snapshotLayerOption);
        QSize snapshotSize(1600, 900);
        const QStringList dimensions =
            parser.value(snapshotSizeOption).split(
                QLatin1Char('x'), Qt::SkipEmptyParts);
        if (dimensions.size() == 2) {
            const int snapshotWidth = dimensions[0].toInt();
            const int snapshotHeight = dimensions[1].toInt();
            if (snapshotWidth >= 64 && snapshotHeight >= 64) {
                snapshotSize = QSize(snapshotWidth, snapshotHeight);
            }
        }
        QRectF snapshotRect;
        const QStringList rectangle =
            parser.value(snapshotRectOption).split(
                QLatin1Char(','), Qt::SkipEmptyParts);
        if (rectangle.size() == 4) {
            snapshotRect = QRectF(
                rectangle[0].toDouble(), rectangle[1].toDouble(),
                rectangle[2].toDouble(), rectangle[3].toDouble());
        }
        QElapsedTimer snapshotWait;
        snapshotWait.start();
        auto *snapshotTimer = new QTimer(&application);
        snapshotTimer->setInterval(250);
        QObject::connect(snapshotTimer, &QTimer::timeout, &application, [&] {
            CadView *view = window.findChild<CadView *>();
            if (!view || !view->document()) {
                if (snapshotWait.elapsed() < 60000) {
                    return;
                } else {
                    snapshotTimer->stop();
                    QTextStream(stderr)
                        << "snapshot-failed=" << snapshotPath
                        << " (drawing load timeout)\n";
                    application.exit(1);
                }
                return;
            }
            snapshotTimer->stop();
            if (view && !snapshotSpace.isEmpty()) {
                view->setActiveSpace(snapshotSpace);
            }
            if (view && !snapshotLayer.isEmpty()) {
                for (CadLayer &layer : view->document()->layers) {
                    layer.visible = layer.name == snapshotLayer;
                }
            }
            const QImage image =
                !view ? QImage()
                      : snapshotRect.isValid()
                            ? view->renderWorldRectToImage(snapshotRect,
                                                           snapshotSize)
                            : view->renderToImage(snapshotSize);
            const bool saved = !image.isNull() && image.save(snapshotPath);
            QTextStream(saved ? stdout : stderr)
                << (saved ? "snapshot=" : "snapshot-failed=")
                << snapshotPath << '\n';
            application.exit(saved ? 0 : 1);
        });
        snapshotTimer->start();
    }
    return application.exec();
}

#include "main.moc"
