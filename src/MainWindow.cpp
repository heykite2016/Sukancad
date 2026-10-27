#include "MainWindow.h"
#include "StartScreen.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDataStream>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPageSize>
#include <QPainter>
#include <QPixmap>
#include <QPrinter>
#include <QPrinterInfo>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStackedWidget>
#include <QStyle>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTextBrowser>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <algorithm>
#include <functional>
#include <utility>

namespace {

QString cacheSettingsPath()
{
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (directory.isEmpty()) {
        directory = QDir::tempPath() + QStringLiteral("/CadLens");
    }
    QDir().mkpath(directory);
    return directory + QStringLiteral("/settings.ini");
}

QSettings cacheSettings()
{
    return QSettings(cacheSettingsPath(), QSettings::IniFormat);
}

QString drawingIdentityHash(const QString &path)
{
    const QFileInfo info(path);
    const QString stablePath =
        info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    const QByteArray fingerprint =
        QFile::encodeName(stablePath)
        + '|' + QByteArray::number(info.size())
        + '|' + QByteArray::number(
            info.lastModified().toUTC().toSecsSinceEpoch());
    return QString::fromLatin1(
        QCryptographicHash::hash(fingerprint,
                                 QCryptographicHash::Sha256)
            .toHex());
}

QImage renderDrawingThumbnail(const CadDocument &document,
                              const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#080b0e")));
    if (!document.bounds.isValid() || size.isEmpty()) {
        return image;
    }
    const double availableWidth = qMax(1.0, size.width() - 24.0);
    const double availableHeight = qMax(1.0, size.height() - 24.0);
    if (document.isPdf() && !document.pdfPages.isEmpty()
        && !document.pdfPages.first().image.isNull()) {
        const QImage &pageImage = document.pdfPages.first().image;
        const QSizeF scaled = QSizeF(pageImage.size()).scaled(
            QSizeF(availableWidth, availableHeight),
            Qt::KeepAspectRatio);
        const QRectF target(
            (size.width() - scaled.width()) * 0.5,
            (size.height() - scaled.height()) * 0.5,
            scaled.width(), scaled.height());
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.fillRect(target, Qt::white);
        painter.drawImage(target, pageImage);
        return image;
    }
    const double scale = qMin(
        availableWidth / qMax(1.0e-12, document.bounds.width()),
        availableHeight / qMax(1.0e-12, document.bounds.height()));
    const QPointF center(size.width() * 0.5, size.height() * 0.5);
    const auto mapPoint = [&](const QPointF &world) {
        const QPointF delta = world - document.bounds.center();
        return center + QPointF(delta.x() * scale, -delta.y() * scale);
    };
    QHash<QRgb, QVector<QLineF>> batches;
    for (const CadEntity &entity : document.entities) {
        const CadLayer *layer = document.layer(entity.layer);
        if ((layer && !layer->visible)
            || entity.type == CadEntityType::Text
            || entity.points.size() < 2) {
            continue;
        }
        QVector<QLineF> &lines = batches[entity.color.rgba()];
        QPointF previous = mapPoint(entity.points.first());
        for (qsizetype i = 1; i < entity.points.size(); ++i) {
            const QPointF current = mapPoint(entity.points[i]);
            if (QLineF(previous, current).length() >= 0.25) {
                lines.append(QLineF(previous, current));
            }
            previous = current;
        }
        if (entity.closed) {
            const QPointF first = mapPoint(entity.points.first());
            if (QLineF(previous, first).length() >= 0.25) {
                lines.append(QLineF(previous, first));
            }
        }
    }
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (auto it = batches.cbegin(); it != batches.cend(); ++it) {
        QPen pen(QColor::fromRgba(it.key()), 0.65);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawLines(it.value());
    }
    return image;
}

QFrame *panelFrame()
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("sidePanel"));
    return frame;
}

QToolButton *smallToolButton(const QString &text)
{
    auto *button = new QToolButton;
    button->setText(text);
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QIcon themedCadIcon(const QString &name, bool dark)
{
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QString svg = QString::fromUtf8(file.readAll());
    struct ColorPair {
        const char *source;
        const char *dark;
        const char *light;
    };
    static const ColorPair colors[] = {
        {"#aebdcc", "#7fe7ff", "#075d85"},
        {"#58dbc8", "#31ffbd", "#007f62"},
        {"#5da7d3", "#56c8ff", "#005f8c"},
        {"#69dfa0", "#5cff9d", "#04753b"},
        {"#cbd6dd", "#eafcff", "#0b526a"},
        {"#dce7ee", "#ffffff", "#173947"},
        {"#ffb84d", "#ffb000", "#a64600"}
    };
    for (const ColorPair &color : colors) {
        svg.replace(QString::fromLatin1(color.source),
                    QString::fromLatin1(dark ? color.dark : color.light),
                    Qt::CaseInsensitive);
    }
    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) {
        return QIcon(QStringLiteral(":/icons/%1.svg").arg(name));
    }
    QPixmap pixmap(44, 44);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        renderer.render(&painter);
    }
    pixmap.setDevicePixelRatio(2.0);
    return QIcon(pixmap);
}

class PrintPreviewCanvas : public QWidget {
public:
    explicit PrintPreviewCanvas(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(520, 480);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setPage(const QImage &image, const QSizeF &paperMillimeters)
    {
        m_image = image;
        m_paperMillimeters = paperMillimeters;
        update();
    }

    void setRulerVisible(bool visible)
    {
        m_rulerVisible = visible;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(QStringLiteral("#dce3e7")));
        if (m_image.isNull() || m_paperMillimeters.width() <= 0.0
            || m_paperMillimeters.height() <= 0.0) {
            return;
        }
        const qreal ruler = m_rulerVisible ? 42.0 : 0.0;
        const QRectF available =
            QRectF(rect()).adjusted(18.0 + ruler, 18.0 + ruler,
                                     -18.0, -18.0);
        if (available.width() <= 1.0 || available.height() <= 1.0) {
            return;
        }
        const qreal paperAspect =
            m_paperMillimeters.width() / m_paperMillimeters.height();
        QSizeF pageSize = available.size();
        if (pageSize.width() / pageSize.height() > paperAspect) {
            pageSize.setWidth(pageSize.height() * paperAspect);
        } else {
            pageSize.setHeight(pageSize.width() / paperAspect);
        }
        const QRectF page(
            available.center().x() - pageSize.width() * 0.5,
            available.center().y() - pageSize.height() * 0.5,
            pageSize.width(), pageSize.height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 42));
        painter.drawRect(page.translated(5.0, 6.0));
        painter.setBrush(Qt::white);
        painter.setPen(QPen(QColor(QStringLiteral("#8b9aa3")), 0.8));
        painter.drawRect(page);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(page, m_image);

        if (!m_rulerVisible) {
            return;
        }
        const double xScale =
            page.width() / m_paperMillimeters.width();
        const double yScale =
            page.height() / m_paperMillimeters.height();
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(QStringLiteral("#3f5764")), 0.8));
        painter.setFont(QFont(QString(), 7));
        const double top = page.top() - 5.0;
        painter.drawLine(QPointF(page.left(), top),
                         QPointF(page.right(), top));
        for (int millimeter = 0;
             millimeter <= int(std::ceil(m_paperMillimeters.width()));
             ++millimeter) {
            const double x = page.left() + millimeter * xScale;
            if (x > page.right() + 0.5) {
                break;
            }
            const int length =
                millimeter % 10 == 0 ? 8
                : millimeter % 5 == 0 ? 5 : 2;
            painter.drawLine(QPointF(x, top),
                             QPointF(x, top - length));
            if (millimeter % 50 == 0) {
                painter.drawText(
                    QRectF(x - 16.0, top - 25.0, 32.0, 15.0),
                    Qt::AlignHCenter | Qt::AlignVCenter,
                    QString::number(millimeter));
            }
        }
        const double left = page.left() - 5.0;
        painter.drawLine(QPointF(left, page.top()),
                         QPointF(left, page.bottom()));
        for (int millimeter = 0;
             millimeter <= int(std::ceil(m_paperMillimeters.height()));
             ++millimeter) {
            const double y = page.top() + millimeter * yScale;
            if (y > page.bottom() + 0.5) {
                break;
            }
            const int length =
                millimeter % 10 == 0 ? 8
                : millimeter % 5 == 0 ? 5 : 2;
            painter.drawLine(QPointF(left, y),
                             QPointF(left - length, y));
            if (millimeter % 50 == 0) {
                painter.drawText(
                    QRectF(left - 36.0, y - 8.0, 30.0, 16.0),
                    Qt::AlignRight | Qt::AlignVCenter,
                    QString::number(millimeter));
            }
        }
        painter.drawText(
            QRectF(page.left(), page.top() - 24.0, 90.0, 15.0),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("打印标尺 (mm)"));
    }

private:
    QImage m_image;
    QSizeF m_paperMillimeters;
    bool m_rulerVisible = true;
};

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("速看CAD · 工程施工看图"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/logo.svg")));
    resize(1500, 940);
    setMinimumSize(1040, 680);

    QSettings settings = cacheSettings();
    m_cadTheme =
        settings.value(QStringLiteral("uiTheme"),
                       QStringLiteral("cadDark"))
                .toString()
            == QStringLiteral("cadLight")
        ? CadTheme::Light : CadTheme::Dark;

    m_view = new CadView;
    m_loadWatcher = new QFutureWatcher<DrawingLoadResult>(this);
    m_annotationAutoSaveTimer = new QTimer(this);
    m_annotationAutoSaveTimer->setSingleShot(true);
    m_annotationAutoSaveTimer->setInterval(400);
    connect(m_annotationAutoSaveTimer, &QTimer::timeout, this,
            &MainWindow::autoSaveAnnotationsNow);
    createActions();
    createInterface();
    createMenus();
    applyStyle();

    m_lastDirectory =
        settings.value(QStringLiteral("lastDirectory"),
                       QStandardPaths::writableLocation(
                           QStandardPaths::DocumentsLocation))
            .toString();
    refreshStartScreen();

    wireViewSignals(m_view);

    connect(m_loadWatcher, &QFutureWatcher<DrawingLoadResult>::finished,
            this, &MainWindow::finishDrawingLoad);
}

MainWindow::~MainWindow() = default;

void MainWindow::wireViewSignals(CadView *view)
{
    if (!view) {
        return;
    }
    view->setDrawingTheme(
        m_cadTheme == CadTheme::Light
            ? CadView::DrawingTheme::CadLight
            : CadView::DrawingTheme::CadDark);
    connect(view, &CadView::statusMessage, this,
            [this](const QString &message) {
                statusBar()->showMessage(message, 7000);
            });
    connect(view, &CadView::cursorWorldChanged, this,
            [this](const QPointF &point) {
                m_coordinateLabel->setText(
                    QStringLiteral("X  %1    Y  %2")
                        .arg(point.x(), 0, 'f', 0)
                        .arg(point.y(), 0, 'f', 0));
            });
    connect(view, &CadView::snapChanged, this,
            [this](const QString &snap) {
                if (!m_snapLabel) {
                    return;
                }
                m_snapLabel->setText(
                    snap.isEmpty() ? QStringLiteral("捕捉就绪")
                                   : QStringLiteral("捕捉  %1").arg(snap));
                m_snapLabel->setProperty("active", !snap.isEmpty());
                m_snapLabel->style()->unpolish(m_snapLabel);
                m_snapLabel->style()->polish(m_snapLabel);
            });
    connect(view, &CadView::measurementsChanged, this, [this, view] {
        if (view != m_view || !m_view->document()) {
            return;
        }
        refreshMeasurementTable();
        refreshStatus();
        setDirty(true);
        if (m_annotationAutoSaveTimer && m_view->document()) {
            m_annotationAutoSaveTimer->start();
        }
    });
    connect(view, &CadView::selectionChanged, this,
            [this, view](const QString &id) {
                if (view != m_view) {
                    return;
                }
                m_currentSelection = id;
                if (m_refreshingTable) {
                    return;
                }
                m_refreshingTable = true;
                for (int row = 0; row < m_measurementTable->rowCount(); ++row) {
                    QTableWidgetItem *idItem =
                        m_measurementTable->item(row, 0);
                    if (idItem
                        && idItem->data(Qt::UserRole).toString() == id) {
                        m_measurementTable->selectRow(row);
                        break;
                    }
                }
                m_refreshingTable = false;
            });
    connect(view, &CadView::calibrationRequested, this,
            &MainWindow::requestCalibration);
    connect(view, &CadView::textAnnotationRequested, this,
            &MainWindow::requestTextAnnotation);
    connect(view, &CadView::printAreaSelected, this,
            &MainWindow::handlePrintArea);
    connect(view, &CadView::toolChanged, this,
            [this, view](CadView::ToolMode mode) {
                if (view != m_view) {
                    return;
                }
                const QList<QAction *> actions = m_toolGroup->actions();
                for (QAction *action : actions) {
                    if (action->data().toInt() == int(mode)) {
                        action->setChecked(true);
                    }
                }
            });
    connect(view, &CadView::viewTransformChanged, this,
            [this, view] {
                if (view == m_view) {
                    refreshStatus();
                }
            });
    connect(view, &CadView::zoomChanged, this, [this, view](double percent) {
        if (m_zoomLabel && view == m_view) {
            m_zoomLabel->setText(
                QStringLiteral("缩放  %1%")
                    .arg(percent, 0, 'f', percent < 10.0 ? 1 : 0));
        }
    });
    connect(view, &CadView::activeSpaceChanged, this,
            [this, view](const QString &spaceName) {
                if (view != m_view) {
                    return;
                }
                for (int i = 0; i < m_layoutTabs->count(); ++i) {
                    if (m_layoutTabs->tabText(i) == spaceName) {
                        const QSignalBlocker blocker(m_layoutTabs);
                        m_layoutTabs->setCurrentIndex(i);
                        break;
                    }
                }
            });
}

QAction *MainWindow::makeAction(const QString &text, const QString &icon,
                                const QKeySequence &shortcut)
{
    auto *action = new QAction(
        themedCadIcon(icon, m_cadTheme == CadTheme::Dark), text, this);
    action->setProperty("cadIconName", icon);
    if (!shortcut.isEmpty()) {
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WindowShortcut);
    }
    return action;
}

void MainWindow::refreshActionIcons()
{
    const bool dark = m_cadTheme == CadTheme::Dark;
    const QList<QAction *> actions = findChildren<QAction *>();
    for (QAction *action : actions) {
        const QString iconName =
            action->property("cadIconName").toString();
        if (!iconName.isEmpty()) {
            action->setIcon(themedCadIcon(iconName, dark));
        }
    }
}

QAction *MainWindow::makeToolAction(const QString &text, const QString &icon,
                                    CadView::ToolMode mode,
                                    const QKeySequence &shortcut)
{
    QAction *action = makeAction(text, icon, shortcut);
    action->setCheckable(true);
    action->setData(int(mode));
    m_toolGroup->addAction(action);
    connect(action, &QAction::triggered, this, [this, mode] {
        m_view->beginTool(mode);
    });
    return action;
}

void MainWindow::createActions()
{
    m_toolGroup = new QActionGroup(this);
    m_toolGroup->setExclusive(true);

    m_openAction = makeAction(QStringLiteral("打开图纸"), QStringLiteral("open"),
                              QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("打开 DWG/DXF/PDF 图纸"), m_lastDirectory,
            QStringLiteral(
                "图纸 (*.dwg *.dxf *.pdf);;DWG (*.dwg);;"
                "DXF (*.dxf);;PDF (*.pdf)"));
        if (!path.isEmpty()) {
            openDrawing(path);
        }
    });
    m_saveAction =
        makeAction(QStringLiteral("保存标注"), QStringLiteral("save"),
                   QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this,
            &MainWindow::saveAnnotationsNow);
    m_saveAsAction = makeAction(QStringLiteral("标注另存为"),
                                QStringLiteral("save-as"));
    connect(m_saveAsAction, &QAction::triggered, this,
            &MainWindow::saveAnnotationsAs);
    m_exportSvgAction = makeAction(QStringLiteral("导出矢量 SVG"),
                                   QStringLiteral("export"));
    connect(m_exportSvgAction, &QAction::triggered, this,
            &MainWindow::exportSvgAs);
    m_printAction =
        makeAction(QStringLiteral("框选打印与预览"),
                   QStringLiteral("print"));
    connect(m_printAction, &QAction::triggered, this,
            [this] { requestPrint(); });

    m_selectAction = makeToolAction(QStringLiteral("选择/拖动标注"),
                                    QStringLiteral("select"),
                                    CadView::ToolMode::Select,
                                    QKeySequence(QStringLiteral("V")));
    m_eraseAction =
        makeToolAction(QStringLiteral("删除/框选删除标注"),
                       QStringLiteral("delete"), CadView::ToolMode::Erase);
    m_deleteAllAction =
        makeAction(QStringLiteral("删除所有标注"),
                   QStringLiteral("delete-all"));
    m_deleteAllAction->setToolTip(
        QStringLiteral("确认后删除当前图纸上的全部标注"));
    connect(m_deleteAllAction, &QAction::triggered, this, [this] {
        if (!m_view->document() || m_view->measurements().isEmpty()) {
            m_view->beginTool(CadView::ToolMode::Select);
            return;
        }
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, QStringLiteral("删除所有标注"),
            QStringLiteral("确定删除当前图纸上的全部标注吗？\n"
                           "此操作可以使用“撤销”恢复。"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            m_view->removeAllMeasurements();
        }
        m_view->beginTool(CadView::ToolMode::Select);
    });
    makeToolAction(QStringLiteral("对齐尺寸"), QStringLiteral("linear"),
                   CadView::ToolMode::Aligned,
                   QKeySequence(QStringLiteral("A")));
    makeToolAction(QStringLiteral("线性尺寸"), QStringLiteral("linear-horizontal"),
                   CadView::ToolMode::Linear,
                   QKeySequence(QStringLiteral("L")));
    makeToolAction(QStringLiteral("连续测量"), QStringLiteral("continuous"),
                   CadView::ToolMode::Continuous,
                   QKeySequence(QStringLiteral("C")));
    makeToolAction(QStringLiteral("批量测量"), QStringLiteral("batch"),
                   CadView::ToolMode::Batch);
    makeToolAction(QStringLiteral("矩形面积（含周长）"),
                   QStringLiteral("rectangle"), CadView::ToolMode::Rectangle);
    makeToolAction(QStringLiteral("多边形/闭合面积"),
                   QStringLiteral("polygon"), CadView::ToolMode::Polygon,
                   QKeySequence(QStringLiteral("P")));
    makeToolAction(QStringLiteral("圆：半径/面积/周长"),
                   QStringLiteral("circle-area"),
                   CadView::ToolMode::CircleArea);
    makeToolAction(QStringLiteral("填充面积"), QStringLiteral("filled-area"),
                   CadView::ToolMode::FilledArea);
    makeToolAction(QStringLiteral("半径"), QStringLiteral("radius"),
                   CadView::ToolMode::Radius);
    makeToolAction(QStringLiteral("角度"), QStringLiteral("angle"),
                   CadView::ToolMode::Angle);
    makeToolAction(QStringLiteral("弧长"), QStringLiteral("arc"),
                   CadView::ToolMode::ArcLength);
    makeToolAction(QStringLiteral("坐标标注"), QStringLiteral("coordinate"),
                   CadView::ToolMode::Coordinate);
    makeToolAction(QStringLiteral("点到直线距离"),
                   QStringLiteral("point-line"),
                   CadView::ToolMode::PointToLine);
    makeToolAction(QStringLiteral("文字注释"), QStringLiteral("text"),
                   CadView::ToolMode::Text,
                   QKeySequence(QStringLiteral("T")));

    const auto addAuxiliaryTool =
        [this](const QString &text, const QString &icon,
               CadView::ToolMode mode,
               const std::function<void()> &starter) {
            QAction *action = makeAction(text, icon);
            action->setCheckable(true);
            action->setData(int(mode));
            m_toolGroup->addAction(action);
            connect(action, &QAction::triggered, this, starter);
        };
    addAuxiliaryTool(QStringLiteral("定距测量"),
                     QStringLiteral("fixed-distance"),
                     CadView::ToolMode::FixedDistance,
                     [this] { beginFixedDistance(); });
    addAuxiliaryTool(QStringLiteral("侧面积"),
                     QStringLiteral("side-area"),
                     CadView::ToolMode::SideArea,
                     [this] { beginSideArea(); });
    addAuxiliaryTool(QStringLiteral("面积偏移"),
                     QStringLiteral("area-offset"),
                     CadView::ToolMode::AreaOffset,
                     [this] { beginAreaOffset(); });
    makeToolAction(QStringLiteral("已知长度校准比例"),
                   QStringLiteral("calibrate"),
                   CadView::ToolMode::Calibrate,
                   QKeySequence(QStringLiteral("R")));
    m_undoAction =
        makeAction(QStringLiteral("撤销"), QStringLiteral("undo"),
                   QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this,
            [this] { if (m_view) m_view->undo(); });
    m_redoAction =
        makeAction(QStringLiteral("重做"), QStringLiteral("redo"),
                   QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this,
            [this] { if (m_view) m_view->redo(); });
    m_deleteAction =
        makeAction(QStringLiteral("删除标注"), QStringLiteral("delete"),
                   QKeySequence::Delete);
    connect(m_deleteAction, &QAction::triggered, this, [this] {
        const int row = m_measurementTable->currentRow();
        if (row >= 0) {
            QTableWidgetItem *item = m_measurementTable->item(row, 0);
            if (item) {
                m_view->removeMeasurement(
                    item->data(Qt::UserRole).toString());
                m_view->beginTool(CadView::ToolMode::Select);
            }
        }
    });
    m_annotationVisibilityAction =
        makeAction(QStringLiteral("标注显示"),
                   QStringLiteral("show-all"));
    m_annotationVisibilityAction->setCheckable(true);
    m_annotationVisibilityAction->setChecked(true);
    m_annotationVisibilityAction->setToolTip(
        QStringLiteral("显示全部标注；再次点击隐藏全部标注"));
    connect(m_annotationVisibilityAction, &QAction::toggled,
            this, [this](bool visible) {
                if (m_view) {
                    if (visible) {
                        m_view->showAllMeasurements();
                    } else {
                        m_view->hideAllMeasurements();
                    }
                }
                const QString icon =
                    visible ? QStringLiteral("show-all")
                            : QStringLiteral("hide-all");
                m_annotationVisibilityAction->setProperty(
                    "cadIconName", icon);
                m_annotationVisibilityAction->setIcon(
                    themedCadIcon(icon, m_cadTheme == CadTheme::Dark));
            });
    m_annotationSettingsAction =
        makeAction(QStringLiteral("标注设置：样式、颜色、单位与精度"),
                   QStringLiteral("settings"));
    connect(m_annotationSettingsAction, &QAction::triggered, this,
            &MainWindow::showAnnotationSettings);
    m_layerPanelAction =
        makeAction(QStringLiteral("图层管理"), QStringLiteral("layers"));
    m_layerPanelAction->setCheckable(true);
    m_annotationManagerAction =
        makeAction(QStringLiteral("标注管理"),
                   QStringLiteral("annotations"));
    m_annotationManagerAction->setCheckable(true);

    m_fitAction =
        makeAction(QStringLiteral("显示全图"), QStringLiteral("fit"),
                   QKeySequence(Qt::Key_0));
    connect(m_fitAction, &QAction::triggered, this,
            [this] { if (m_view) m_view->fitToDrawing(); });
    m_zoomWindowAction = makeToolAction(
        QStringLiteral("窗口缩放"), QStringLiteral("zoom-window"),
        CadView::ToolMode::ZoomWindow,
        QKeySequence(QStringLiteral("Z")));
    m_lineWeightAction =
        makeAction(QStringLiteral("显示 CAD 线宽"),
                   QStringLiteral("lineweight"));
    m_lineWeightAction->setCheckable(true);
    m_lineWeightAction->setChecked(false);
    connect(m_lineWeightAction, &QAction::toggled, this,
            [this](bool visible) {
                if (m_view) m_view->setLineWeightsVisible(visible);
            });
    m_snapEnabledAction =
        makeAction(QStringLiteral("启用对象捕捉"),
                   QStringLiteral("snap"));
    m_snapEnabledAction->setShortcut(QKeySequence(Qt::Key_F3));
    m_snapEnabledAction->setShortcutContext(Qt::WindowShortcut);
    m_snapEnabledAction->setCheckable(true);
    m_snapEnabledAction->setChecked(true);
    connect(m_snapEnabledAction, &QAction::toggled, this,
            [this](bool enabled) {
                if (m_view) m_view->setSnapEnabled(enabled);
            });
    m_gridAction =
        makeAction(QStringLiteral("显示栅格"), QStringLiteral("grid"));
    m_gridAction->setShortcut(QKeySequence(Qt::Key_F7));
    m_gridAction->setShortcutContext(Qt::WindowShortcut);
    m_gridAction->setCheckable(true);
    m_gridAction->setChecked(false);
    connect(m_gridAction, &QAction::toggled, this,
            [this](bool visible) {
                if (m_view) m_view->setGridVisible(visible);
            });
    m_orthoAction =
        makeAction(QStringLiteral("正交模式"), QStringLiteral("ortho"));
    m_orthoAction->setShortcut(QKeySequence(Qt::Key_F8));
    m_orthoAction->setShortcutContext(Qt::WindowShortcut);
    m_orthoAction->setCheckable(true);
    connect(m_orthoAction, &QAction::toggled, this,
            [this](bool enabled) {
                if (m_view) m_view->setOrthoEnabled(enabled);
            });
}

void MainWindow::createInterface()
{
    auto *toolbar = addToolBar(QStringLiteral("主工具栏"));
    m_mainToolbar = toolbar;
    m_mainToolbar->setEnabled(false);
    toolbar->setObjectName(QStringLiteral("mainToolbar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize(QSize(22, 22));
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->addAction(m_openAction);
    toolbar->addAction(m_saveAction);
    toolbar->addAction(m_printAction);
    toolbar->addAction(m_layerPanelAction);
    toolbar->addAction(m_annotationManagerAction);
    toolbar->addAction(m_annotationSettingsAction);
    toolbar->addSeparator();
    toolbar->addAction(m_selectAction);
    toolbar->addAction(m_fitAction);
    toolbar->addAction(m_zoomWindowAction);
    toolbar->addSeparator();
    for (QAction *action : m_toolGroup->actions()) {
        if (action != m_selectAction && action != m_zoomWindowAction
            && action != m_eraseAction) {
            toolbar->addAction(action);
        }
    }
    toolbar->addAction(m_eraseAction);
    toolbar->addAction(m_deleteAllAction);
    toolbar->addSeparator();
    toolbar->addAction(m_lineWeightAction);
    toolbar->addAction(m_snapEnabledAction);
    toolbar->addAction(m_orthoAction);
    toolbar->addAction(m_gridAction);
    toolbar->addAction(m_annotationVisibilityAction);
    toolbar->addWidget(new QLabel(QStringLiteral("    ")));

    auto *titleStack = new QWidget;
    auto *titleLayout = new QVBoxLayout(titleStack);
    titleLayout->setContentsMargins(8, 2, 8, 2);
    titleLayout->setSpacing(0);
    m_titleLabel = new QLabel(QStringLiteral("速看CAD"));
    m_titleLabel->setObjectName(QStringLiteral("documentTitle"));
    m_drawingMetaLabel =
        new QLabel(QStringLiteral("DWG / DXF / PDF · 施工测量"));
    m_drawingMetaLabel->setObjectName(QStringLiteral("documentMeta"));
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addWidget(m_drawingMetaLabel);

    QFrame *leftPanel = panelFrame();
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(12, 14, 12, 12);
    leftLayout->setSpacing(10);
    auto *layerHeader = new QHBoxLayout;
    auto *layerTitle = new QLabel(QStringLiteral("图层"));
    layerTitle->setObjectName(QStringLiteral("panelTitle"));
    auto *showAll = smallToolButton(QStringLiteral("全部显示"));
    auto *hideAll = smallToolButton(QStringLiteral("全部隐藏"));
    layerHeader->addWidget(layerTitle);
    layerHeader->addStretch();
    layerHeader->addWidget(showAll);
    layerHeader->addWidget(hideAll);
    leftLayout->addLayout(layerHeader);
    m_layerTree = new QTreeWidget;
    m_layerTree->setHeaderHidden(true);
    m_layerTree->setRootIsDecorated(false);
    m_layerTree->setIndentation(10);
    leftLayout->addWidget(m_layerTree, 1);
    auto *layerHint = new QLabel(
        QStringLiteral("勾选控制图层显示。颜色与 CAD 图层同步。"));
    layerHint->setWordWrap(true);
    layerHint->setObjectName(QStringLiteral("hintLabel"));
    leftLayout->addWidget(layerHint);
    connect(m_layerTree, &QTreeWidget::itemChanged, this,
            &MainWindow::updateLayerVisibility);
    connect(showAll, &QToolButton::clicked, this, [this] {
        for (int i = 0; i < m_layerTree->topLevelItemCount(); ++i) {
            m_layerTree->topLevelItem(i)->setCheckState(0, Qt::Checked);
        }
    });
    connect(hideAll, &QToolButton::clicked, this, [this] {
        for (int i = 0; i < m_layerTree->topLevelItemCount(); ++i) {
            m_layerTree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        }
    });
    m_layerDialog = new QDialog(this, Qt::Tool);
    m_layerDialog->setWindowTitle(QStringLiteral("图层管理"));
    m_layerDialog->setModal(false);
    m_layerDialog->resize(320, 620);
    auto *layerDialogLayout = new QVBoxLayout(m_layerDialog);
    layerDialogLayout->setContentsMargins(0, 0, 0, 0);
    layerDialogLayout->addWidget(leftPanel);
    m_layerDialog->hide();
    connect(m_layerPanelAction, &QAction::toggled, m_layerDialog,
            &QWidget::setVisible);
    connect(m_layerDialog, &QDialog::finished, this, [this] {
        m_layerPanelAction->setChecked(false);
    });

    auto *canvasFrame = new QFrame;
    m_canvasPage = canvasFrame;
    canvasFrame->setObjectName(QStringLiteral("canvasFrame"));
    auto *canvasLayout = new QVBoxLayout(canvasFrame);
    canvasLayout->setContentsMargins(1, 1, 1, 1);
    m_documentStack = new QStackedWidget;
    m_documentStack->addWidget(m_view);
    canvasLayout->addWidget(m_documentStack, 1);
    auto *tabsRow = new QHBoxLayout;
    tabsRow->setContentsMargins(8, 4, 8, 3);
    tabsRow->addStretch();
    m_layoutTabs = new QTabBar;
    m_layoutTabs->setObjectName(QStringLiteral("layoutTabs"));
    m_layoutTabs->setDrawBase(false);
    m_layoutTabs->setExpanding(false);
    m_layoutTabs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_layoutTabs->setMovable(false);
    tabsRow->addWidget(m_layoutTabs);
    tabsRow->addStretch();
    canvasLayout->addLayout(tabsRow);
    connect(m_layoutTabs, &QTabBar::currentChanged, this,
            [this](int index) {
                if (!m_view->document() || index < 0) {
                    return;
                }
                const QString name = m_layoutTabs->tabText(index);
                m_view->setActiveSpace(name);
                m_view->setFocus();
            });
    QFrame *rightPanel = panelFrame();
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(12, 14, 12, 12);
    rightLayout->setSpacing(10);
    auto *measureTitle = new QLabel(QStringLiteral("测量与标注"));
    measureTitle->setObjectName(QStringLiteral("panelTitle"));
    rightLayout->addWidget(measureTitle);
    auto *unitRow = new QHBoxLayout;
    auto *unitLabel = new QLabel(QStringLiteral("单位"));
    m_unitCombo = new QComboBox;
    const MeasuringUnit units[] = {
        MeasuringUnit::Millimeter, MeasuringUnit::Centimeter,
        MeasuringUnit::Meter, MeasuringUnit::Kilometer,
        MeasuringUnit::Inch, MeasuringUnit::Foot
    };
    for (MeasuringUnit unit : units) {
        m_unitCombo->addItem(measuringUnitLabel(unit), int(unit));
    }
    auto *precisionLabel = new QLabel(QStringLiteral("精度"));
    m_precisionCombo = new QComboBox;
    for (int i = 0; i <= 4; ++i) {
        m_precisionCombo->addItem(QStringLiteral("%1 位").arg(i), i);
    }
    m_precisionCombo->setCurrentIndex(0);
    unitRow->addWidget(unitLabel);
    unitRow->addWidget(m_unitCombo, 1);
    unitRow->addWidget(precisionLabel);
    unitRow->addWidget(m_precisionCombo);
    rightLayout->addLayout(unitRow);
    connect(m_unitCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::changeUnit);
    connect(m_precisionCombo, &QComboBox::currentIndexChanged, this,
            &MainWindow::changePrecision);

    m_measurementTable = new QTableWidget(0, 4);
    m_measurementTable->setHorizontalHeaderLabels(
        {QStringLiteral("显示"), QStringLiteral("类型"),
         QStringLiteral("测量结果"), QStringLiteral("备注")});
    m_measurementTable->verticalHeader()->setVisible(false);
    m_measurementTable->horizontalHeader()->setStretchLastSection(true);
    m_measurementTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_measurementTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    m_measurementTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    m_measurementTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_measurementTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_measurementTable->setEditTriggers(
        QAbstractItemView::DoubleClicked
        | QAbstractItemView::SelectedClicked);
    rightLayout->addWidget(m_measurementTable, 1);
    connect(m_measurementTable, &QTableWidget::itemSelectionChanged, this,
            [this] {
                if (m_refreshingTable) {
                    return;
                }
                const int row = m_measurementTable->currentRow();
                if (row >= 0 && m_measurementTable->item(row, 0)) {
                    m_view->selectMeasurement(
                        m_measurementTable->item(row, 0)
                            ->data(Qt::UserRole)
                            .toString());
                }
            });
    connect(m_measurementTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
                if (m_refreshingTable || !item) {
                    return;
                }
                QTableWidgetItem *idItem =
                    m_measurementTable->item(item->row(), 0);
                if (!idItem) {
                    return;
                }
                const QString id = idItem->data(Qt::UserRole).toString();
                if (item->column() == 0) {
                    m_view->setMeasurementVisible(
                        id, item->checkState() == Qt::Checked);
                } else if (item->column() == 3) {
                    m_view->setMeasurementNote(id, item->text());
                }
            });
    connect(m_measurementTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                if (m_measurementTable->item(row, 0)) {
                    m_view->focusMeasurement(
                        m_measurementTable->item(row, 0)
                            ->data(Qt::UserRole)
                            .toString());
                }
            });

    m_summaryLabel = new QLabel(QStringLiteral("尚未添加测量标注"));
    m_summaryLabel->setWordWrap(true);
    m_summaryLabel->setObjectName(QStringLiteral("summaryLabel"));
    rightLayout->addWidget(m_summaryLabel);
    auto *annotationButtons = new QHBoxLayout;
    auto *deleteButton = new QPushButton(QStringLiteral("删除"));
    auto *focusButton = new QPushButton(QStringLiteral("定位"));
    auto *clearButton = new QPushButton(QStringLiteral("清空"));
    annotationButtons->addWidget(deleteButton);
    annotationButtons->addWidget(focusButton);
    annotationButtons->addWidget(clearButton);
    rightLayout->addLayout(annotationButtons);
    connect(deleteButton, &QPushButton::clicked, m_deleteAction,
            &QAction::trigger);
    connect(focusButton, &QPushButton::clicked, this, [this] {
        const int row = m_measurementTable->currentRow();
        if (row >= 0 && m_measurementTable->item(row, 0)) {
            m_view->focusMeasurement(
                m_measurementTable->item(row, 0)
                    ->data(Qt::UserRole)
                    .toString());
        }
    });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        if (m_view->measurements().isEmpty()) {
            return;
        }
        if (QMessageBox::question(
                this, QStringLiteral("清空标注"),
                QStringLiteral("确定删除当前图纸的全部测量标注吗？"))
            == QMessageBox::Yes) {
            m_view->removeAllMeasurements();
        }
    });
    m_measurementDock =
        new QDockWidget(QStringLiteral("标注管理"), this);
    m_measurementDock->setObjectName(QStringLiteral("annotationDock"));
    m_measurementDock->setAllowedAreas(
        Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_measurementDock->setFeatures(
        QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_measurementDock->setMinimumWidth(310);
    m_measurementDock->setWidget(rightPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_measurementDock);
    m_measurementDock->hide();
    connect(m_annotationManagerAction, &QAction::toggled,
            m_measurementDock, &QWidget::setVisible);
    connect(m_measurementDock, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
                const QSignalBlocker blocker(m_annotationManagerAction);
                m_annotationManagerAction->setChecked(visible);
            });
    m_startScreen = new StartScreen;
    connect(m_startScreen, &StartScreen::browseRequested, this,
            [this] { m_openAction->trigger(); });
    connect(m_startScreen, &StartScreen::openRequested, this,
            &MainWindow::openDrawing);
    connect(m_startScreen, &StartScreen::removeRecentRequested, this,
            &MainWindow::removeRecentDrawing);
    m_centralStack = new QStackedWidget;
    m_centralStack->addWidget(m_startScreen);
    m_centralStack->addWidget(m_canvasPage);
    auto *workspaceShell = new QWidget;
    auto *workspaceLayout = new QVBoxLayout(workspaceShell);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    auto *documentTabsRow = new QHBoxLayout;
    documentTabsRow->setContentsMargins(8, 5, 8, 0);
    m_documentTabs = new QTabBar;
    m_documentTabs->setObjectName(QStringLiteral("documentTabs"));
    m_documentTabs->setDrawBase(false);
    m_documentTabs->setExpanding(false);
    m_documentTabs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_documentTabs->setMovable(false);
    m_documentTabs->setDocumentMode(true);
    m_documentTabs->setTabsClosable(true);
    m_documentTabs->setUsesScrollButtons(true);
    m_documentTabs->setElideMode(Qt::ElideMiddle);
    m_documentTabs->addTab(QStringLiteral("启动页"));
    m_documentTabs->setTabToolTip(0, QStringLiteral("速看CAD 启动工作区"));
    m_documentTabs->setTabButton(0, QTabBar::RightSide, nullptr);
    documentTabsRow->addWidget(m_documentTabs);
    documentTabsRow->addStretch();
    workspaceLayout->addLayout(documentTabsRow);
    workspaceLayout->addWidget(m_centralStack, 1);
    setCentralWidget(workspaceShell);
    m_centralStack->setCurrentWidget(m_startScreen);
    connect(m_documentTabs, &QTabBar::currentChanged, this,
            [this](int index) {
                if (index == 0) {
                    m_centralStack->setCurrentWidget(m_startScreen);
                    if (m_mainToolbar) {
                        m_mainToolbar->setEnabled(false);
                    }
                    return;
                }
                activateDocument(index - 1);
            });
    connect(m_documentTabs, &QTabBar::tabCloseRequested, this,
            [this](int index) {
                if (index > 0) {
                    closeDocumentTab(index - 1);
                }
            });

    m_coordinateLabel = new QLabel(QStringLiteral("X  —    Y  —"));
    m_snapLabel = nullptr;
    m_scaleLabel = new QLabel(QStringLiteral("比例 未校准"));
    m_zoomLabel = new QLabel(QStringLiteral("缩放  100%"));
    m_progress = new QProgressBar;
    m_progress->setRange(0, 0);
    m_progress->setMaximumWidth(120);
    m_progress->setTextVisible(false);
    m_progress->hide();
    statusBar()->addWidget(m_coordinateLabel, 1);
    statusBar()->addPermanentWidget(m_scaleLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);
    statusBar()->addPermanentWidget(m_progress);
    statusBar()->addPermanentWidget(
        new QLabel(QStringLiteral("OpenGL 加速 · 8× MSAA")));
}

void MainWindow::createMenus()
{
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    fileMenu->addAction(m_openAction);
    fileMenu->addAction(m_saveAction);
    fileMenu->addAction(m_saveAsAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_printAction);
    fileMenu->addAction(m_exportSvgAction);
    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(QStringLiteral("退出"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto *editMenu = menuBar()->addMenu(QStringLiteral("编辑"));
    editMenu->addAction(m_undoAction);
    editMenu->addAction(m_redoAction);
    editMenu->addAction(m_deleteAction);
    editMenu->addAction(m_deleteAllAction);
    editMenu->addSeparator();
    editMenu->addAction(m_annotationVisibilityAction);
    editMenu->addAction(m_annotationSettingsAction);

    auto *measureMenu = menuBar()->addMenu(QStringLiteral("测量"));
    for (QAction *action : m_toolGroup->actions()) {
        if (action != m_zoomWindowAction) {
            measureMenu->addAction(action);
        }
    }
    measureMenu->addSeparator();
    auto *scaleAction = measureMenu->addAction(
        QStringLiteral("设置已知线段比例..."));
    connect(scaleAction, &QAction::triggered, this,
            [this] { m_view->beginTool(CadView::ToolMode::Calibrate); });
    auto *unitAction = measureMenu->addAction(
        QStringLiteral("面积 / 长度 / 坐标与精度设置..."));
    connect(unitAction, &QAction::triggered, this, [this] {
        showAnnotationSettings();
    });

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    viewMenu->addAction(m_fitAction);
    viewMenu->addAction(m_zoomWindowAction);
    viewMenu->addAction(m_lineWeightAction);
    auto *themeMenu = viewMenu->addMenu(QStringLiteral("主题"));
    auto *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    m_darkThemeAction = themeMenu->addAction(
        themedCadIcon(QStringLiteral("theme-dark"),
                      m_cadTheme == CadTheme::Dark),
        QStringLiteral("CAD 深色"));
    m_lightThemeAction = themeMenu->addAction(
        themedCadIcon(QStringLiteral("theme-light"),
                      m_cadTheme == CadTheme::Dark),
        QStringLiteral("CAD 浅色"));
    m_darkThemeAction->setProperty("cadIconName",
                                   QStringLiteral("theme-dark"));
    m_lightThemeAction->setProperty("cadIconName",
                                    QStringLiteral("theme-light"));
    m_darkThemeAction->setCheckable(true);
    m_lightThemeAction->setCheckable(true);
    themeGroup->addAction(m_darkThemeAction);
    themeGroup->addAction(m_lightThemeAction);
    (m_cadTheme == CadTheme::Dark
         ? m_darkThemeAction : m_lightThemeAction)->setChecked(true);
    connect(m_darkThemeAction, &QAction::triggered, this, [this] {
        setCadTheme(CadTheme::Dark);
    });
    connect(m_lightThemeAction, &QAction::triggered, this, [this] {
        setCadTheme(CadTheme::Light);
    });
    viewMenu->addSeparator();
    viewMenu->addAction(m_layerPanelAction);
    viewMenu->addAction(m_annotationManagerAction);
    auto *zoomIn = viewMenu->addAction(QStringLiteral("放大"));
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, this, [this] {
        m_view->zoomAt(1.25, m_view->rect().center());
    });
    auto *zoomOut = viewMenu->addAction(QStringLiteral("缩小"));
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, [this] {
        m_view->zoomAt(0.8, m_view->rect().center());
    });
    viewMenu->addSeparator();
    viewMenu->addAction(m_snapEnabledAction);
    viewMenu->addAction(m_orthoAction);
    viewMenu->addAction(m_gridAction);
    auto *snapMenu = viewMenu->addMenu(QStringLiteral("对象捕捉设置"));
    const QList<QPair<QString, SnapKind>> snapItems = {
        {QStringLiteral("端点"), SnapKind::Endpoint},
        {QStringLiteral("中点"), SnapKind::Midpoint},
        {QStringLiteral("交点"), SnapKind::Intersection},
        {QStringLiteral("垂足"), SnapKind::Perpendicular},
        {QStringLiteral("圆心 / 象限点"), SnapKind::Center},
        {QStringLiteral("最近点"), SnapKind::Nearest}
    };
    for (const auto &pair : snapItems) {
        QAction *action = snapMenu->addAction(pair.first);
        action->setCheckable(true);
        action->setChecked(true);
        action->setData(int(pair.second));
        m_snapActions.append(action);
        connect(action, &QAction::toggled, this,
                &MainWindow::updateSnapModesFromMenu);
    }

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("帮助"));
    QAction *help = helpMenu->addAction(QStringLiteral("使用帮助"));
    help->setShortcut(QKeySequence::HelpContents);
    connect(help, &QAction::triggered, this, &MainWindow::showHelp);
    helpMenu->addSeparator();
    QAction *about = helpMenu->addAction(QStringLiteral("关于速看CAD"));
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::applyStyle()
{
    refreshActionIcons();
    if (m_cadTheme == CadTheme::Light) {
        setStyleSheet(QStringLiteral(R"(
            QMainWindow { background: #eef1f3; color: #17252d; }
            QDialog { background: #ffffff; color: #17232b; }
            QDialog QLabel { color: #17232b; }
            QDialog QCheckBox, QDialog QRadioButton { color: #17232b; }
            QDialog QComboBox, QDialog QSpinBox, QDialog QDoubleSpinBox,
            QDialog QLineEdit, QDialog QTextEdit, QDialog QTextBrowser {
                background: #ffffff; color: #17232b;
                border: 1px solid #aebbc4; border-radius: 2px;
                padding: 4px 7px;
            }
            QDialog QComboBox QAbstractItemView {
                background: #ffffff; color: #17232b;
                selection-background-color: #dceffd;
                selection-color: #102d3d;
            }
            QDialog QPushButton, QDialog QToolButton {
                background: #ffffff; color: #1d2b34;
                border: 1px solid #aebbc4; border-radius: 2px;
                padding: 6px 10px;
            }
            QDialog QPushButton:hover, QDialog QToolButton:hover {
                background: #eef6fb; border-color: #4c93ba;
            }
            QDialog QTreeWidget, QDialog QTableWidget {
                background: #ffffff; alternate-background-color: #f4f7f9;
                color: #17232b; border: 1px solid #b8c5cd;
                selection-background-color: #dceffd;
                selection-color: #102d3d;
            }
            QDialog QHeaderView::section {
                background: #e9eef2; color: #253740;
                border: none; border-bottom: 1px solid #c7d2d9;
                padding: 6px;
            }
            QDialog QFrame#sidePanel {
                background: #ffffff; border: none;
            }
            QDialog QLabel#panelTitle { color: #102f40; }
            QDialog QLabel#hintLabel { color: #52646f; }
            QMenuBar {
                background: #f5f6f7; color: #263842;
                padding: 2px 6px; border-bottom: 1px solid #c9d1d6;
            }
            QMenuBar::item {
                padding: 5px 10px; background: transparent;
            }
            QMenuBar::item:selected {
                background: #dce8ef; border-radius: 1px;
            }
            QMenu {
                background: #ffffff; color: #263842;
                border: 1px solid #b8c4cb; padding: 3px;
            }
            QMenu::item {
                padding: 6px 26px 6px 10px; border-radius: 1px;
            }
            QMenu::item:selected {
                background: #dcecf5; color: #102d3d;
                border-left: 2px solid #287da6;
            }
            QMenu::separator {
                height: 1px; background: #d7dee3; margin: 4px 8px;
            }
            QToolBar#mainToolbar {
                background: #f7f8f9; border: none;
                border-bottom: 1px solid #c9d1d6;
                spacing: 2px; padding: 5px 7px;
            }
            QToolBar#mainToolbar::separator {
                background: #9eabb3; width: 1px;
                margin: 5px 6px;
            }
            QToolButton {
                background: transparent; border: 1px solid transparent;
                border-radius: 1px; padding: 5px; color: #31434d;
            }
            QToolButton:hover {
                background: #e7edf1; border-color: #bdc9d0;
            }
            QToolButton:checked {
                background: #d8eaf4; border-color: #3b91ba;
                color: #14394c;
            }
            QToolButton#styleChoice {
                background: #ffffff; border: 1px solid #b9c4cb;
                border-radius: 2px; color: #31434d; padding: 5px;
            }
            QToolButton#styleChoice:hover {
                background: #edf5f9; border-color: #5797b6;
            }
            QToolButton#styleChoice:checked {
                background: #e1f4e9; border: 2px solid #168c4e;
                color: #145d36;
            }
            QFrame#sidePanel { background: #f7f8f9; border: none; }
            QFrame#canvasFrame {
                background: #f7f7f4; border: 1px solid #bcc6cc;
            }
            QDockWidget { background: #f5f7f8; color: #263842; }
            QDockWidget::title {
                background: #e9edf0; border-bottom: 1px solid #c4cdd3;
                color: #334750; padding: 7px 9px; text-align: left;
            }
            QLabel#panelTitle {
                color: #173847; font-size: 12px; font-weight: 600;
                letter-spacing: 1px;
            }
            QLabel#documentTitle {
                color: #173847; font-weight: 600; font-size: 13px;
            }
            QLabel#documentMeta { color: #63747d; font-size: 10px; }
            QLabel#hintLabel { color: #5b6c75; font-size: 11px; }
            QLabel#summaryLabel {
                background: #ffffff; border: 1px solid #c5cfd5;
                border-radius: 1px; padding: 9px; color: #4b5e68;
            }
            QTreeWidget, QTableWidget {
                background: #ffffff; alternate-background-color: #f4f7f8;
                border: 1px solid #c0cbd1; border-radius: 1px;
                color: #253943; selection-background-color: #d8eaf4;
                selection-color: #14384a; gridline-color: #e0e5e8;
            }
            QTreeWidget::item, QTableWidget::item { padding: 5px; }
            QHeaderView::section {
                background: #e9edf0; color: #455b65; border: none;
                border-bottom: 1px solid #c4cdd3; padding: 6px;
            }
            QComboBox {
                background: #ffffff; border: 1px solid #b7c3ca;
                border-radius: 1px; padding: 5px 8px; color: #253943;
            }
            QComboBox::drop-down { border: none; width: 20px; }
            QComboBox QAbstractItemView {
                background: #ffffff; color: #253943;
                selection-background-color: #d8eaf4;
                selection-color: #14384a;
            }
            QPushButton {
                background: #ffffff; border: 1px solid #b7c3ca;
                border-radius: 1px; color: #253943; padding: 6px 12px;
            }
            QPushButton:hover {
                background: #edf5f9; border-color: #5797b6;
            }
            QStatusBar {
                background: #ffffff; color: #263842;
                border-top: 1px solid #c5d0d7;
            }
            QStatusBar QLabel { padding: 2px 8px; }
            QCheckBox { color: #31434d; spacing: 6px; }
            QSplitter::handle { background: #c5ced4; }
            QScrollBar:vertical { background: #eef1f3; width: 10px; }
            QScrollBar::handle:vertical {
                background: #aebbc3; border-radius: 0; min-height: 24px;
            }
            QScrollBar::add-line:vertical,
            QScrollBar::sub-line:vertical { height: 0; }
            QScrollBar:horizontal { background: #eef1f3; height: 10px; }
            QScrollBar::handle:horizontal {
                background: #aebbc3; border-radius: 0; min-width: 24px;
            }
            QScrollBar::add-line:horizontal,
            QScrollBar::sub-line:horizontal { width: 0; }
            QProgressBar {
                background: #dfe5e8; border: none;
                border-radius: 0; height: 4px;
            }
            QProgressBar::chunk { background: #287da6; }
            QTabBar::tab {
                background: #e9edf0; color: #52646e;
                border: 1px solid #bcc7cd; border-bottom: none;
                padding: 5px 18px; margin: 0 1px;
                border-top-left-radius: 1px;
                border-top-right-radius: 1px;
            }
            QTabBar::tab:selected {
                background: #ffffff; color: #173847;
                border-color: #6ca6c2;
            }
            QTabBar::tab:disabled { color: #9ca8ae; }
            QTabBar#documentTabs {
                background: transparent; border: none; spacing: 3px;
            }
            QTabBar#documentTabs::tab {
                background: #e9edf0; color: #5b6d76;
                border: 1px solid #bec9cf; border-bottom: none;
                border-top-left-radius: 3px;
                border-top-right-radius: 3px;
                padding: 4px 30px 3px 13px; margin: 1px 2px 0 0;
                min-width: 120px;
            }
            QTabBar#documentTabs::tab:hover {
                background: #f3f7f9; color: #203a48;
                border-color: #93aebc;
            }
            QTabBar#documentTabs::tab:selected {
                background: #ffffff; color: #102f40;
                border-top: 2px solid #287da6;
                border-left-color: #9eb4c0;
                border-right-color: #9eb4c0;
                padding-top: 3px;
            }
            QTabBar#documentTabs::close-button {
                image: url(:/icons/close-tab.svg);
                width: 12px; height: 12px;
                subcontrol-position: center right;
                margin-right: 6px;
            }
            QTabBar#documentTabs::close-button:hover {
                background: #c04b47; border-radius: 7px;
            }
            QTabBar#layoutTabs {
                background: transparent; border: none; spacing: 3px;
            }
            QTabBar#layoutTabs::tab {
                background: #e9edf0; color: #60727b;
                border: 1px solid #bdc8ce;
                border-bottom: 2px solid transparent;
                border-radius: 0; padding: 3px 22px; margin-top: 2px;
            }
            QTabBar#layoutTabs::tab:hover {
                background: #f4f8fa; color: #263f4c;
            }
            QTabBar#layoutTabs::tab:selected {
                background: #ffffff; color: #102f40;
                border-color: #9db3bf;
                border-bottom: 2px solid #287da6;
            }
        )"));
        if (m_startScreen) {
            m_startScreen->setCadTheme(false);
        }
        return;
    }
    setStyleSheet(QStringLiteral(R"(
        QMainWindow { background: #0b0e11; color: #d7dde2; }
        QDialog { background: #ffffff; color: #17232b; }
        QDialog QLabel { color: #17232b; }
        QDialog QCheckBox, QDialog QRadioButton { color: #17232b; }
        QDialog QComboBox, QDialog QSpinBox, QDialog QDoubleSpinBox,
        QDialog QLineEdit, QDialog QTextEdit, QDialog QTextBrowser {
            background: #ffffff; color: #17232b;
            border: 1px solid #b8c5cd; border-radius: 2px;
            padding: 4px 7px;
        }
        QDialog QComboBox QAbstractItemView {
            background: #ffffff; color: #17232b;
            selection-background-color: #dceffd;
            selection-color: #102d3d;
        }
        QDialog QPushButton, QDialog QToolButton {
            background: #ffffff; color: #1d2b34;
            border: 1px solid #aebbc4; border-radius: 2px;
            padding: 6px 10px;
        }
        QDialog QPushButton:hover, QDialog QToolButton:hover {
            background: #eef6fb; border-color: #5a9fc4;
        }
        QDialog QTreeWidget, QDialog QTableWidget {
            background: #ffffff; alternate-background-color: #f4f7f9;
            color: #17232b; border: 1px solid #b8c5cd;
            selection-background-color: #dceffd;
            selection-color: #102d3d;
        }
        QDialog QHeaderView::section {
            background: #e9eef2; color: #253740;
            border: none; border-bottom: 1px solid #c7d2d9;
            padding: 6px;
        }
        QDialog QFrame#sidePanel {
            background: #ffffff; border: none;
        }
        QDialog QLabel#panelTitle { color: #102f40; }
        QDialog QLabel#hintLabel { color: #52646f; }
        QMenuBar { background: #11161a; color: #b9c2c9; padding: 2px 6px; border-bottom: 1px solid #252c32; }
        QMenuBar::item { padding: 5px 10px; background: transparent; }
        QMenuBar::item:selected { background: #273038; border-radius: 1px; }
        QMenu { background: #151a1f; color: #d7dde2; border: 1px solid #343e46; padding: 3px; }
        QMenu::item { padding: 6px 26px 6px 10px; border-radius: 1px; }
        QMenu::item:selected { background: #2b4658; border-left: 2px solid #5da7d3; }
        QMenu::separator { height: 1px; background: #303941; margin: 4px 8px; }
        QToolBar#mainToolbar {
            background: #14191e; border: none; border-bottom: 1px solid #303840;
            spacing: 2px; padding: 5px 7px;
        }
        QToolBar#mainToolbar::separator {
            background: #60717d; width: 1px;
            margin: 5px 6px;
        }
        QToolButton {
            background: transparent; border: 1px solid transparent;
            border-radius: 1px; padding: 5px; color: #b8c2ca;
        }
        QToolButton:hover { background: #252d34; border-color: #3b4650; }
        QToolButton:checked { background: #263b49; border-color: #5da7d3; }
        QToolButton#styleChoice {
            background: #10161a; border: 1px solid #2e3a42;
            border-radius: 2px; color: #aeb9c1; padding: 5px;
        }
        QToolButton#styleChoice:hover {
            background: #19242b; border-color: #5b8299;
        }
        QToolButton#styleChoice:checked {
            background: #18392b; border: 2px solid #37d67a;
            color: #dff8e9;
        }
        QFrame#sidePanel { background: #11161a; border: none; }
        QFrame#canvasFrame { background: #000000; border: 1px solid #2a3239; }
        QDockWidget { background: #11161a; color: #d7dde2; }
        QDockWidget::title {
            background: #171d22; border-bottom: 1px solid #303941;
            color: #aeb8c0; padding: 7px 9px; text-align: left;
        }
        QLabel#panelTitle { color: #edf1f3; font-size: 12px; font-weight: 600; letter-spacing: 1px; }
        QLabel#documentTitle { color: #edf1f3; font-weight: 600; font-size: 13px; }
        QLabel#documentMeta { color: #7f8b94; font-size: 10px; }
        QLabel#hintLabel { color: #77838d; font-size: 11px; }
        QLabel#summaryLabel {
            background: #0d1114; border: 1px solid #2b343b; border-radius: 1px;
            padding: 9px; color: #98a4ad;
        }
        QTreeWidget, QTableWidget {
            background: #0c1013; alternate-background-color: #101519;
            border: 1px solid #2a333a; border-radius: 1px;
            color: #c5ced5; selection-background-color: #263f50;
            gridline-color: #20282e;
        }
        QTreeWidget::item, QTableWidget::item { padding: 5px; }
        QHeaderView::section {
            background: #171d22; color: #85929c; border: none;
            border-bottom: 1px solid #303941; padding: 6px;
        }
        QComboBox {
            background: #171d22; border: 1px solid #35404a; border-radius: 1px;
            padding: 5px 8px; color: #d5dce1;
        }
        QComboBox::drop-down { border: none; width: 20px; }
        QComboBox QAbstractItemView {
            background: #151a1f; color: #d7dde2; selection-background-color: #2b4658;
        }
        QPushButton {
            background: #1a2127; border: 1px solid #36414a; border-radius: 1px;
            color: #cbd3d9; padding: 6px 12px;
        }
        QPushButton:hover { background: #27343e; border-color: #5b8299; }
        QStatusBar {
            background: #ffffff; color: #263842;
            border-top: 1px solid #c5d0d7;
        }
        QStatusBar QLabel { padding: 2px 8px; }
        QCheckBox { color: #bdc6cd; spacing: 6px; }
        QSplitter::handle { background: #2b343b; }
        QScrollBar:vertical { background: #0d1114; width: 10px; }
        QScrollBar::handle:vertical { background: #35424b; border-radius: 0; min-height: 24px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background: #0d1114; height: 10px; }
        QScrollBar::handle:horizontal { background: #35424b; border-radius: 0; min-width: 24px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
        QProgressBar { background: #20282e; border: none; border-radius: 0; height: 4px; }
        QProgressBar::chunk { background: #5da7d3; }
        QTabBar::tab {
            background: #151b20; color: #85929b; border: 1px solid #303941;
            border-bottom: none; padding: 5px 18px; margin: 0 1px;
            border-top-left-radius: 1px; border-top-right-radius: 1px;
        }
        QTabBar::tab:selected {
            background: #273d4b; color: #eef3f5;
            border-color: #5a9cbf;
        }
        QTabBar::tab:disabled { color: #46525b; }
        QTabBar#documentTabs {
            background: transparent; border: none; spacing: 3px;
        }
        QTabBar#documentTabs::tab {
            background: #11191f; color: #8e9ca5;
            border: 1px solid #2d3b44; border-bottom: none;
            border-top-left-radius: 3px; border-top-right-radius: 3px;
            padding: 4px 30px 3px 13px; margin: 1px 2px 0 0;
            min-width: 120px;
        }
        QTabBar#documentTabs::tab:hover {
            background: #1b2932; color: #d8e1e5;
            border-color: #49616f;
        }
        QTabBar#documentTabs::tab:selected {
            background: #203846; color: #ffffff;
            border-top: 2px solid #62b4dd;
            border-left-color: #456879; border-right-color: #456879;
            padding-top: 3px;
        }
        QTabBar#documentTabs::close-button {
            image: url(:/icons/close-tab.svg);
            width: 12px; height: 12px;
            subcontrol-position: center right;
            margin-right: 6px;
        }
        QTabBar#documentTabs::close-button:hover {
            background: #a94442; border-radius: 7px;
        }
        QTabBar#layoutTabs {
            background: transparent; border: none;
            spacing: 3px;
        }
        QTabBar#layoutTabs::tab {
            background: #11191f; color: #82919a;
            border: 1px solid #2b3942; border-bottom: 2px solid transparent;
            border-radius: 0; padding: 3px 22px; margin-top: 2px;
        }
        QTabBar#layoutTabs::tab:hover {
            background: #1b2932; color: #d7e1e5;
        }
        QTabBar#layoutTabs::tab:selected {
            background: #1b303c; color: #f5f9fb;
            border-color: #456879; border-bottom: 2px solid #62b4dd;
        }
    )"));
    if (m_startScreen) {
        m_startScreen->setCadTheme(true);
    }
}

void MainWindow::setCadTheme(CadTheme theme, bool persist)
{
    if (m_cadTheme == theme) {
        return;
    }
    m_cadTheme = theme;
    if (persist) {
        QSettings settings = cacheSettings();
        settings.setValue(
            QStringLiteral("uiTheme"),
            theme == CadTheme::Light
                ? QStringLiteral("cadLight")
                : QStringLiteral("cadDark"));
        settings.sync();
    }
    applyStyle();
    for (const DocumentSession &session : std::as_const(m_documentSessions)) {
        if (session.view) {
            session.view->setDrawingTheme(
                theme == CadTheme::Light
                    ? CadView::DrawingTheme::CadLight
                    : CadView::DrawingTheme::CadDark);
        }
    }
    if (m_view) {
        m_view->setDrawingTheme(
            theme == CadTheme::Light
                ? CadView::DrawingTheme::CadLight
                : CadView::DrawingTheme::CadDark);
    }
    if (m_layerTree && m_view && m_view->document()) {
        populateLayers();
    }
    if (m_darkThemeAction && m_lightThemeAction) {
        (theme == CadTheme::Dark
             ? m_darkThemeAction : m_lightThemeAction)->setChecked(true);
    }
}

void MainWindow::openDrawing(const QString &path)
{
    if (m_loading || path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    const QString canonicalPath =
        info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    for (int i = 0; i < m_documentSessions.size(); ++i) {
        if (m_documentSessions[i].path == canonicalPath) {
            if (m_centralStack && m_canvasPage) {
                m_centralStack->setCurrentWidget(m_canvasPage);
            }
            m_documentTabs->setCurrentIndex(i + 1);
            return;
        }
    }
    if (m_centralStack && m_canvasPage) {
        m_centralStack->setCurrentWidget(m_canvasPage);
    }
    m_lastDirectory = info.absolutePath();
    QSettings settings = cacheSettings();
    settings.setValue(QStringLiteral("lastDirectory"), m_lastDirectory);
    m_loading = true;
    m_openAction->setEnabled(false);
    m_progress->show();
    statusBar()->showMessage(
        QStringLiteral("正在解析 %1 ...").arg(info.fileName()));
    m_titleLabel->setText(info.fileName());
    m_drawingMetaLabel->setText(QStringLiteral("正在读取图纸..."));
    m_loadWatcher->setFuture(QtConcurrent::run([path] {
        DrawingLoadResult result;
        auto document = std::make_shared<CadDocument>();
        QElapsedTimer stageTimer;
        stageTimer.start();
        QString error;
        if (document->load(path, &error)) {
            const qint64 parseMilliseconds = stageTimer.restart();
            result.layoutContents =
                CadView::prepareAllLayoutContents(*document);
            const qint64 layoutMilliseconds = stageTimer.restart();
            auto index = std::make_shared<SpatialIndex>();
            index->rebuild(*document);
            const qint64 indexMilliseconds = stageTimer.restart();
            result.modelIndex = std::move(index);
            result.thumbnail =
                renderDrawingThumbnail(*document, QSize(640, 360));
            if (qEnvironmentVariableIsSet("CADLENS_VERBOSE_LOAD")) {
                std::fprintf(
                    stderr,
                    "load-stage parse=%lldms layouts=%lldms index=%lldms "
                    "thumbnail=%lldms\n",
                    static_cast<long long>(parseMilliseconds),
                    static_cast<long long>(layoutMilliseconds),
                    static_cast<long long>(indexMilliseconds),
                    static_cast<long long>(stageTimer.elapsed()));
            }
            result.document = std::move(document);
        } else {
            result.error = error;
        }
        return result;
    }));
}

void MainWindow::finishDrawingLoad()
{
    m_loading = false;
    m_openAction->setEnabled(true);
    m_progress->hide();
    const DrawingLoadResult result = m_loadWatcher->result();
    if (!result.document) {
        showLoadError(result.error);
        return;
    }
    if (m_mainToolbar) {
        m_mainToolbar->setEnabled(true);
    }
    if (m_activeDocumentIndex >= 0) {
        auto *newView = new CadView;
        wireViewSignals(newView);
        m_documentStack->addWidget(newView);
        m_view = newView;
        m_view->setLineWeightsVisible(m_lineWeightAction->isChecked());
        m_view->setSnapEnabled(m_snapEnabledAction->isChecked());
        m_view->setGridVisible(m_gridAction->isChecked());
        m_view->setOrthoEnabled(m_orthoAction->isChecked());
    }
    m_view->setDocument(result.document, result.modelIndex,
                        result.layoutContents);
    DisplaySettings settings;
    settings.lengthUnit = result.document->sourceUnit;
    settings.areaUnit = MeasuringUnit::Meter;
    settings.coordinateUnit = result.document->sourceUnit;
    QVector<Measurement> measurements;
    QString annotationError;
    loadAnnotations(result.document->path, *result.document, &settings,
                    &measurements, &annotationError);
    restoreCalibrationRecord(*result.document);
    m_view->setMeasurements(measurements);
    if (m_annotationVisibilityAction
        && !m_annotationVisibilityAction->isChecked()) {
        m_view->hideAllMeasurements();
    }
    m_view->setDisplaySettings(settings);
    refreshUnitControls();
    populateLayers();
    populateLayoutTabs();
    refreshMeasurementTable();
    if (m_annotationVisibilityAction) {
        const bool anyVisible = std::any_of(
            m_view->measurements().cbegin(),
            m_view->measurements().cend(),
            [](const Measurement &measurement) {
                return measurement.visible;
            });
        const QSignalBlocker blocker(m_annotationVisibilityAction);
        m_annotationVisibilityAction->setChecked(anyVisible);
        const QString icon =
            anyVisible ? QStringLiteral("show-all")
                       : QStringLiteral("hide-all");
        m_annotationVisibilityAction->setProperty("cadIconName", icon);
        m_annotationVisibilityAction->setIcon(
            themedCadIcon(icon, m_cadTheme == CadTheme::Dark));
    }
    refreshStatus();
    m_dirty = false;
    setWindowTitle(
        QStringLiteral("%1 · 速看CAD").arg(result.document->fileName));
    m_titleLabel->setText(result.document->fileName);
    m_drawingMetaLabel->setText(
        result.document->isPdf()
            ? QStringLiteral("%1 · %2 页 · %3 ms")
                  .arg(result.document->sourceFormat,
                       QString::number(result.document->pdfPages.size()),
                       QString::number(result.document->loadMilliseconds))
            : QStringLiteral("%1 · %2 个图元 · %3 个图层 · %4 ms")
                  .arg(result.document->sourceFormat,
                       QString::number(result.document->entities.size()),
                       QString::number(result.document->layers.size()),
                       QString::number(result.document->loadMilliseconds)));
    statusBar()->showMessage(
        result.document->isPdf()
            ? QStringLiteral("PDF 已加载：%1 页，%2 ms")
                  .arg(result.document->pdfPages.size())
                  .arg(result.document->loadMilliseconds)
            : QStringLiteral("图纸已加载：%1 个图元，%2 ms")
                  .arg(result.document->entities.size())
                  .arg(result.document->loadMilliseconds),
        8000);
    if (!annotationError.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("标注文件读取失败：%1").arg(annotationError),
            8000);
    }
    const QFileInfo openedInfo(result.document->path);
    const QString sessionPath =
        openedInfo.canonicalFilePath().isEmpty()
        ? openedInfo.absoluteFilePath() : openedInfo.canonicalFilePath();
    const int sessionIndex = m_documentSessions.size();
    m_documentSessions.append(
        {m_view, sessionPath, openedInfo.fileName()});
    m_activeDocumentIndex = sessionIndex;
    {
        const QSignalBlocker blocker(m_documentTabs);
        m_documentTabs->addTab(openedInfo.fileName());
        m_documentTabs->setTabToolTip(sessionIndex + 1, sessionPath);
        m_documentTabs->setCurrentIndex(sessionIndex + 1);
    }
    m_documentStack->setCurrentWidget(m_view);
    saveRecentThumbnail(result.document->path, result.thumbnail);
    updateRecentDrawing(result.document->path);
    if (m_centralStack && m_canvasPage) {
        m_centralStack->setCurrentWidget(m_canvasPage);
    }
    m_view->setFocus();
    beginSelectAndFit();
}

void MainWindow::activateDocument(int index)
{
    if (index < 0 || index >= m_documentSessions.size()) {
        return;
    }
    if (index == m_activeDocumentIndex) {
        m_view = m_documentSessions[index].view;
        m_documentStack->setCurrentWidget(m_view);
        if (m_centralStack && m_canvasPage) {
            m_centralStack->setCurrentWidget(m_canvasPage);
        }
        if (m_mainToolbar) {
            m_mainToolbar->setEnabled(true);
        }
        refreshDocumentUi();
        m_view->setFocus();
        return;
    }
    if (m_annotationAutoSaveTimer) {
        m_annotationAutoSaveTimer->stop();
    }
    if (m_activeDocumentIndex >= 0 && m_view && m_view->document()) {
        autoSaveAnnotationsNow();
    }
    m_activeDocumentIndex = index;
    m_view = m_documentSessions[index].view;
    m_documentStack->setCurrentWidget(m_view);
    if (m_centralStack && m_canvasPage) {
        m_centralStack->setCurrentWidget(m_canvasPage);
    }
    if (m_mainToolbar) {
        m_mainToolbar->setEnabled(true);
    }
    m_currentSelection.clear();
    refreshDocumentUi();
    m_view->setFocus();
}

void MainWindow::closeDocumentTab(int index)
{
    if (index < 0 || index >= m_documentSessions.size()) {
        return;
    }
    const bool showingStart =
        m_centralStack && m_startScreen
        && m_centralStack->currentWidget() == m_startScreen;
    const DocumentSession closing = m_documentSessions[index];
    if (closing.view && closing.view->document()) {
        QString saveError;
        if (!saveAnnotationsAutosave(
                closing.path, *closing.view->document(),
                closing.view->displaySettings(),
                closing.view->measurements(), &saveError)) {
            statusBar()->showMessage(
                QStringLiteral("关闭前自动保存失败：%1").arg(saveError),
                7000);
        }
    }
    const bool wasActive = index == m_activeDocumentIndex;
    m_documentSessions.removeAt(index);
    {
        const QSignalBlocker blocker(m_documentTabs);
        m_documentTabs->removeTab(index + 1);
    }

    if (m_documentSessions.isEmpty()) {
        m_activeDocumentIndex = -1;
        m_view = closing.view;
        if (m_view) {
            m_view->cancelCurrentTool();
            m_view->setDocument(std::shared_ptr<CadDocument>());
            m_documentStack->setCurrentWidget(m_view);
        }
        m_annotationAutoSaveTimer->stop();
        m_layoutTabs->blockSignals(true);
        while (m_layoutTabs->count() > 0) {
            m_layoutTabs->removeTab(0);
        }
        m_layoutTabs->blockSignals(false);
        m_layerTree->clear();
        m_measurementTable->setRowCount(0);
        m_coordinateLabel->setText(QStringLiteral("X  —    Y  —"));
        m_scaleLabel->setText(QStringLiteral("比例 未校准"));
        m_zoomLabel->setText(QStringLiteral("缩放  100%"));
        m_summaryLabel->setText(QStringLiteral("尚未添加测量标注"));
        m_undoAction->setEnabled(false);
        m_redoAction->setEnabled(false);
        m_titleLabel->setText(QStringLiteral("速看CAD"));
        m_drawingMetaLabel->setText(
            QStringLiteral("DWG / DXF / PDF · 施工测量"));
        setWindowTitle(QStringLiteral("速看CAD · 工程施工看图"));
        {
            const QSignalBlocker blocker(m_documentTabs);
            m_documentTabs->setCurrentIndex(0);
        }
        if (m_mainToolbar) {
            m_mainToolbar->setEnabled(false);
        }
        if (m_centralStack && m_startScreen) {
            m_centralStack->setCurrentWidget(m_startScreen);
        }
        m_dirty = false;
        refreshStartScreen();
        return;
    }

    m_documentStack->removeWidget(closing.view);
    closing.view->deleteLater();
    int nextActive = m_activeDocumentIndex;
    if (wasActive) {
        nextActive = qMin(index, m_documentSessions.size() - 1);
    } else if (index < m_activeDocumentIndex) {
        --nextActive;
    }
    m_activeDocumentIndex = nextActive;
    m_view = m_documentSessions[nextActive].view;
    m_documentStack->setCurrentWidget(m_view);
    if (showingStart) {
        const QSignalBlocker blocker(m_documentTabs);
        m_documentTabs->setCurrentIndex(0);
        if (m_mainToolbar) {
            m_mainToolbar->setEnabled(false);
        }
        return;
    }
    {
        const QSignalBlocker blocker(m_documentTabs);
        m_documentTabs->setCurrentIndex(nextActive + 1);
    }
    refreshDocumentUi();
    m_view->setFocus();
}

void MainWindow::populateLayoutTabs()
{
    m_layoutTabs->blockSignals(true);
    while (m_layoutTabs->count() > 0) {
        m_layoutTabs->removeTab(0);
    }
    if (!m_view || !m_view->document()) {
        m_layoutTabs->blockSignals(false);
        return;
    }
    const CadDocument &document = *m_view->document();
    if (document.isPdf()) {
        for (const CadPdfPage &page : document.pdfPages) {
            const int index = m_layoutTabs->addTab(page.name);
            m_layoutTabs->setTabToolTip(
                index, QStringLiteral("PDF 页面 · %1 × %2 pt")
                           .arg(page.pageRect.width(), 0, 'f', 1)
                           .arg(page.pageRect.height(), 0, 'f', 1));
            m_layoutTabs->setTabEnabled(index, true);
        }
        int selectedPage = 0;
        for (int i = 0; i < m_layoutTabs->count(); ++i) {
            if (m_layoutTabs->tabText(i) == m_view->activeSpace()) {
                selectedPage = i;
                break;
            }
        }
        m_layoutTabs->setCurrentIndex(selectedPage);
        m_layoutTabs->blockSignals(false);
        return;
    }
    m_layoutTabs->addTab(QStringLiteral("模型"));
    for (const QString &layout : document.layouts) {
        const int index = m_layoutTabs->addTab(layout);
        const int entityCount =
            document.layoutEntities.value(layout).size();
        int modelViewportCount = 0;
        for (const CadViewportDefinition &viewport :
             document.layoutViewports.value(layout)) {
            modelViewportCount += viewport.revealsModel ? 1 : 0;
        }
        m_layoutTabs->setTabToolTip(
            index, QStringLiteral("%1 个图元 · %2 个模型视口")
                       .arg(entityCount)
                       .arg(modelViewportCount));
        m_layoutTabs->setTabEnabled(
            index, entityCount > 0 || modelViewportCount > 0);
    }
    if (document.layouts.isEmpty()) {
        const int index = m_layoutTabs->addTab(QStringLiteral("布局1"));
        m_layoutTabs->setTabEnabled(index, false);
        m_layoutTabs->setTabToolTip(
            index, QStringLiteral("此图纸未包含可切换的布局对象"));
    }
    int selected = 0;
    for (int i = 0; i < m_layoutTabs->count(); ++i) {
        if (m_layoutTabs->tabText(i) == m_view->activeSpace()) {
            selected = i;
            break;
        }
    }
    m_layoutTabs->setCurrentIndex(selected);
    m_layoutTabs->blockSignals(false);
}

void MainWindow::refreshDocumentUi()
{
    if (!m_view || !m_view->document()) {
        return;
    }
    const QString path = m_documentSessions[m_activeDocumentIndex].path;
    const QString title = m_documentSessions[m_activeDocumentIndex].title;
    populateLayers();
    refreshUnitControls();
    populateLayoutTabs();
    refreshMeasurementTable();
    refreshStatus();
    m_dirty = false;
    setWindowTitle(QStringLiteral("%1 · 速看CAD").arg(title));
    m_titleLabel->setText(title);
    m_drawingMetaLabel->setText(
        QStringLiteral("%1 · %2 个图层 · %3")
            .arg(m_view->document()->sourceFormat)
            .arg(m_view->document()->layers.size())
            .arg(QFileInfo(path).absolutePath()));
}

void MainWindow::showLoadError(const QString &message)
{
    m_titleLabel->setText(QStringLiteral("速看CAD"));
    m_drawingMetaLabel->setText(
        QStringLiteral("DWG / DXF / PDF · 施工测量"));
    QMessageBox::warning(
        this, QStringLiteral("无法打开图纸"),
        message.isEmpty() ? QStringLiteral("图纸解析失败。") : message);
    statusBar()->showMessage(QStringLiteral("图纸解析失败"), 5000);
    if (m_centralStack && m_startScreen) {
        m_centralStack->setCurrentWidget(m_startScreen);
    }
    if (m_mainToolbar) {
        m_mainToolbar->setEnabled(false);
    }
}

void MainWindow::beginSelectAndFit()
{
    m_selectAction->trigger();
}

void MainWindow::populateLayers()
{
    m_layerTree->blockSignals(true);
    m_layerTree->clear();
    if (m_view->document()) {
        for (const CadLayer &layer : m_view->document()->layers) {
            const QColor layerColor =
                m_view->displayColor(layer.color);
            auto *item = new QTreeWidgetItem(m_layerTree);
            item->setText(
                0, QStringLiteral("●  %1  %2%3")
                       .arg(layer.name,
                            layerColor.name().toUpper(),
                            layer.locked ? QStringLiteral("  [锁定]")
                                         : QString()));
            item->setForeground(0, QBrush(layerColor));
            item->setData(0, Qt::UserRole, layer.name);
            item->setCheckState(0,
                                layer.visible ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(
                0, QStringLiteral("%1 · %2 个图元 · 颜色 %3")
                       .arg(layer.name)
                       .arg(layer.entityCount)
                       .arg(layerColor.name()));
            item->setTextAlignment(0, Qt::AlignVCenter);
        }
    }
    m_layerTree->blockSignals(false);
}

void MainWindow::updateLayerVisibility(QTreeWidgetItem *item, int column)
{
    if (!item || column != 0 || !m_view->document()) {
        return;
    }
    CadLayer *layer =
        m_view->document()->layer(item->data(0, Qt::UserRole).toString());
    if (layer) {
        layer->visible = item->checkState(0) == Qt::Checked;
        m_view->invalidateDrawingCache();
    }
}

void MainWindow::refreshMeasurementTable()
{
    if (!m_view->document()) {
        return;
    }
    m_refreshingTable = true;
    m_measurementTable->setRowCount(m_view->measurements().size());
    for (int row = 0; row < m_view->measurements().size(); ++row) {
        const Measurement &measurement = m_view->measurements()[row];
        auto *visible = new QTableWidgetItem;
        visible->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled
                          | Qt::ItemIsSelectable);
        visible->setCheckState(measurement.visible ? Qt::Checked
                                                    : Qt::Unchecked);
        visible->setData(Qt::UserRole, measurement.id);
        auto *type = new QTableWidgetItem(measurement.typeLabel());
        type->setFlags(type->flags() & ~Qt::ItemIsEditable);
        type->setData(Qt::UserRole, measurement.id);
        auto *value = new QTableWidgetItem(measurement.primaryValue(
            *m_view->document(), m_view->displaySettings()));
        value->setFlags(value->flags() & ~Qt::ItemIsEditable);
        auto *note = new QTableWidgetItem(measurement.note);
        note->setData(Qt::UserRole, measurement.id);
        m_measurementTable->setItem(row, 0, visible);
        m_measurementTable->setItem(row, 1, type);
        m_measurementTable->setItem(row, 2, value);
        m_measurementTable->setItem(row, 3, note);
        if (measurement.id == m_currentSelection) {
            m_measurementTable->selectRow(row);
        }
    }
    m_refreshingTable = false;
    m_summaryLabel->setText(measurementsSummary());
}

void MainWindow::refreshUnitControls()
{
    if (!m_view->document()) {
        return;
    }
    const int unitIndex =
        m_unitCombo->findData(int(m_view->displayUnit()));
    if (unitIndex >= 0) {
        const QSignalBlocker blocker(m_unitCombo);
        m_unitCombo->setCurrentIndex(unitIndex);
    }
    const int precisionIndex =
        m_precisionCombo->findData(
            m_view->displaySettings().lengthPrecision);
    if (precisionIndex >= 0) {
        const QSignalBlocker blocker(m_precisionCombo);
        m_precisionCombo->setCurrentIndex(precisionIndex);
    }
    refreshStatus();
}

void MainWindow::refreshStatus()
{
    if (!m_view->document()) {
        return;
    }
    const CadDocument &document = *m_view->document();
    const double sourceUnitMeters =
        qMax(1.0e-15, measuringUnitMeters(document.sourceUnit));
    const double ratio =
        document.metersPerDrawingUnit / sourceUnitMeters;
    m_scaleLabel->setText(
        QStringLiteral("比例  1:%1").arg(ratio, 0, 'g', 8));
    m_summaryLabel->setText(measurementsSummary());
    m_undoAction->setEnabled(m_view->canUndo());
    m_redoAction->setEnabled(m_view->canRedo());
}

QString MainWindow::measurementsSummary() const
{
    if (!m_view->document() || m_view->measurements().isEmpty()) {
        return QStringLiteral("尚未添加测量标注");
    }
    int visible = 0;
    for (const Measurement &measurement : m_view->measurements()) {
        visible += measurement.visible ? 1 : 0;
    }
    return QStringLiteral("共 %1 条标注 · 当前显示 %2 条\n"
                          "双击列表项可定位，拖动图纸上的标注文字可调整位置。")
        .arg(m_view->measurements().size())
        .arg(visible);
}

void MainWindow::updateSnapModesFromMenu()
{
    SnapModes modes;
    for (QAction *action : std::as_const(m_snapActions)) {
        const SnapKind kind = SnapKind(action->data().toInt());
        switch (kind) {
        case SnapKind::Endpoint:
            modes.endpoint = action->isChecked();
            break;
        case SnapKind::Midpoint:
            modes.midpoint = action->isChecked();
            break;
        case SnapKind::Intersection:
            modes.intersection = action->isChecked();
            break;
        case SnapKind::Perpendicular:
            modes.perpendicular = action->isChecked();
            break;
        case SnapKind::Center:
            modes.center = action->isChecked();
            break;
        case SnapKind::Nearest:
            modes.nearest = action->isChecked();
            break;
        case SnapKind::Quadrant:
        case SnapKind::None:
            break;
        }
    }
    m_view->setSnapModes(modes);
}

void MainWindow::requestCalibration(double rawLength)
{
    const DisplaySettings settings = m_view->displaySettings();
    const double currentValue = rawLength
        * m_view->document()->metersPerDrawingUnit
        / measuringUnitMeters(settings.lengthUnit);
    bool accepted = false;
    const double known = QInputDialog::getDouble(
        this, QStringLiteral("校准图纸比例"),
        QStringLiteral(
            "原图纸测量值：%1\n"
            "请输入图纸标注的已知长度（%2），修改后将按该长度反算比例：")
            .arg(formatMeasuredLength(rawLength, *m_view->document(),
                                      settings.lengthUnit,
                                      settings.lengthPrecision),
                 measuringUnitLabel(settings.lengthUnit)),
        currentValue, 0.0, 1.0e15, 6, &accepted);
    if (!accepted) {
        m_view->cancelCurrentTool();
        m_view->beginTool(CadView::ToolMode::Select);
        return;
    }
    m_view->applyCalibration(known, settings.lengthUnit);
    // Calibration is drawing-specific and must survive the next open even
    // when the user has not created any measurement annotations yet.
    rememberCalibrationRecord(*m_view->document());
    saveAnnotationsNow();
    refreshStatus();
}

void MainWindow::requestTextAnnotation(const QPointF &position)
{
    bool accepted = false;
    const QString text = QInputDialog::getMultiLineText(
        this, QStringLiteral("文字注释"),
        QStringLiteral("输入要标注在图纸上的文字："), QString(), &accepted);
    if (accepted && !text.trimmed().isEmpty()) {
        m_view->addTextAnnotation(position, text);
    }
}

void MainWindow::showAnnotationSettings()
{
    if (!m_view->document()) {
        return;
    }
    DisplaySettings settings = m_view->displaySettings();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("标注设置"));
    dialog.setObjectName(QStringLiteral("annotationSettingsDialog"));
    dialog.setStyleSheet(QStringLiteral(R"(
        QDialog#annotationSettingsDialog {
            background: #f4f6f8; color: #17232b;
        }
        QDialog#annotationSettingsDialog QLabel {
            color: #24343e;
        }
        QDialog#annotationSettingsDialog QLabel#panelTitle {
            color: #102f40; font-size: 14px; font-weight: 700;
        }
        QDialog#annotationSettingsDialog QLabel#hintLabel {
            color: #52646f; background: #e9eef2;
            border: 1px solid #d6dee3; padding: 8px;
        }
        QDialog#annotationSettingsDialog QComboBox,
        QDialog#annotationSettingsDialog QSpinBox {
            background: #ffffff; color: #17232b;
            border: 1px solid #b8c5cd; border-radius: 2px;
            padding: 4px 7px; min-height: 24px;
        }
        QDialog#annotationSettingsDialog QComboBox QAbstractItemView {
            background: #ffffff; color: #17232b;
            selection-background-color: #dceffd;
            selection-color: #102d3d;
        }
        QDialog#annotationSettingsDialog QPushButton {
            background: #ffffff; color: #1d2b34;
            border: 1px solid #aebbc4; border-radius: 2px;
            padding: 6px 12px;
        }
        QDialog#annotationSettingsDialog QPushButton:hover {
            background: #eef6fb; border-color: #5a9fc4;
        }
        QDialog#annotationSettingsDialog QToolButton#styleChoice {
            background: #ffffff; border: 1px solid #b8c5cd;
            border-radius: 2px; color: #2e414c; padding: 5px;
        }
        QDialog#annotationSettingsDialog QToolButton#styleChoice:hover {
            background: #eef6fb; border-color: #5a9fc4;
        }
        QDialog#annotationSettingsDialog QToolButton#styleChoice:checked {
            background: #e4f7ec; border: 2px solid #1b9e57;
            color: #145d36;
        }
    )"));
    dialog.setMinimumWidth(620);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    const auto makeUnitCombo = [](MeasuringUnit selected, bool area) {
        auto *combo = new QComboBox;
        const MeasuringUnit units[] = {
            MeasuringUnit::Millimeter, MeasuringUnit::Centimeter,
            MeasuringUnit::Meter, MeasuringUnit::Kilometer,
            MeasuringUnit::Inch, MeasuringUnit::Foot
        };
        for (MeasuringUnit unit : units) {
            QString label = measuringUnitLabel(unit);
            if (area) {
                switch (unit) {
                case MeasuringUnit::Millimeter:
                    label = QStringLiteral("平方毫米 (mm²)");
                    break;
                case MeasuringUnit::Centimeter:
                    label = QStringLiteral("平方厘米 (cm²)");
                    break;
                case MeasuringUnit::Meter:
                    label = QStringLiteral("平方米 (m²)");
                    break;
                case MeasuringUnit::Kilometer:
                    label = QStringLiteral("平方千米 (km²)");
                    break;
                case MeasuringUnit::Inch:
                    label = QStringLiteral("平方英寸 (in²)");
                    break;
                case MeasuringUnit::Foot:
                    label = QStringLiteral("平方英尺 (ft²)");
                    break;
                case MeasuringUnit::None:
                    break;
                }
            }
            combo->addItem(label, int(unit));
        }
        combo->setCurrentIndex(combo->findData(int(selected)));
        return combo;
    };
    auto *lengthUnit = makeUnitCombo(settings.lengthUnit, false);
    auto *areaUnit = makeUnitCombo(settings.areaUnit, true);
    auto *coordinateUnit = makeUnitCombo(settings.coordinateUnit, false);
    const auto makePrecision = [](int value) {
        auto *spin = new QSpinBox;
        spin->setRange(0, 6);
        spin->setValue(value);
        spin->setSuffix(QStringLiteral(" 位"));
        return spin;
    };
    auto *lengthPrecision = makePrecision(settings.lengthPrecision);
    auto *areaPrecision = makePrecision(settings.areaPrecision);
    auto *coordinatePrecision = makePrecision(settings.coordinatePrecision);
    auto *styleTitle = new QLabel(QStringLiteral("CAD 标注样式"));
    styleTitle->setObjectName(QStringLiteral("panelTitle"));
    auto *stylePanel = new QWidget;
    auto *styleGrid = new QGridLayout(stylePanel);
    styleGrid->setContentsMargins(0, 0, 0, 0);
    styleGrid->setHorizontalSpacing(8);
    styleGrid->setVerticalSpacing(8);
    auto *styleGroup = new QButtonGroup(&dialog);
    styleGroup->setExclusive(true);
    const QStringList styleLabels = annotationStyleLabels();
    const QStringList styleIcons = {
        QStringLiteral("dim-standard"),
        QStringLiteral("dim-architectural"),
        QStringLiteral("dim-mechanical"),
        QStringLiteral("dim-engineering"),
        QStringLiteral("dim-decimal"),
        QStringLiteral("dim-compact")
    };
    for (int i = 0; i < styleLabels.size(); ++i) {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("styleChoice"));
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIcon(
            QIcon(QStringLiteral(":/icons/%1.svg").arg(styleIcons[i])));
        button->setIconSize(QSize(106, 48));
        button->setMinimumSize(122, 82);
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("baseLabel", styleLabels[i]);
        button->setText(styleLabels[i]);
        styleGroup->addButton(button, i);
        connect(button, &QToolButton::toggled, button,
                [button](bool checked) {
                    const QString label =
                        button->property("baseLabel").toString();
                    button->setText(
                        checked ? QStringLiteral("✓  ") + label : label);
                });
        styleGrid->addWidget(button, i / 3, i % 3);
    }
    const int selectedStyle = qBound(
        0, int(settings.annotationStyle), styleLabels.size() - 1);
    styleGroup->button(selectedStyle)->setChecked(true);
    auto *colorButton = new QPushButton(
        QStringLiteral("选择标注颜色"));
    QColor selectedColor = settings.annotationColor;
    const auto updateColorButton = [colorButton, &selectedColor] {
        colorButton->setStyleSheet(
            QStringLiteral("background:%1; color:%2;")
                .arg(selectedColor.name(),
                     selectedColor.lightness() > 150
                         ? QStringLiteral("#101820")
                         : QStringLiteral("#f5f8fa")));
        colorButton->setText(selectedColor.name().toUpper());
    };
    updateColorButton();
    connect(colorButton, &QPushButton::clicked, &dialog,
            [this, &selectedColor, &updateColorButton] {
                const QColor picked = QColorDialog::getColor(
                    selectedColor, this, QStringLiteral("标注颜色"));
                if (picked.isValid()) {
                    selectedColor = picked;
                    updateColorButton();
                }
            });
    auto *lengthRow = new QWidget;
    auto *lengthRowLayout = new QHBoxLayout(lengthRow);
    lengthRowLayout->setContentsMargins(0, 0, 0, 0);
    lengthRowLayout->addWidget(lengthUnit, 1);
    lengthRowLayout->addWidget(lengthPrecision);
    auto *areaRow = new QWidget;
    auto *areaRowLayout = new QHBoxLayout(areaRow);
    areaRowLayout->setContentsMargins(0, 0, 0, 0);
    areaRowLayout->addWidget(areaUnit, 1);
    areaRowLayout->addWidget(areaPrecision);
    auto *coordinateRow = new QWidget;
    auto *coordinateRowLayout = new QHBoxLayout(coordinateRow);
    coordinateRowLayout->setContentsMargins(0, 0, 0, 0);
    coordinateRowLayout->addWidget(coordinateUnit, 1);
    coordinateRowLayout->addWidget(coordinatePrecision);
    form->addRow(QStringLiteral("长度单位 / 精度"), lengthRow);
    form->addRow(QStringLiteral("面积单位 / 精度"), areaRow);
    form->addRow(QStringLiteral("坐标单位 / 精度"), coordinateRow);
    form->addRow(QStringLiteral("标注颜色设置"), colorButton);
    layout->addWidget(styleTitle);
    layout->addWidget(stylePanel);
    layout->addLayout(form);
    auto *hint = new QLabel(
        QStringLiteral(
            "默认颜色为绿色。六种样式分别使用实心箭头、建筑斜线、"
            "窄箭头、开口箭头、圆点端部和端线标记；尺寸文字高度"
            "参考图纸现有尺寸并随图纸缩放。"));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("hintLabel"));
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog,
            &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    settings.lengthUnit =
        static_cast<MeasuringUnit>(lengthUnit->currentData().toInt());
    settings.areaUnit =
        static_cast<MeasuringUnit>(areaUnit->currentData().toInt());
    settings.coordinateUnit =
        static_cast<MeasuringUnit>(coordinateUnit->currentData().toInt());
    settings.annotationStyle =
        static_cast<AnnotationStyle>(styleGroup->checkedId());
    settings.lengthPrecision = lengthPrecision->value();
    settings.areaPrecision = areaPrecision->value();
    settings.coordinatePrecision = coordinatePrecision->value();
    settings.annotationColor = selectedColor;
    m_view->setDisplaySettings(settings);
    refreshUnitControls();
    refreshMeasurementTable();
    setDirty(true);
}

void MainWindow::showHelp()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("速看CAD 使用帮助"));
    dialog.resize(820, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *browser = new QTextBrowser;
    browser->setOpenExternalLinks(true);
    browser->setHtml(QStringLiteral(R"(
        <html><head><style>
        body { color:#17232b; font-family:"Noto Sans CJK SC","PingFang SC"; font-size:14px; line-height:1.65; }
        h2 { color:#102f40; margin-top:22px; margin-bottom:8px; }
        h3 { color:#174d67; margin-top:18px; margin-bottom:6px; }
        li { margin:4px 0; }
        code { color:#155e75; background:#eef4f7; padding:1px 4px; }
        .tip { background:#eef7f1; border-left:4px solid #2ba76b; padding:9px 12px; }
        </style></head><body>
        <h2>速看CAD 工程施工看图</h2>
        <p>本软件直接读取 DWG/DXF 的模型空间与布局及 PDF 页面，测量、标注和比例数据均保存在系统缓存中。</p>
        <h3>打开与视图</h3>
        <ul>
          <li>启动页可打开图纸，每行显示 4 个最近图纸缩略图，最多保留 20 条。</li>
          <li>左键拖动图纸平移；滚轮以鼠标位置为中心缩放；双击中键显示全图。</li>
          <li><code>Z</code> 或“窗口缩放”：依次单击区域的两个对角点后放大。</li>
          <li><code>F3</code> 对象捕捉、<code>F7</code> 栅格、<code>F8</code> 正交。</li>
        </ul>
        <h3>测量工具</h3>
        <ul>
          <li>对齐、线性、连续、批量、矩形、多边形、圆、角度、弧长、坐标、点到直线和定距测量。</li>
          <li>连续/批量/多边形可在最后一点按 <code>Esc</code>、右键、Enter 或双击结束，再单击放置尺寸文字。</li>
          <li>连续测量逐段在线段上显示独立长度，总长单独标注；批量尺寸按鼠标方向布置。</li>
          <li>连续、面积和半径尺寸线绘制在对应边线或半径线上，文字覆盖部分自动断开；长度不足时自动使用引线，圆不标注圆心位置。</li>
          <li>点到直线：先选点，再单击已有直线；系统自动求垂足，在点与垂足间绘制尺寸线。</li>
          <li>矩形和多边形只绘制边界和尺寸，不填充测量区域。</li>
          <li>圆测量在圆周或圆弧上取三个不重复点，显示半径、面积和周长，不标注圆心位置。</li>
        </ul>
        <h3>对象捕捉</h3>
        <p>支持端点、中点、交点、垂足、圆心、象限点和最近点。测量十字线贯穿绘图区，中心小方框为拾取范围提示。</p>
        <h3>单位、比例和标注样式</h3>
        <ul>
          <li>在“标注设置：样式、颜色、单位与精度”中选择长度、面积、坐标单位与精度。</li>
          <li>ISO 标准、建筑斜线、机械 ISO-25、工程开口箭头、小数圆点、紧凑端线六种样式可勾选切换。</li>
          <li>尺寸文字高度参考图纸现有尺寸标注，并随图纸放大缩小同步变化。</li>
          <li>比例校准后自动保存到系统缓存目录。</li>
          <li>默认标注颜色为绿色，可单独修改。</li>
        </ul>
        <h3>自动保存与打印</h3>
        <ul>
          <li>测量修改后自动写入系统缓存目录，再次打开自动恢复。</li>
          <li>“框选打印与预览”先两点框选范围，再在预览右侧直接选择打印机、纸张、方向和份数。</li>
          <li>PDF 输出可选择 CAD 原色或全黑模式；全黑模式保持白色背景，图纸、文字、尺寸和填充均输出为黑色。</li>
          <li>打印到 PDF 使用 1200 DPI，并对 PDF 页面重新进行高分辨率栅格化。</li>
          <li>纸张或方向改变后预览立即更新；打印标尺可随时显示或关闭。</li>
          <li>打印设置提供“打印到 PDF”；PDF 默认文件名与当前图纸名一致。</li>
        </ul>
        <h3>常用快捷键</h3>
        <ul>
          <li><code>V</code> 选择/拖动标注，<code>A</code> 对齐尺寸，<code>L</code> 线性尺寸，<code>C</code> 连续测量，<code>P</code> 多边形。</li>
          <li><code>T</code> 文字注释，<code>R</code> 比例校准，<code>0</code> 显示全图。</li>
          <li><code>Delete</code> 删除所选标注，<code>Esc</code> 取消或结束当前路径。</li>
        </ul>
        <p class="tip">图纸仅在本地读取和处理，不会上传。按 F1 可随时重新打开本帮助。</p>
        </body></html>
    )"));
    layout->addWidget(browser, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog,
            &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showAbout()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("关于速看CAD"));
    dialog.setMinimumWidth(680);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 18);
    layout->setSpacing(16);

    auto *content = new QHBoxLayout;
    content->setSpacing(24);
    auto *info = new QVBoxLayout;
    info->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("速看CAD"));
    title->setObjectName(QStringLiteral("panelTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(24.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *version = new QLabel(
        QStringLiteral("版本 %1")
            .arg(QCoreApplication::applicationVersion()));
    version->setObjectName(QStringLiteral("documentMeta"));
    auto *description = new QLabel(
        QStringLiteral(
            "速看CAD 是面向工程施工人员的本地图纸查看、测量与标注工具。"
            "可直接打开 DWG、DXF 和 PDF 图纸，查看模型与布局，"
            "完成尺寸、面积、角度等工程测量，并保存矢量化标注。"
            "所有图纸均在本机处理，不上传文件。"));
    description->setWordWrap(true);
    description->setMinimumWidth(360);
    auto *author = new QLabel(
        QStringLiteral("开发者：Mr. 王嘉"));
    auto *wechat = new QLabel(
        QStringLiteral("微信：SerendipityWangJia"));
    author->setTextInteractionFlags(Qt::TextSelectableByMouse);
    wechat->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info->addWidget(title);
    info->addWidget(version);
    info->addSpacing(4);
    info->addWidget(description);
    info->addSpacing(8);
    info->addWidget(author);
    info->addWidget(wechat);
    info->addStretch();
    content->addLayout(info, 1);

    auto *donationPanel = new QFrame;
    donationPanel->setObjectName(QStringLiteral("sidePanel"));
    auto *donationLayout = new QVBoxLayout(donationPanel);
    donationLayout->setContentsMargins(14, 14, 14, 14);
    donationLayout->setSpacing(8);
    auto *donationTitle = new QLabel(QStringLiteral("微信打赏"));
    donationTitle->setAlignment(Qt::AlignCenter);
    donationTitle->setObjectName(QStringLiteral("panelTitle"));
    auto *qr = new QLabel;
    qr->setAlignment(Qt::AlignCenter);
    qr->setFixedSize(210, 210);
    QPixmap donationQr(
        QStringLiteral(":/images/wechat-donation.png"));
    if (!donationQr.isNull()) {
        qr->setPixmap(donationQr.scaled(
            200, 200, Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
        qr->setStyleSheet(
            QStringLiteral("background:white;border:1px solid #d6dde1;"));
    } else {
        qr->setText(QStringLiteral("微信收款码\n待提供"));
        qr->setStyleSheet(
            QStringLiteral(
                "background:#f7f8f8;color:#70808a;"
                "border:1px dashed #b9c3c9;"));
    }
    auto *hint = new QLabel(
        QStringLiteral("感谢支持持续维护"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setObjectName(QStringLiteral("hintLabel"));
    donationLayout->addWidget(donationTitle);
    donationLayout->addWidget(qr, 0, Qt::AlignCenter);
    donationLayout->addWidget(hint);
    content->addWidget(donationPanel);
    layout->addLayout(content);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected,
            &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::beginFixedDistance()
{
    if (!m_view->document()) {
        return;
    }
    const DisplaySettings settings = m_view->displaySettings();
    bool accepted = false;
    const double value = QInputDialog::getDouble(
        this, QStringLiteral("定距测量"),
        QStringLiteral("输入固定距离（%1）：")
            .arg(measuringUnitLabel(settings.lengthUnit)),
        1.0, 0.0, 1.0e15, settings.lengthPrecision, &accepted);
    if (!accepted) {
        m_view->beginTool(CadView::ToolMode::Select);
        return;
    }
    const double raw = value * measuringUnitMeters(settings.lengthUnit)
        / m_view->document()->metersPerDrawingUnit;
    m_view->beginTool(CadView::ToolMode::FixedDistance, raw);
}

void MainWindow::beginAreaOffset()
{
    if (!m_view->document()) {
        return;
    }
    const DisplaySettings settings = m_view->displaySettings();
    bool accepted = false;
    const double value = QInputDialog::getDouble(
        this, QStringLiteral("面积偏移"),
        QStringLiteral(
            "输入边界偏移距离（%1，正值向外扩展，负值向内收缩）：")
            .arg(measuringUnitSuffix(settings.lengthUnit)),
        0.0, -1.0e15, 1.0e15, settings.lengthPrecision, &accepted);
    if (!accepted) {
        m_view->beginTool(CadView::ToolMode::Select);
        return;
    }
    const double raw = value * measuringUnitMeters(settings.lengthUnit)
        / m_view->document()->metersPerDrawingUnit;
    m_view->beginTool(CadView::ToolMode::AreaOffset, raw);
}

void MainWindow::beginSideArea()
{
    if (!m_view->document()) {
        return;
    }
    const DisplaySettings settings = m_view->displaySettings();
    bool accepted = false;
    const double value = QInputDialog::getDouble(
        this, QStringLiteral("计算侧面积"),
        QStringLiteral("输入侧面高度（%1）：")
            .arg(measuringUnitLabel(settings.lengthUnit)),
        1.0, 0.0, 1.0e15, settings.lengthPrecision, &accepted);
    if (!accepted) {
        m_view->beginTool(CadView::ToolMode::Select);
        return;
    }
    const double raw = value * measuringUnitMeters(settings.lengthUnit)
        / m_view->document()->metersPerDrawingUnit;
    m_view->beginTool(CadView::ToolMode::SideArea, raw);
}

void MainWindow::requestPrint()
{
    if (!m_view->document()) {
        return;
    }
    m_view->beginTool(CadView::ToolMode::PrintArea);
}

void MainWindow::handlePrintArea(const QRectF &worldArea)
{
    if (!m_view->document() || !worldArea.isValid()) {
        m_view->beginTool(CadView::ToolMode::Select);
        return;
    }
    m_lastPrintArea = worldArea;
    showPrintPreview(worldArea);
    m_view->beginTool(CadView::ToolMode::Select);
}

void MainWindow::showPrintPreview(const QRectF &worldArea)
{
    if (!m_view->document()) {
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(
        QStringLiteral("打印预览 · %1")
            .arg(m_view->document()->fileName));
    dialog.resize(1380, 860);
    dialog.setMinimumSize(1050, 680);
    auto *mainLayout = new QHBoxLayout(&dialog);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(12);

    auto *previewCanvas = new PrintPreviewCanvas;
    previewCanvas->setObjectName(QStringLiteral("printPreview"));
    mainLayout->addWidget(previewCanvas, 1);

    QFrame *settingsPanel = panelFrame();
    settingsPanel->setMinimumWidth(320);
    settingsPanel->setMaximumWidth(360);
    auto *settingsLayout = new QVBoxLayout(settingsPanel);
    settingsLayout->setContentsMargins(16, 16, 16, 14);
    settingsLayout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("打印设置"));
    title->setObjectName(QStringLiteral("panelTitle"));
    settingsLayout->addWidget(title);
    auto *documentTitle = new QLabel(
        m_view->document()->fileName);
    documentTitle->setWordWrap(true);
    documentTitle->setObjectName(QStringLiteral("documentMeta"));
    settingsLayout->addWidget(documentTitle);

    auto *printerCombo = new QComboBox;
    auto *refreshPrinterButton =
        new QPushButton(QStringLiteral("刷新"));
    auto *printerRow = new QHBoxLayout;
    printerRow->setContentsMargins(0, 0, 0, 0);
    printerRow->addWidget(printerCombo, 1);
    printerRow->addWidget(refreshPrinterButton);
    settingsLayout->addWidget(new QLabel(QStringLiteral("打印机")));
    settingsLayout->addLayout(printerRow);

    auto *paperCombo = new QComboBox;
    const QList<QPageSize::PageSizeId> pageSizes = {
        QPageSize::A4, QPageSize::A3, QPageSize::A2,
        QPageSize::A1, QPageSize::A0,
        QPageSize::Letter, QPageSize::Legal
    };
    for (QPageSize::PageSizeId pageSize : pageSizes) {
        paperCombo->addItem(
            QPageSize(pageSize).name(), int(pageSize));
    }
    auto *orientationCombo = new QComboBox;
    orientationCombo->addItem(
        QStringLiteral("横向"), int(QPageLayout::Landscape));
    orientationCombo->addItem(
        QStringLiteral("纵向"), int(QPageLayout::Portrait));
    orientationCombo->setCurrentIndex(
        worldArea.width() >= worldArea.height() ? 0 : 1);
    auto *colorModeCombo = new QComboBox;
    colorModeCombo->addItem(
        QStringLiteral("黑白（所有 CAD 元素为黑色）"), true);
    colorModeCombo->addItem(
        QStringLiteral("彩色（使用 CAD 原色）"), false);
    auto *copies = new QSpinBox;
    copies->setRange(1, 99);
    copies->setValue(1);
    auto *ruler = new QCheckBox(QStringLiteral("显示打印标尺"));
    ruler->setChecked(true);
    auto *systemSettingsButton =
        new QPushButton(QStringLiteral("打开系统打印机设置"));
    auto *printerStatus = new QLabel;
    printerStatus->setObjectName(QStringLiteral("hintLabel"));
    printerStatus->setWordWrap(true);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow(QStringLiteral("纸张"), paperCombo);
    form->addRow(QStringLiteral("方向"), orientationCombo);
    form->addRow(QStringLiteral("输出颜色"), colorModeCombo);
    form->addRow(QStringLiteral("份数"), copies);
    form->addRow(QString(), ruler);
    settingsLayout->addLayout(form);
    settingsLayout->addWidget(printerStatus);
    settingsLayout->addWidget(systemSettingsButton);
    settingsLayout->addStretch();

    auto *rangeInfo = new QLabel(
        QStringLiteral("打印范围：当前框选区域\n"
                       "预览将按所选纸张、方向和打印边距重新生成。"));
    rangeInfo->setWordWrap(true);
    rangeInfo->setObjectName(QStringLiteral("hintLabel"));
    settingsLayout->addWidget(rangeInfo);
    auto *pdfButton = new QPushButton(QStringLiteral("打印到 PDF"));
    auto *printButton = new QPushButton(QStringLiteral("打印"));
    printButton->setDefault(true);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"));
    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(pdfButton);
    outputRow->addStretch();
    outputRow->addWidget(closeButton);
    outputRow->addWidget(printButton);
    settingsLayout->addLayout(outputRow);
    mainLayout->addWidget(settingsPanel);

    const auto selectedPaper = [&] {
        return QPageSize(QPageSize::PageSizeId(
            paperCombo->currentData().toInt()));
    };
    const auto selectedOrientation = [&] {
        return QPageLayout::Orientation(
            orientationCombo->currentData().toInt());
    };
    const auto paperMillimeters = [&] {
        QSizeF size =
            selectedPaper().size(QPageSize::Millimeter);
        if (selectedOrientation() == QPageLayout::Landscape) {
            size.transpose();
        }
        return size;
    };
    const auto updatePreview = [&] {
        const QSizeF millimeters = paperMillimeters();
        const double longEdge = 1500.0;
        QSize pixelSize;
        if (millimeters.width() >= millimeters.height()) {
            pixelSize = QSize(
                int(longEdge),
                qMax(64, qRound(longEdge * millimeters.height()
                                / millimeters.width())));
        } else {
            pixelSize = QSize(
                qMax(64, qRound(longEdge * millimeters.width()
                                / millimeters.height())),
                int(longEdge));
        }
        previewCanvas->setPage(
            m_view->renderPrintAreaToImage(
                worldArea, pixelSize,
                colorModeCombo->currentData().toBool()),
            millimeters);
    };
    const auto refreshPrinters = [&] {
        const QString previous = printerCombo->currentData().toString();
        printerCombo->clear();
        const QList<QPrinterInfo> printers =
            QPrinterInfo::availablePrinters();
        for (const QPrinterInfo &printer : printers) {
            printerCombo->addItem(printer.printerName(),
                                  printer.printerName());
        }
        const int previousIndex = printerCombo->findData(previous);
        if (previousIndex >= 0) {
            printerCombo->setCurrentIndex(previousIndex);
        } else {
            const QPrinterInfo defaultPrinter =
                QPrinterInfo::defaultPrinter();
            const int defaultIndex =
                defaultPrinter.isNull()
                ? -1 : printerCombo->findText(
                    defaultPrinter.printerName());
            if (defaultIndex >= 0) {
                printerCombo->setCurrentIndex(defaultIndex);
            }
        }
        const bool available = !printers.isEmpty();
        if (!available) {
            printerCombo->addItem(
                QStringLiteral("未检测到已配置打印机"));
            printerStatus->setText(
                QStringLiteral("没有可用打印机。可直接打印到 PDF，"
                               "或先打开系统打印机设置添加打印机。"));
        } else {
            printerStatus->setText(
                QStringLiteral("已检测到 %1 台打印机")
                    .arg(printers.size()));
        }
        printButton->setProperty("noPrinter", !available);
        systemSettingsButton->setVisible(!available);
    };
    const auto openSystemPrinterSettings = [&] {
#if defined(Q_OS_MACOS)
        const QUrl modern(
            QStringLiteral(
                "x-apple.systempreferences:"
                "com.apple.Print-Scan-Settings.extension"));
        const QUrl legacy(
            QStringLiteral(
                "x-apple.systempreferences:"
                "com.apple.preference.printfax"));
        if (!QDesktopServices::openUrl(modern)) {
            QDesktopServices::openUrl(legacy);
        }
#elif defined(Q_OS_WIN)
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:printers")));
#else
        QMessageBox::information(
            &dialog, QStringLiteral("系统打印机设置"),
            QStringLiteral("请使用操作系统设置配置打印机。"));
#endif
    };
    const auto printOutput = [&](bool toPdf) {
        if (!toPdf && printerCombo->currentData().toString().isEmpty()) {
            printerStatus->setText(
                QStringLiteral("没有可用打印机，正在打开系统打印机设置。"));
            openSystemPrinterSettings();
            return;
        }
        QPrinter output(QPrinter::HighResolution);
        if (toPdf) {
            QFileInfo sourceInfo(m_view->document()->path);
            QString baseName = sourceInfo.completeBaseName();
            if (sourceInfo.suffix().compare(
                    QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
                baseName += QStringLiteral("-打印");
            }
            QString suggested =
                sourceInfo.absolutePath() + QLatin1Char('/')
                + baseName + QStringLiteral(".pdf");
            QString path = QFileDialog::getSaveFileName(
                &dialog, QStringLiteral("打印到 PDF"), suggested,
                QStringLiteral("PDF 文件 (*.pdf)"));
            if (path.isEmpty()) {
                return;
            }
            if (QFileInfo(path).suffix().isEmpty()) {
                path += QStringLiteral(".pdf");
            }
            output.setOutputFormat(QPrinter::PdfFormat);
            output.setOutputFileName(path);
            output.setResolution(1200);
        } else {
            output.setPrinterName(
                printerCombo->currentData().toString());
            output.setOutputFormat(QPrinter::NativeFormat);
        }
        output.setPageSize(selectedPaper());
        output.setPageOrientation(selectedOrientation());
        output.setCopyCount(copies->value());
        if (!m_view->printToPrinter(
                &output, worldArea,
                colorModeCombo->currentData().toBool())) {
            QMessageBox::warning(
                &dialog, QStringLiteral("打印失败"),
                toPdf ? QStringLiteral("无法创建 PDF 文件。")
                      : QStringLiteral("无法发送到所选打印机。"));
            return;
        }
        statusBar()->showMessage(
            toPdf
                ? QStringLiteral("已打印到 PDF：%1")
                      .arg(output.outputFileName())
                : QStringLiteral("已发送到打印机：%1")
                      .arg(output.printerName()),
            7000);
        dialog.accept();
    };

    connect(refreshPrinterButton, &QPushButton::clicked,
            &dialog, refreshPrinters);
    connect(systemSettingsButton, &QPushButton::clicked,
            &dialog, openSystemPrinterSettings);
    connect(paperCombo, &QComboBox::currentIndexChanged,
            &dialog, [&] { updatePreview(); });
    connect(orientationCombo, &QComboBox::currentIndexChanged,
            &dialog, [&] { updatePreview(); });
    connect(colorModeCombo, &QComboBox::currentIndexChanged,
            &dialog, [&] { updatePreview(); });
    connect(ruler, &QCheckBox::toggled, previewCanvas,
            &PrintPreviewCanvas::setRulerVisible);
    connect(pdfButton, &QPushButton::clicked, &dialog,
            [&] { printOutput(true); });
    connect(printButton, &QPushButton::clicked, &dialog,
            [&] { printOutput(false); });
    connect(closeButton, &QPushButton::clicked, &dialog,
            &QDialog::reject);
    refreshPrinters();
    updatePreview();
    dialog.exec();
}

void MainWindow::changeUnit(int index)
{
    if (!m_view->document() || index < 0) {
        return;
    }
    m_view->setDisplayUnit(
        static_cast<MeasuringUnit>(m_unitCombo->itemData(index).toInt()));
    refreshMeasurementTable();
    refreshStatus();
    setDirty(true);
}

void MainWindow::changePrecision(int index)
{
    if (index < 0) {
        return;
    }
    m_view->setPrecision(m_precisionCombo->itemData(index).toInt());
    refreshMeasurementTable();
}

void MainWindow::saveAnnotationsNow()
{
    if (!m_view->document()) {
        return;
    }
    QString error;
    if (!saveAnnotations(m_view->document()->path, *m_view->document(),
                         m_view->displaySettings(), m_view->measurements(),
                         &error)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return;
    }
    m_annotationAutoSaveTimer->stop();
    QFile::remove(annotationAutosavePath(m_view->document()->path));
    setDirty(false);
    statusBar()->showMessage(
        QStringLiteral("图纸比例、单位与标注已保存到 %1")
            .arg(annotationSidecarPath(m_view->document()->path)),
        6000);
}

void MainWindow::autoSaveAnnotationsNow()
{
    if (!m_view || !m_view->document() || m_loading) {
        return;
    }
    QString error;
    if (saveAnnotationsAutosave(
            m_view->document()->path, *m_view->document(),
            m_view->displaySettings(), m_view->measurements(), &error)) {
        setDirty(false);
        statusBar()->showMessage(
            QStringLiteral("测量记录已自动保存"), 2200);
    } else {
        setDirty(true);
        statusBar()->showMessage(
            QStringLiteral("测量记录自动保存失败：%1").arg(error), 7000);
    }
}

void MainWindow::restoreCalibrationRecord(CadDocument &document)
{
    const QString key = drawingIdentityHash(document.path);

    QSettings settings = cacheSettings();
    settings.beginGroup(QStringLiteral("calibrationCache"));
    settings.beginGroup(key);
    const double meters =
        settings.value(QStringLiteral("metersPerDrawingUnit")).toDouble();
    if (!document.calibrated && meters > 0.0 && std::isfinite(meters)) {
        document.metersPerDrawingUnit = meters;
        document.calibrated = true;
    }
    settings.endGroup();
    settings.endGroup();

    if (document.calibrated) {
        rememberCalibrationRecord(document);
    }
}

void MainWindow::rememberCalibrationRecord(const CadDocument &document)
{
    if (!document.calibrated
        || !std::isfinite(document.metersPerDrawingUnit)
        || document.metersPerDrawingUnit <= 0.0) {
        return;
    }
    const QFileInfo info(document.path);
    const QString stablePath =
        info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    const QString key = drawingIdentityHash(document.path);

    QSettings settings = cacheSettings();
    settings.beginGroup(QStringLiteral("calibrationCache"));
    settings.beginGroup(key);
    settings.setValue(QStringLiteral("drawingPath"), stablePath);
    settings.setValue(QStringLiteral("fileSize"), info.size());
    settings.setValue(
        QStringLiteral("fileModified"),
        info.lastModified().toUTC().toSecsSinceEpoch());
    settings.setValue(QStringLiteral("metersPerDrawingUnit"),
                      document.metersPerDrawingUnit);
    settings.setValue(QStringLiteral("calibrated"), true);
    settings.setValue(
        QStringLiteral("lastOpened"),
        QDateTime::currentSecsSinceEpoch());
    settings.endGroup();
    settings.endGroup();
}

void MainWindow::updateRecentDrawing(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return;
    }
    const QString stablePath =
        info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    QSettings settings = cacheSettings();
    const QVariantList current =
        settings.value(QStringLiteral("recentDrawings")).toList();
    QVariantList updated;
    QVariantMap entry;
    entry.insert(QStringLiteral("path"), stablePath);
    entry.insert(
        QStringLiteral("lastOpened"),
        QDateTime::currentSecsSinceEpoch());
    updated.append(entry);
    for (const QVariant &value : current) {
        QVariantMap old = value.toMap();
        const QString oldPath =
            old.value(QStringLiteral("path")).toString();
        if (oldPath.isEmpty() || oldPath == stablePath
            || !QFileInfo::exists(oldPath)) {
            continue;
        }
        const QFileInfo oldInfo(oldPath);
        const QString oldStablePath =
            oldInfo.canonicalFilePath().isEmpty()
            ? oldInfo.absoluteFilePath() : oldInfo.canonicalFilePath();
        old.insert(QStringLiteral("path"), oldStablePath);
        old.insert(QStringLiteral("cacheKey"),
                   drawingIdentityHash(oldStablePath));
        updated.append(old);
        if (updated.size() >= 20) {
            break;
        }
    }
    settings.setValue(QStringLiteral("recentDrawings"), updated);
    settings.sync();
    pruneSystemCachesToRecent(updated);
    refreshStartScreen();
}

void MainWindow::refreshStartScreen()
{
    if (!m_startScreen) {
        return;
    }
    QSettings settings = cacheSettings();
    const QVariantList stored =
        settings.value(QStringLiteral("recentDrawings")).toList();
    const QString thumbnailDirectory =
        QStandardPaths::writableLocation(
            QStandardPaths::CacheLocation)
        + QStringLiteral("/recent-thumbnails");
    QVector<RecentDrawing> drawings;
    QVariantList validRecords;
    for (const QVariant &value : stored) {
        if (validRecords.size() >= 20) {
            break;
        }
        QVariantMap record = value.toMap();
        const QString path =
            record.value(QStringLiteral("path")).toString();
        const QFileInfo info(path);
        if (path.isEmpty() || !info.exists() || !info.isFile()) {
            continue;
        }
        const QString stablePath =
            info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath() : info.canonicalFilePath();
        record.insert(QStringLiteral("path"), stablePath);
        record.insert(QStringLiteral("cacheKey"),
                      drawingIdentityHash(stablePath));
        validRecords.append(record);
        RecentDrawing drawing;
        drawing.path = stablePath;
        drawing.title = info.fileName();
        drawing.lastOpened = QDateTime::fromSecsSinceEpoch(
            record.value(QStringLiteral("lastOpened")).toLongLong());
        drawing.thumbnailPath =
            thumbnailDirectory + QLatin1Char('/')
            + drawingIdentityHash(stablePath)
            + QStringLiteral(".png");
        drawings.append(std::move(drawing));
    }
    settings.setValue(QStringLiteral("recentDrawings"), validRecords);
    settings.sync();
    pruneSystemCachesToRecent(validRecords);
    m_startScreen->setRecentDrawings(drawings);
}

void MainWindow::removeRecentDrawing(const QString &path)
{
    QSettings settings = cacheSettings();
    const QVariantList current =
        settings.value(QStringLiteral("recentDrawings")).toList();
    const QFileInfo removedInfo(path);
    const QString removedPath =
        removedInfo.canonicalFilePath().isEmpty()
        ? removedInfo.absoluteFilePath()
        : removedInfo.canonicalFilePath();
    QVariantList updated;
    bool removed = false;
    for (const QVariant &value : current) {
        QVariantMap record = value.toMap();
        const QFileInfo info(
            record.value(QStringLiteral("path")).toString());
        const QString stablePath =
            info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath() : info.canonicalFilePath();
        if (!stablePath.isEmpty() && stablePath == removedPath) {
            removed = true;
            continue;
        }
        record.insert(QStringLiteral("path"), stablePath);
        updated.append(record);
    }
    if (updated.size() > 20) {
        updated = updated.mid(0, 20);
    }
    settings.setValue(QStringLiteral("recentDrawings"), updated);
    settings.sync();
    pruneSystemCachesToRecent(updated);
    refreshStartScreen();
    if (removed) {
        statusBar()->showMessage(
            QStringLiteral("已从最近打开列表移除，并清理对应系统缓存"),
            5000);
    }
}

void MainWindow::pruneSystemCachesToRecent(
    const QVariantList &recentRecords)
{
    QSet<QString> allowedPaths;
    QSet<QString> allowedCacheKeys;
    QSet<QString> allowedParsedFiles;
    for (const QVariant &value : recentRecords) {
        const QString path =
            value.toMap().value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            continue;
        }
        const QFileInfo info(path);
        const QString stablePath =
            info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath() : info.canonicalFilePath();
        allowedPaths.insert(stablePath);
        allowedCacheKeys.insert(drawingIdentityHash(stablePath));
        allowedParsedFiles.insert(
            QFileInfo(drawingParsedCachePath(stablePath)).fileName());
    }

    QSettings settings = cacheSettings();
    settings.beginGroup(QStringLiteral("calibrationCache"));
    const QStringList calibrationKeys = settings.childGroups();
    settings.endGroup();
    for (const QString &key : calibrationKeys) {
        const QString group =
            QStringLiteral("calibrationCache/") + key;
        const QString path =
            settings.value(group + QStringLiteral("/drawingPath"))
                .toString();
        const QFileInfo info(path);
        const QString stablePath =
            info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath() : info.canonicalFilePath();
        if (!allowedPaths.contains(stablePath)
            || !allowedCacheKeys.contains(key)) {
            settings.remove(group);
        }
    }
    settings.sync();

    const QString cacheRoot =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheRoot.isEmpty()) {
        return;
    }

    QDir parsedDirectory(cacheRoot + QStringLiteral("/parsed"));
    const QStringList parsedFiles = parsedDirectory.entryList(
        {QStringLiteral("*.cadcache")}, QDir::Files);
    for (const QString &fileName : parsedFiles) {
        const QString cachePath =
            parsedDirectory.filePath(fileName);
        QFile file(cachePath);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_8);
        quint32 magic = 0;
        quint32 version = 0;
        QString drawingPath;
        stream >> magic >> version >> drawingPath;
        if (stream.status() != QDataStream::Ok
            || magic != 0x4341444cU || drawingPath.isEmpty()) {
            continue;
        }
        const QFileInfo info(drawingPath);
        const QString stablePath =
            info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath() : info.canonicalFilePath();
        if (!allowedPaths.contains(stablePath)
            || !allowedParsedFiles.contains(fileName)) {
            file.close();
            parsedDirectory.remove(fileName);
        }
    }

    const auto pruneKeyedDirectory =
        [&allowedCacheKeys](const QString &directoryPath,
                            const QStringList &nameFilters,
                            const std::function<QString(const QString &)>
                                &cacheKeyForFile) {
            QDir directory(directoryPath);
            const QStringList files =
                directory.entryList(nameFilters, QDir::Files);
            for (const QString &fileName : files) {
                const QString key = cacheKeyForFile(fileName);
                if (!key.isEmpty() && !allowedCacheKeys.contains(key)) {
                    directory.remove(fileName);
                }
            }
        };
    pruneKeyedDirectory(
        cacheRoot + QStringLiteral("/drawings"),
        {QStringLiteral("*.cadlens.json"),
         QStringLiteral("*.cadlens.autosave.json")},
        [](const QString &fileName) {
            QString key = fileName;
            const QString autosaveSuffix =
                QStringLiteral(".cadlens.autosave.json");
            const QString sidecarSuffix =
                QStringLiteral(".cadlens.json");
            if (key.endsWith(autosaveSuffix)) {
                key.chop(autosaveSuffix.size());
            } else if (key.endsWith(sidecarSuffix)) {
                key.chop(sidecarSuffix.size());
            }
            return key;
        });
    pruneKeyedDirectory(
        cacheRoot + QStringLiteral("/recent-thumbnails"),
        {QStringLiteral("*.png")},
        [](const QString &fileName) {
            return QFileInfo(fileName).completeBaseName();
        });
}

void MainWindow::saveRecentThumbnail(const QString &path,
                                     const QImage &image)
{
    if (image.isNull()) {
        return;
    }
    const QString thumbnailDirectory =
        QStandardPaths::writableLocation(
            QStandardPaths::CacheLocation)
        + QStringLiteral("/recent-thumbnails");
    if (!QDir().mkpath(thumbnailDirectory)) {
        return;
    }
    const QString thumbnailPath =
        thumbnailDirectory + QLatin1Char('/')
        + drawingIdentityHash(path) + QStringLiteral(".png");
    image.save(thumbnailPath, "PNG");
}

void MainWindow::saveAnnotationsAs()
{
    if (!m_view->document()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("标注另存为"), m_lastDirectory,
        QStringLiteral("速看CAD 标注 (*.cadlens.json);;JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             file.errorString());
        return;
    }
    QJsonArray annotations;
    for (const Measurement &measurement : m_view->measurements()) {
        annotations.append(measurement.toJson());
    }
    const DisplaySettings settings = m_view->displaySettings();
    QJsonObject root;
    root.insert(QStringLiteral("schema"),
                QStringLiteral("cadlens.annotations"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("drawing"),
                QFileInfo(m_view->document()->path).fileName());
    root.insert(QStringLiteral("annotationStyle"),
                annotationStyleKey(settings.annotationStyle));
    root.insert(QStringLiteral("metersPerDrawingUnit"),
                m_view->document()->metersPerDrawingUnit);
    root.insert(QStringLiteral("calibrated"),
                m_view->document()->calibrated);
    root.insert(QStringLiteral("displayUnit"), int(settings.lengthUnit));
    root.insert(QStringLiteral("lengthUnit"), int(settings.lengthUnit));
    root.insert(QStringLiteral("areaUnit"), int(settings.areaUnit));
    root.insert(QStringLiteral("coordinateUnit"),
                int(settings.coordinateUnit));
    root.insert(QStringLiteral("lengthPrecision"),
                settings.lengthPrecision);
    root.insert(QStringLiteral("areaPrecision"), settings.areaPrecision);
    root.insert(QStringLiteral("coordinatePrecision"),
                settings.coordinatePrecision);
    root.insert(QStringLiteral("annotationColor"),
                settings.annotationColor.name(QColor::HexArgb));
    root.insert(QStringLiteral("measurements"), annotations);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    statusBar()->showMessage(QStringLiteral("标注副本已保存"), 5000);
}

void MainWindow::exportSvgAs()
{
    if (!m_view->document()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出矢量图纸"), m_lastDirectory,
        QStringLiteral("SVG 矢量图 (*.svg)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!m_view->exportSvg(path, &error)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), error);
    } else {
        statusBar()->showMessage(
            QStringLiteral("已导出矢量 SVG：%1").arg(path), 7000);
    }
}

void MainWindow::setDirty(bool dirty)
{
    if (m_dirty == dirty) {
        return;
    }
    m_dirty = dirty;
    m_saveAction->setEnabled(m_view->document() != nullptr);
    QString title = windowTitle();
    if (dirty && !title.startsWith(QLatin1Char('*'))) {
        setWindowTitle(QLatin1Char('*') + title);
    } else if (!dirty && title.startsWith(QLatin1Char('*'))) {
        setWindowTitle(title.mid(1));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_dirty && m_view->document()) {
        m_annotationAutoSaveTimer->stop();
        autoSaveAnnotationsNow();
    }
    if (m_dirty && m_view->document()) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, QStringLiteral("未保存的标注"),
            QStringLiteral("测量记录自动保存失败，是否另存后再退出？"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save) {
            saveAnnotationsNow();
            if (m_dirty) {
                event->ignore();
                return;
            }
        }
    }
    event->accept();
}
