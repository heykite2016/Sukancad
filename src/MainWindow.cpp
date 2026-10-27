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
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLinearGradient>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNativeGestureEvent>
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
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrent>

#include <cmath>
#include <algorithm>
#include <functional>
#include <limits>
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

QString applicationSettingsPath()
{
    QString directory = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
    }
    if (directory.isEmpty()) {
        directory = QDir::tempPath() + QStringLiteral("/CadLens/config");
    }
    QDir().mkpath(directory);
    return directory + QStringLiteral("/settings.ini");
}

QSettings applicationSettings()
{
    return QSettings(applicationSettingsPath(), QSettings::IniFormat);
}

void saveApplicationDisplaySettings(const DisplaySettings &settings)
{
    QSettings values = applicationSettings();
    values.setValue(QStringLiteral("display/annotationStyle"),
                    annotationStyleKey(settings.annotationStyle));
    values.setValue(
        QStringLiteral("display/annotationColor"),
        settings.annotationColor.name(QColor::HexArgb));
    values.setValue(
        QStringLiteral("display/crosshairColor"),
        settings.crosshairColor.name(QColor::HexArgb));
    values.setValue(
        QStringLiteral("display/crosshairLength"),
        qBound(1, settings.crosshairLength, 100));
    values.setValue(
        QStringLiteral("display/crosshairLineWidth"),
        qBound(0.5, settings.crosshairLineWidth, 4.0));
    values.setValue(
        QStringLiteral("display/lengthUnit"), int(settings.lengthUnit));
    values.setValue(
        QStringLiteral("display/areaUnit"), int(settings.areaUnit));
    values.setValue(
        QStringLiteral("display/coordinateUnit"),
        int(settings.coordinateUnit));
    values.setValue(
        QStringLiteral("display/lengthPrecision"),
        qBound(0, settings.lengthPrecision, 6));
    values.setValue(
        QStringLiteral("display/areaPrecision"),
        qBound(0, settings.areaPrecision, 6));
    values.setValue(
        QStringLiteral("display/coordinatePrecision"),
        qBound(0, settings.coordinatePrecision, 6));
    values.sync();
}

void loadApplicationDisplaySettings(DisplaySettings &settings)
{
    QSettings values = applicationSettings();
    settings.annotationStyle = annotationStyleFromKey(
        values.value(
            QStringLiteral("display/annotationStyle"),
            annotationStyleKey(settings.annotationStyle))
            .toString(),
        settings.annotationStyle);
    QColor color(values.value(
        QStringLiteral("display/annotationColor")).toString());
    if (color.isValid()) {
        settings.annotationColor = color;
    }
    color = QColor(values.value(
        QStringLiteral("display/crosshairColor")).toString());
    if (color.isValid()) {
        settings.crosshairColor = color;
    }
    settings.crosshairLength = qBound(
        1,
        values.value(
            QStringLiteral("display/crosshairLength"),
            settings.crosshairLength).toInt(),
        100);
    settings.crosshairLineWidth = qBound(
        0.5,
        values.value(
            QStringLiteral("display/crosshairLineWidth"),
            settings.crosshairLineWidth).toDouble(),
        4.0);
    settings.lengthUnit = MeasuringUnit(values.value(
        QStringLiteral("display/lengthUnit"),
        int(settings.lengthUnit)).toInt());
    settings.areaUnit = MeasuringUnit(values.value(
        QStringLiteral("display/areaUnit"),
        int(settings.areaUnit)).toInt());
    settings.coordinateUnit = MeasuringUnit(values.value(
        QStringLiteral("display/coordinateUnit"),
        int(settings.coordinateUnit)).toInt());
    settings.lengthPrecision = qBound(
        0, values.value(
            QStringLiteral("display/lengthPrecision"),
            settings.lengthPrecision).toInt(), 6);
    settings.areaPrecision = qBound(
        0, values.value(
            QStringLiteral("display/areaPrecision"),
            settings.areaPrecision).toInt(), 6);
    settings.coordinatePrecision = qBound(
        0, values.value(
            QStringLiteral("display/coordinatePrecision"),
            settings.coordinatePrecision).toInt(), 6);
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

QIcon themedCadIcon(const QString &name, const QString &theme)
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
        const char *gray;
        const char *blueprint;
    };
    static const ColorPair colors[] = {
        {"#aebdcc", "#7fe7ff", "#075d85", "#d2dde3", "#c4ebff"},
        {"#58dbc8", "#31ffbd", "#007f62", "#62dfc2", "#62f0d2"},
        {"#5da7d3", "#56c8ff", "#005f8c", "#78c8ed", "#78d8ff"},
        {"#69dfa0", "#5cff9d", "#04753b", "#82dfa4", "#91ecb4"},
        {"#cbd6dd", "#eafcff", "#0b526a", "#eef4f6", "#e8f8ff"},
        {"#dce7ee", "#ffffff", "#173947", "#ffffff", "#ffffff"},
        {"#ffb84d", "#ffb000", "#a64600", "#ffc76c", "#ffd387"}
    };
    for (const ColorPair &color : colors) {
        const char *target = color.light;
        if (theme == QStringLiteral("cadDark")) {
            target = color.dark;
        } else if (theme == QStringLiteral("cadGray")) {
            target = color.gray;
        } else if (theme == QStringLiteral("cadBlueprint")) {
            target = color.blueprint;
        }
        svg.replace(QString::fromLatin1(color.source),
                    QString::fromLatin1(target),
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
    struct Page {
        QImage image;
        QSizeF paperMillimeters;
    };

    explicit PrintPreviewCanvas(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(520, 480);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setCursor(Qt::OpenHandCursor);
    }

    void setPages(QVector<Page> pages, int focusPage)
    {
        m_pages = std::move(pages);
        m_zoom = 1.0;
        m_pan = QPointF();
        m_currentPage = qBound(
            0, focusPage, qMax(0, m_pages.size() - 1));
        scrollToPage(m_currentPage, true);
        update();
    }

    void setRulerVisible(bool visible)
    {
        m_rulerVisible = visible;
        update();
    }

    void setVisiblePageHandler(
        std::function<void(int)> handler)
    {
        m_visiblePageHandler = std::move(handler);
    }

    void scrollToPage(int page, bool keepZoom = false)
    {
        if (m_pages.isEmpty()) {
            return;
        }
        m_currentPage = qBound(0, page, m_pages.size() - 1);
        if (!keepZoom) {
            m_zoom = 1.0;
        }
        m_pan.setX(0.0);
        const QVector<QRectF> rectangles = pageRects();
        if (m_currentPage < rectangles.size()) {
            m_pan.setY(
                availableRect().top() - rectangles[m_currentPage].top());
        }
        clampPan();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient workspace(rect().topLeft(), rect().bottomRight());
        workspace.setColorAt(0.0, QColor(QStringLiteral("#edf2f5")));
        workspace.setColorAt(1.0, QColor(QStringLiteral("#ccd6dc")));
        painter.fillRect(rect(), workspace);
        if (m_pages.isEmpty()) {
            return;
        }
        const QRectF available = availableRect();
        if (available.width() <= 1.0 || available.height() <= 1.0) {
            return;
        }
        painter.save();
        painter.setClipRect(rect());
        const QVector<QRectF> rectangles = pageRects();
        for (int index = 0; index < rectangles.size(); ++index) {
            const QRectF page = rectangles[index];
            if (!page.intersects(rect())) {
                continue;
            }
            const Page &source = m_pages[index];
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(23, 43, 54, 26));
            painter.drawRect(page.translated(9.0, 11.0));
            painter.setBrush(QColor(23, 43, 54, 42));
            painter.drawRect(page.translated(5.0, 6.0));
            painter.setBrush(Qt::white);
            const bool currentPage = index == m_currentPage;
            painter.setPen(QPen(
                currentPage
                    ? QColor(QStringLiteral("#2d8fb3"))
                    : QColor(QStringLiteral("#8b9aa3")),
                currentPage ? 1.6 : 0.8));
            painter.drawRect(page);
            painter.setRenderHint(
                QPainter::SmoothPixmapTransform, true);
            painter.drawImage(page, source.image);
            if (m_rulerVisible) {
                paintRuler(painter, page, source.paperMillimeters);
            }
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(
                currentPage
                    ? QColor(QStringLiteral("#1f6f8b"))
                    : QColor(QStringLiteral("#667982")));
            painter.setFont(QFont(QString(), 9, QFont::DemiBold));
            painter.drawText(
                QRectF(page.left(), page.bottom() + 8.0,
                       page.width(), 18.0),
                Qt::AlignHCenter | Qt::AlignVCenter,
                QStringLiteral("第 %1 / %2 页")
                    .arg(index + 1).arg(m_pages.size()));
        }
        painter.restore();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (event->angleDelta().y() == 0
            && event->pixelDelta().y() == 0) {
            return;
        }
        const double delta =
            event->angleDelta().y() != 0
            ? event->angleDelta().y() / 120.0
            : event->pixelDelta().y() / 80.0;
        if (event->modifiers().testFlag(Qt::ControlModifier)
            || event->modifiers().testFlag(Qt::MetaModifier)) {
            applyZoom(std::pow(1.18, delta), event->position());
            event->accept();
            return;
        }
        const double pixels =
            event->pixelDelta().y() != 0
            ? event->pixelDelta().y()
            : delta * 96.0;
        m_pan.ry() += pixels;
        clampPan();
        updateVisiblePage();
        update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        m_dragging = true;
        m_lastMousePosition = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            return;
        }
        m_pan += event->position() - m_lastMousePosition;
        m_lastMousePosition = event->position();
        clampPan();
        updateVisiblePage();
        update();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }
        m_zoom = 1.0;
        scrollToPage(m_currentPage);
        event->accept();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::NativeGesture) {
            auto *gesture = static_cast<QNativeGestureEvent *>(event);
            if (gesture->gestureType() == Qt::ZoomNativeGesture) {
                applyZoom(
                    qBound(0.8, 1.0 + gesture->value(), 1.25),
                    gesture->position());
                event->accept();
                return true;
            }
        }
        return QWidget::event(event);
    }

private:
    QRectF availableRect() const
    {
        const qreal ruler = m_rulerVisible ? 42.0 : 0.0;
        return QRectF(rect()).adjusted(
            18.0 + ruler, 18.0 + ruler,
            -18.0, -18.0);
    }

    QSizeF fittedPageSize(const QSizeF &paperMillimeters) const
    {
        const QRectF available = availableRect();
        if (available.width() <= 1.0 || available.height() <= 1.0
            || paperMillimeters.width() <= 0.0
            || paperMillimeters.height() <= 0.0) {
            return QSizeF();
        }
        const qreal paperAspect =
            paperMillimeters.width() / paperMillimeters.height();
        return QSizeF(available.width(), available.width() / paperAspect);
    }

    QVector<QRectF> pageRects() const
    {
        QVector<QRectF> result;
        result.reserve(m_pages.size());
        const QRectF available = availableRect();
        const double gap = 30.0 * m_zoom;
        double y = available.top() + m_pan.y();
        for (const Page &page : std::as_const(m_pages)) {
            const QSizeF fitted = fittedPageSize(
                page.paperMillimeters);
            if (!fitted.isValid() || fitted.width() <= 0.0) {
                continue;
            }
            const QSizeF size(
                fitted.width() * m_zoom,
                fitted.height() * m_zoom);
            result.append(QRectF(
                available.center().x() + m_pan.x()
                    - size.width() * 0.5,
                y, size.width(), size.height()));
            y += size.height() + gap;
        }
        return result;
    }

    void applyZoom(double factor, const QPointF &anchor)
    {
        if (!std::isfinite(factor) || factor <= 0.0) {
            return;
        }
        const QVector<QRectF> before = pageRects();
        if (before.isEmpty()) {
            return;
        }
        const QRectF available = availableRect();
        const QPointF origin(
            available.center().x() + m_pan.x(),
            available.top() + m_pan.y());
        const QPointF baseAnchor(
            (anchor.x() - origin.x()) / m_zoom,
            (anchor.y() - origin.y()) / m_zoom);
        m_zoom = qBound(0.2, m_zoom * factor, 8.0);
        m_pan = QPointF(
            anchor.x() - baseAnchor.x() * m_zoom
                - available.center().x(),
            anchor.y() - baseAnchor.y() * m_zoom
                - available.top());
        clampPan();
        update();
    }

    void clampPan()
    {
        if (m_pages.isEmpty()) {
            m_pan = QPointF();
            return;
        }
        const QRectF available = availableRect();
        const double gap = 30.0 * m_zoom;
        double maximumPageWidth = 0.0;
        double contentHeight = 0.0;
        int pageCount = 0;
        for (const Page &page : std::as_const(m_pages)) {
            const QSizeF fitted = fittedPageSize(
                page.paperMillimeters);
            if (!fitted.isValid() || fitted.width() <= 0.0) {
                continue;
            }
            maximumPageWidth = qMax(
                maximumPageWidth, fitted.width() * m_zoom);
            contentHeight += fitted.height() * m_zoom;
            contentHeight += gap;
            ++pageCount;
        }
        if (pageCount == 0) {
            m_pan = QPointF();
            return;
        }
        if (contentHeight > available.height()) {
            m_pan.setY(qBound(
                available.height() - contentHeight,
                m_pan.y(), 0.0));
        } else {
            m_pan.setY(0.0);
        }
        if (maximumPageWidth > available.width()) {
            const double halfDifference =
                (maximumPageWidth - available.width()) * 0.5;
            m_pan.setX(qBound(
                -halfDifference, m_pan.x(), halfDifference));
        } else {
            m_pan.setX(0.0);
        }
    }

    void updateVisiblePage()
    {
        const QVector<QRectF> rectangles = pageRects();
        if (rectangles.isEmpty()) {
            return;
        }
        const double viewportCenter = rect().center().y();
        int bestPage = 0;
        double bestDistance =
            std::numeric_limits<double>::max();
        for (int index = 0; index < rectangles.size(); ++index) {
            const double distance = qAbs(
                rectangles[index].center().y() - viewportCenter);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestPage = index;
            }
        }
        if (bestPage != m_currentPage) {
            m_currentPage = bestPage;
            if (m_visiblePageHandler) {
                m_visiblePageHandler(bestPage);
            }
        }
    }

    void paintRuler(QPainter &painter, const QRectF &page,
                    const QSizeF &paperMillimeters)
    {
        if (paperMillimeters.width() <= 0.0
            || paperMillimeters.height() <= 0.0) {
            return;
        }
        const double xScale = page.width() / paperMillimeters.width();
        const double yScale = page.height() / paperMillimeters.height();
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(QStringLiteral("#3f5764")), 0.8));
        painter.setFont(QFont(QString(), 7));
        const double top = page.top() - 5.0;
        painter.drawLine(QPointF(page.left(), top),
                         QPointF(page.right(), top));
        for (int millimeter = 0;
             millimeter <= int(std::ceil(paperMillimeters.width()));
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
             millimeter <= int(std::ceil(paperMillimeters.height()));
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

    QVector<Page> m_pages;
    bool m_rulerVisible = true;
    double m_zoom = 1.0;
    QPointF m_pan;
    bool m_dragging = false;
    QPointF m_lastMousePosition;
    int m_currentPage = 0;
    std::function<void(int)> m_visiblePageHandler;
};

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("速看CAD · 工程施工看图"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/logo.svg")));
    resize(1500, 940);
    setMinimumSize(1040, 680);

    QSettings settings = applicationSettings();
    QSettings legacySettings = cacheSettings();
    if (!settings.contains(QStringLiteral("uiTheme"))
        && legacySettings.contains(QStringLiteral("uiTheme"))) {
        settings.setValue(
            QStringLiteral("uiTheme"),
            legacySettings.value(QStringLiteral("uiTheme")));
        settings.sync();
    }
    const QString savedTheme =
        settings.value(QStringLiteral("uiTheme"),
                       QStringLiteral("cadDark")).toString();
    m_cadTheme =
        savedTheme == QStringLiteral("cadLight") ? CadTheme::Light
        : savedTheme == QStringLiteral("cadGray") ? CadTheme::Gray
        : savedTheme == QStringLiteral("cadBlueprint")
            ? CadTheme::Blueprint : CadTheme::Dark;

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
        legacySettings.value(
            QStringLiteral("lastDirectory"),
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
    const CadView::DrawingTheme drawingTheme =
        m_cadTheme == CadTheme::Light
            ? CadView::DrawingTheme::CadLight
        : m_cadTheme == CadTheme::Gray
            ? CadView::DrawingTheme::CadGray
        : m_cadTheme == CadTheme::Blueprint
            ? CadView::DrawingTheme::CadBlueprint
            : CadView::DrawingTheme::CadDark;
    view->setDrawingTheme(drawingTheme);
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
    connect(view, &CadView::printAreaSelectionCanceled, this,
            &MainWindow::handlePrintAreaCanceled);
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

QIcon MainWindow::themedIcon(const QString &name) const
{
    const QString theme =
        m_cadTheme == CadTheme::Light
            ? QStringLiteral("cadLight")
        : m_cadTheme == CadTheme::Gray
            ? QStringLiteral("cadGray")
        : m_cadTheme == CadTheme::Blueprint
            ? QStringLiteral("cadBlueprint")
            : QStringLiteral("cadDark");
    return themedCadIcon(name, theme);
}

QAction *MainWindow::makeAction(const QString &text, const QString &icon,
                                const QKeySequence &shortcut)
{
    auto *action = new QAction(themedIcon(icon), text, this);
    action->setProperty("cadIconName", icon);
    if (!shortcut.isEmpty()) {
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WindowShortcut);
    }
    QString help = text;
    if (!shortcut.isEmpty()) {
        help += QStringLiteral("  [%1]").arg(
            shortcut.toString(QKeySequence::NativeText));
    }
    action->setToolTip(help);
    action->setStatusTip(help);
    return action;
}

void MainWindow::refreshActionIcons()
{
    const QList<QAction *> actions = findChildren<QAction *>();
    for (QAction *action : actions) {
        const QString iconName =
            action->property("cadIconName").toString();
        if (!iconName.isEmpty()) {
            action->setIcon(themedIcon(iconName));
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
        makeAction(QStringLiteral("打印预览"),
                   QStringLiteral("print"),
                   QKeySequence(QStringLiteral("Ctrl+P")));
    connect(m_printAction, &QAction::triggered, this,
            [this] { requestPrint(); });

    m_selectAction = makeToolAction(QStringLiteral("选择"),
                                    QStringLiteral("select"),
                                    CadView::ToolMode::Select,
                                    QKeySequence(QStringLiteral("V")));
    m_eraseAction =
        makeToolAction(QStringLiteral("删除"),
                       QStringLiteral("delete"), CadView::ToolMode::Erase,
                       QKeySequence(QStringLiteral("E")));
    m_deleteAllAction =
        makeAction(QStringLiteral("删除全部标注"),
                   QStringLiteral("delete-all"),
                   QKeySequence(QStringLiteral("Shift+Delete")));
    m_deleteAllAction->setToolTip(
        QStringLiteral("确认后删除当前图纸的全部标注  [%1]")
            .arg(m_deleteAllAction->shortcut().toString(
                QKeySequence::NativeText)));
    m_deleteAllAction->setStatusTip(
        QStringLiteral("确认后删除当前图纸的全部标注"));
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
    makeToolAction(QStringLiteral("对齐标注"), QStringLiteral("linear"),
                   CadView::ToolMode::Aligned,
                   QKeySequence(QStringLiteral("A")));
    makeToolAction(QStringLiteral("线性标注"), QStringLiteral("linear-horizontal"),
                   CadView::ToolMode::Linear,
                   QKeySequence(QStringLiteral("L")));
    makeToolAction(QStringLiteral("连续测量"), QStringLiteral("continuous"),
                   CadView::ToolMode::Continuous,
                   QKeySequence(QStringLiteral("C")));
    makeToolAction(QStringLiteral("批量标注"), QStringLiteral("batch"),
                   CadView::ToolMode::Batch,
                   QKeySequence(QStringLiteral("B")));
    makeToolAction(QStringLiteral("矩形面积"),
                   QStringLiteral("rectangle"), CadView::ToolMode::Rectangle,
                   QKeySequence(QStringLiteral("Shift+R")));
    makeToolAction(QStringLiteral("多边形面积"),
                   QStringLiteral("polygon"), CadView::ToolMode::Polygon,
                   QKeySequence(QStringLiteral("P")));
    makeToolAction(QStringLiteral("圆测量"),
                   QStringLiteral("circle-area"),
                   CadView::ToolMode::CircleArea,
                   QKeySequence(QStringLiteral("O")));
    makeToolAction(QStringLiteral("填充面积"), QStringLiteral("filled-area"),
                   CadView::ToolMode::FilledArea,
                   QKeySequence(QStringLiteral("F")));
    makeToolAction(QStringLiteral("半径标注"), QStringLiteral("radius"),
                   CadView::ToolMode::Radius,
                   QKeySequence(QStringLiteral("R")));
    makeToolAction(QStringLiteral("直径标注"), QStringLiteral("diameter"),
                   CadView::ToolMode::Diameter,
                   QKeySequence(QStringLiteral("Shift+D")));
    makeToolAction(QStringLiteral("角度标注"), QStringLiteral("angle"),
                   CadView::ToolMode::Angle,
                   QKeySequence(QStringLiteral("G")));
    makeToolAction(QStringLiteral("弧长标注"), QStringLiteral("arc"),
                   CadView::ToolMode::ArcLength,
                   QKeySequence(QStringLiteral("K")));
    makeToolAction(QStringLiteral("坐标标注"), QStringLiteral("coordinate"),
                   CadView::ToolMode::Coordinate,
                   QKeySequence(QStringLiteral("X")));
    makeToolAction(QStringLiteral("点到线距离"),
                   QStringLiteral("point-line"),
                   CadView::ToolMode::PointToLine,
                   QKeySequence(QStringLiteral("J")));
    makeToolAction(QStringLiteral("文字注释"), QStringLiteral("text"),
                   CadView::ToolMode::Text,
                   QKeySequence(QStringLiteral("T")));

    const auto addAuxiliaryTool =
        [this](const QString &text, const QString &icon,
               CadView::ToolMode mode,
               const QKeySequence &shortcut,
               const std::function<void()> &starter) {
            QAction *action = makeAction(text, icon, shortcut);
            action->setCheckable(true);
            action->setData(int(mode));
            m_toolGroup->addAction(action);
            connect(action, &QAction::triggered, this, starter);
        };
    addAuxiliaryTool(QStringLiteral("定距标注"),
                     QStringLiteral("fixed-distance"),
                     CadView::ToolMode::FixedDistance,
                     QKeySequence(QStringLiteral("M")),
                     [this] { beginFixedDistance(); });
    addAuxiliaryTool(QStringLiteral("侧面积"),
                     QStringLiteral("side-area"),
                     CadView::ToolMode::SideArea,
                     QKeySequence(QStringLiteral("Shift+S")),
                     [this] { beginSideArea(); });
    addAuxiliaryTool(QStringLiteral("偏移面积"),
                     QStringLiteral("area-offset"),
                     CadView::ToolMode::AreaOffset,
                     QKeySequence(QStringLiteral("U")),
                     [this] { beginAreaOffset(); });
    makeToolAction(QStringLiteral("校准比例"),
                   QStringLiteral("calibrate"),
                   CadView::ToolMode::Calibrate,
                   QKeySequence(QStringLiteral("Shift+C")));
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
                m_annotationVisibilityAction->setIcon(themedIcon(icon));
            });
    m_annotationSettingsAction =
        makeAction(QStringLiteral("标注显示与测量设置"),
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
    m_gridAction->setChecked(true);
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

    const auto toolHelp = [](CadView::ToolMode mode) {
        switch (mode) {
        case CadView::ToolMode::Select:
            return QStringLiteral(
                "单击选择或拖动标注；在空白处拖动可平移图纸");
        case CadView::ToolMode::Erase:
            return QStringLiteral(
                "单击删除单个标注，拖动框选可批量删除");
        case CadView::ToolMode::Aligned:
            return QStringLiteral(
                "依次选择两点，创建沿两点方向的真实距离标注");
        case CadView::ToolMode::Linear:
            return QStringLiteral(
                "依次选择两点，创建水平或垂直线性尺寸");
        case CadView::ToolMode::Continuous:
            return QStringLiteral(
                "连续点击端点或圆弧测量多段长度；右键、Enter 或双击结束");
        case CadView::ToolMode::Batch:
            return QStringLiteral(
                "连续选择多个节点，一次生成各段独立尺寸");
        case CadView::ToolMode::Rectangle:
            return QStringLiteral(
                "选择两个对角点，测量矩形周长与面积");
        case CadView::ToolMode::Polygon:
            return QStringLiteral(
                "依次选择闭合边界点，测量周长与面积");
        case CadView::ToolMode::CircleArea:
            return QStringLiteral(
                "在圆周上选择三个点，测量半径、周长与面积");
        case CadView::ToolMode::FilledArea:
            return QStringLiteral(
                "单击闭合填充或轮廓内部，读取填充面积");
        case CadView::ToolMode::SideArea:
            return QStringLiteral(
                "连续选择线段或圆弧，并按指定高度计算总长和侧面积");
        case CadView::ToolMode::AreaOffset:
            return QStringLiteral(
                "为闭合轮廓设置偏移距离，计算偏移后的面积");
        case CadView::ToolMode::Radius:
            return QStringLiteral(
                "在圆周上选择三个点，创建半径标注");
        case CadView::ToolMode::Diameter:
            return QStringLiteral(
                "选择圆心和圆周点，创建直径标注");
        case CadView::ToolMode::Angle:
            return QStringLiteral(
                "依次选择顶点和两条边，创建角度标注");
        case CadView::ToolMode::ArcLength:
            return QStringLiteral(
                "依次选择起点、弧上点和终点，创建弧长标注");
        case CadView::ToolMode::Coordinate:
            return QStringLiteral(
                "单击目标位置，创建 X/Y 坐标标注");
        case CadView::ToolMode::PointToLine:
            return QStringLiteral(
                "先选择点，再选择直线，标注垂直距离");
        case CadView::ToolMode::FixedDistance:
            return QStringLiteral(
                "选择起点和方向，创建指定长度的定距标注");
        case CadView::ToolMode::Text:
            return QStringLiteral(
                "单击图纸位置，添加文字注释");
        case CadView::ToolMode::PrintArea:
            return QStringLiteral(
                "框选需要打印的图纸范围");
        case CadView::ToolMode::Calibrate:
            return QStringLiteral(
                "选择已知长度的两个端点，校准图纸比例");
        case CadView::ToolMode::ZoomWindow:
            return QStringLiteral(
                "拖出矩形区域并缩放到该区域");
        }
        return QStringLiteral("选择测量工具");
    };
    for (QAction *action : m_toolGroup->actions()) {
        const QString description = toolHelp(
            static_cast<CadView::ToolMode>(
                action->data().toInt()));
        const QString shortcut =
            action->shortcut().toString(QKeySequence::NativeText);
        const QString help =
            shortcut.isEmpty()
                ? description
                : description
                    + QStringLiteral("  [%1]").arg(shortcut);
        action->setToolTip(help);
        action->setStatusTip(description);
    }
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
    const auto actionForMode =
        [this](CadView::ToolMode mode) -> QAction * {
            for (QAction *action : m_toolGroup->actions()) {
                if (action->data().toInt() == int(mode)) {
                    return action;
                }
            }
            return nullptr;
        };
    for (QAction *action : m_toolGroup->actions()) {
        if (action != m_selectAction && action != m_zoomWindowAction
            && action != m_eraseAction) {
            toolbar->addAction(action);
        }
    }
    toolbar->addAction(m_eraseAction);
    toolbar->addAction(m_deleteAllAction);
    toolbar->addSeparator();
    toolbar->addAction(m_snapEnabledAction);
    toolbar->addAction(m_orthoAction);
    toolbar->addAction(m_gridAction);
    toolbar->addAction(m_annotationVisibilityAction);
    toolbar->addWidget(new QLabel(QStringLiteral("    ")));

    m_measureToolbar = new QToolBar(QStringLiteral("测量工具"), this);
    m_measureToolbar->setObjectName(QStringLiteral("measureToolbar"));
    m_measureToolbar->setAllowedAreas(
        Qt::LeftToolBarArea | Qt::RightToolBarArea);
    m_measureToolbar->setMovable(true);
    m_measureToolbar->setFloatable(false);
    m_measureToolbar->setIconSize(QSize(22, 22));
    m_measureToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_measureToolbar->addAction(m_selectAction);
    m_measureToolbar->addSeparator();
    const auto addMeasureModes =
        [this, &actionForMode](
            std::initializer_list<CadView::ToolMode> modes) {
            for (CadView::ToolMode mode : modes) {
                if (QAction *action = actionForMode(mode)) {
                    m_measureToolbar->addAction(action);
                }
            }
        };
    addMeasureModes({
        CadView::ToolMode::Aligned,
        CadView::ToolMode::Linear,
        CadView::ToolMode::Continuous,
        CadView::ToolMode::Batch
    });
    m_measureToolbar->addSeparator();
    addMeasureModes({
        CadView::ToolMode::Rectangle,
        CadView::ToolMode::Polygon,
        CadView::ToolMode::CircleArea,
        CadView::ToolMode::FilledArea,
        CadView::ToolMode::SideArea,
        CadView::ToolMode::AreaOffset
    });
    m_measureToolbar->addSeparator();
    addMeasureModes({
        CadView::ToolMode::Radius,
        CadView::ToolMode::Diameter,
        CadView::ToolMode::Angle,
        CadView::ToolMode::ArcLength
    });
    m_measureToolbar->addSeparator();
    addMeasureModes({
        CadView::ToolMode::Coordinate,
        CadView::ToolMode::PointToLine,
        CadView::ToolMode::FixedDistance,
        CadView::ToolMode::Text,
        CadView::ToolMode::Calibrate,
        CadView::ToolMode::ZoomWindow
    });
    m_measureToolbar->addSeparator();
    if (m_toolGroup) {
        for (QAction *action : m_toolGroup->actions()) {
            const int mode = action->data().toInt();
            if (mode == int(CadView::ToolMode::Aligned)
                || mode == int(CadView::ToolMode::Linear)
                || mode == int(CadView::ToolMode::Continuous)
                || mode == int(CadView::ToolMode::Batch)
                || mode == int(CadView::ToolMode::Rectangle)
                || mode == int(CadView::ToolMode::Polygon)
                || mode == int(CadView::ToolMode::CircleArea)
                || mode == int(CadView::ToolMode::FilledArea)
                || mode == int(CadView::ToolMode::SideArea)
                || mode == int(CadView::ToolMode::AreaOffset)
                || mode == int(CadView::ToolMode::Radius)
                || mode == int(CadView::ToolMode::Diameter)
                || mode == int(CadView::ToolMode::Angle)
                || mode == int(CadView::ToolMode::ArcLength)
                || mode == int(CadView::ToolMode::Coordinate)
                || mode == int(CadView::ToolMode::PointToLine)
                || mode == int(CadView::ToolMode::FixedDistance)
                || mode == int(CadView::ToolMode::Text)
                || mode == int(CadView::ToolMode::Calibrate)
                || mode == int(CadView::ToolMode::ZoomWindow)
                || mode == int(CadView::ToolMode::Select)
                || mode == int(CadView::ToolMode::Erase)) {
                continue;
            }
            m_measureToolbar->addAction(action);
        }
    }
    m_measureToolbar->addAction(m_eraseAction);
    m_measureToolbar->addAction(m_deleteAllAction);
    m_measureToolbar->setEnabled(false);
    const QSettings interfaceSettings = applicationSettings();
    const Qt::ToolBarArea measureToolbarArea =
        interfaceSettings
                .value(QStringLiteral("layout/measureToolbarArea"),
                       QStringLiteral("left"))
                .toString()
            == QStringLiteral("right")
        ? Qt::RightToolBarArea : Qt::LeftToolBarArea;
    addToolBar(measureToolbarArea, m_measureToolbar);
    m_measureToolbarAction = m_measureToolbar->toggleViewAction();
    m_measureToolbarAction->setText(QStringLiteral("测量工具"));
    m_measureToolbar->setVisible(
        interfaceSettings
            .value(QStringLiteral("layout/measureToolbarVisible"), false)
            .toBool());
    connect(m_measureToolbar, &QToolBar::visibilityChanged, this,
            [](bool visible) {
                QSettings settings = applicationSettings();
                settings.setValue(
                    QStringLiteral("layout/measureToolbarVisible"), visible);
                settings.sync();
            });

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
    auto *canvasLayout = new QGridLayout(canvasFrame);
    canvasLayout->setContentsMargins(1, 1, 1, 1);
    canvasLayout->setSpacing(0);
    m_documentStack = new QStackedWidget;
    m_documentStack->addWidget(m_view);
    canvasLayout->addWidget(m_documentStack, 0, 0);
    m_layoutTabs = new QTabBar;
    m_layoutTabs->setObjectName(QStringLiteral("layoutTabs"));
    m_layoutTabs->setDrawBase(false);
    m_layoutTabs->setExpanding(false);
    m_layoutTabs->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_layoutTabs->setMovable(false);
    m_layoutTabs->setContentsMargins(8, 4, 8, 4);
    m_layoutTabs->setAutoFillBackground(false);
    m_layoutTabs->setAttribute(Qt::WA_TranslucentBackground, true);
    canvasLayout->addWidget(
        m_layoutTabs, 0, 0, Qt::AlignBottom | Qt::AlignHCenter);
    m_layoutTabs->raise();
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
    m_startScreen->setMotionEnabled(
        interfaceSettings
            .value(QStringLiteral("startup/motionEnabled"), true)
            .toBool());
    connect(m_startScreen, &StartScreen::browseRequested, this,
            [this] { m_openAction->trigger(); });
    connect(m_startScreen, &StartScreen::openRequested, this,
            &MainWindow::openDrawing);
    connect(m_startScreen, &StartScreen::removeRecentRequested, this,
            &MainWindow::removeRecentDrawing);
    connect(m_startScreen, &StartScreen::motionEnabledChanged, this,
            [](bool enabled) {
                QSettings settings = applicationSettings();
                settings.setValue(
                    QStringLiteral("startup/motionEnabled"), enabled);
                settings.sync();
            });
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
                    setDrawingActionsEnabled(false);
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
    const auto actionForMode =
        [this](CadView::ToolMode mode) -> QAction * {
            for (QAction *action : m_toolGroup->actions()) {
                if (action->data().toInt() == int(mode)) {
                    return action;
                }
            }
            return nullptr;
        };
    const auto addMeasureSection =
        [measureMenu, &actionForMode](
            const QString &title,
            std::initializer_list<CadView::ToolMode> modes) {
            measureMenu->addSection(title);
            for (CadView::ToolMode mode : modes) {
                if (QAction *action = actionForMode(mode)) {
                    measureMenu->addAction(action);
                }
            }
        };
    addMeasureSection(
        QStringLiteral("尺寸与连续测量"),
        {
            CadView::ToolMode::Aligned,
            CadView::ToolMode::Linear,
            CadView::ToolMode::Continuous,
            CadView::ToolMode::Batch
        });
    addMeasureSection(
        QStringLiteral("面积测量"),
        {
            CadView::ToolMode::Rectangle,
            CadView::ToolMode::Polygon,
            CadView::ToolMode::CircleArea,
            CadView::ToolMode::FilledArea,
            CadView::ToolMode::SideArea,
            CadView::ToolMode::AreaOffset
        });
    addMeasureSection(
        QStringLiteral("半径、角度与弧长"),
        {
            CadView::ToolMode::Radius,
            CadView::ToolMode::Diameter,
            CadView::ToolMode::Angle,
            CadView::ToolMode::ArcLength
        });
    addMeasureSection(
        QStringLiteral("坐标与辅助工具"),
        {
            CadView::ToolMode::Coordinate,
            CadView::ToolMode::PointToLine,
            CadView::ToolMode::FixedDistance,
            CadView::ToolMode::Text,
            CadView::ToolMode::Calibrate
        });
    measureMenu->addSection(QStringLiteral("选择、视图与显示"));
    measureMenu->addAction(m_selectAction);
    measureMenu->addAction(m_eraseAction);
    measureMenu->addAction(m_deleteAllAction);
    measureMenu->addAction(m_zoomWindowAction);
    measureMenu->addSeparator();
    measureMenu->addAction(m_annotationVisibilityAction);
    measureMenu->addAction(m_annotationSettingsAction);
    if (m_measureToolbarAction) {
        measureMenu->addAction(m_measureToolbarAction);
    }

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    viewMenu->addAction(m_fitAction);
    viewMenu->addAction(m_zoomWindowAction);
    auto *themeMenu = viewMenu->addMenu(QStringLiteral("主题"));
    auto *themeGroup = new QActionGroup(this);
    themeGroup->setExclusive(true);
    m_darkThemeAction = themeMenu->addAction(
        themedIcon(QStringLiteral("theme-dark")),
        QStringLiteral("CAD 深色"));
    m_lightThemeAction = themeMenu->addAction(
        themedIcon(QStringLiteral("theme-light")),
        QStringLiteral("CAD 浅色"));
    m_grayThemeAction = themeMenu->addAction(
        themedIcon(QStringLiteral("theme-dark")),
        QStringLiteral("CAD 深灰"));
    m_blueprintThemeAction = themeMenu->addAction(
        themedIcon(QStringLiteral("theme-dark")),
        QStringLiteral("经典蓝图"));
    m_darkThemeAction->setProperty("cadIconName",
                                   QStringLiteral("theme-dark"));
    m_lightThemeAction->setProperty("cadIconName",
                                    QStringLiteral("theme-light"));
    m_grayThemeAction->setProperty("cadIconName",
                                   QStringLiteral("theme-dark"));
    m_blueprintThemeAction->setProperty("cadIconName",
                                        QStringLiteral("theme-dark"));
    m_darkThemeAction->setCheckable(true);
    m_lightThemeAction->setCheckable(true);
    m_grayThemeAction->setCheckable(true);
    m_blueprintThemeAction->setCheckable(true);
    themeGroup->addAction(m_darkThemeAction);
    themeGroup->addAction(m_lightThemeAction);
    themeGroup->addAction(m_grayThemeAction);
    themeGroup->addAction(m_blueprintThemeAction);
    (m_cadTheme == CadTheme::Gray
         ? m_grayThemeAction
     : m_cadTheme == CadTheme::Blueprint
         ? m_blueprintThemeAction
     : m_cadTheme == CadTheme::Light
         ? m_lightThemeAction : m_darkThemeAction)->setChecked(true);
    connect(m_darkThemeAction, &QAction::triggered, this, [this] {
        setCadTheme(CadTheme::Dark);
    });
    connect(m_lightThemeAction, &QAction::triggered, this, [this] {
        setCadTheme(CadTheme::Light);
    });
    connect(m_grayThemeAction, &QAction::triggered, this, [this] {
        setCadTheme(CadTheme::Gray);
    });
    connect(m_blueprintThemeAction, &QAction::triggered, this, [this] {
        setCadTheme(CadTheme::Blueprint);
    });
    viewMenu->addSeparator();
    viewMenu->addAction(m_layerPanelAction);
    viewMenu->addAction(m_annotationManagerAction);
    viewMenu->addAction(m_measureToolbarAction);
    auto *measureDockMenu =
        viewMenu->addMenu(QStringLiteral("测量工具停靠位置"));
    auto *measureDockGroup = new QActionGroup(this);
    measureDockGroup->setExclusive(true);
    QAction *dockLeft = measureDockMenu->addAction(
        QStringLiteral("停靠左侧"));
    QAction *dockRight = measureDockMenu->addAction(
        QStringLiteral("停靠右侧"));
    dockLeft->setCheckable(true);
    dockRight->setCheckable(true);
    measureDockGroup->addAction(dockLeft);
    measureDockGroup->addAction(dockRight);
    const bool measureToolbarOnLeft =
        m_measureToolbar
        && toolBarArea(m_measureToolbar) == Qt::LeftToolBarArea;
    (measureToolbarOnLeft ? dockLeft : dockRight)->setChecked(true);
    connect(dockLeft, &QAction::triggered, this, [this] {
        dockMeasureToolbar(Qt::LeftToolBarArea);
    });
    connect(dockRight, &QAction::triggered, this, [this] {
        dockMeasureToolbar(Qt::RightToolBarArea);
    });
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
            QToolBar#mainToolbar, QToolBar#measureToolbar {
                background: #e4eaee; border: none;
                border-bottom: 1px solid #b8c7cf;
                spacing: 2px; padding: 6px 8px;
            }
            QToolBar#mainToolbar::separator,
            QToolBar#measureToolbar::separator {
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
                background: #dfe7eb; border: none;
                border-bottom: 1px solid #bccbd2;
                spacing: 3px; padding: 3px 6px 0 6px;
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
                background: transparent; border: none;
                spacing: 3px; padding: 2px 8px;
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
            QPushButton { border-radius: 5px; padding: 7px 13px; }
            QPushButton:focus { border: 1px solid #287da6; }
            QToolButton { border-radius: 4px; }
            QComboBox { border-radius: 4px; min-height: 24px; }
            QTreeWidget, QTableWidget {
                border-radius: 6px;
            }
            QTreeWidget::item, QTableWidget::item {
                padding: 6px; border-radius: 3px;
            }
            QTreeWidget::item:selected, QTableWidget::item:selected {
                background: #d8eaf4; color: #12384b;
            }
            QTreeWidget::item:hover, QTableWidget::item:hover {
                background: #edf5f9;
            }
            QFrame#sidePanel {
                border: 1px solid #d5dde2; border-radius: 6px;
            }
        )"));
        if (m_startScreen) {
            m_startScreen->setCadTheme(false);
        }
        return;
    }
    QString themeStyleSheet = QStringLiteral(R"(
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
        QToolBar#mainToolbar, QToolBar#measureToolbar {
            background: #1b252c; border: none;
            border-bottom: 1px solid #3a4c57;
            spacing: 2px; padding: 6px 8px;
        }
        QToolBar#mainToolbar::separator,
        QToolBar#measureToolbar::separator {
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
            background: #121b21; border: none;
            border-bottom: 1px solid #2d3d47;
            spacing: 3px; padding: 3px 6px 0 6px;
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
            spacing: 3px; padding: 2px 8px;
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
        QPushButton { border-radius: 5px; padding: 7px 13px; }
        QPushButton:focus { border: 1px solid #62b4dd; }
        QToolButton { border-radius: 4px; }
        QComboBox { border-radius: 4px; min-height: 24px; }
        QTreeWidget, QTableWidget { border-radius: 6px; }
        QTreeWidget::item, QTableWidget::item {
            padding: 6px; border-radius: 3px;
        }
        QTreeWidget::item:selected, QTableWidget::item:selected {
            background: #263f50; color: #f0f7fa;
        }
        QTreeWidget::item:hover, QTableWidget::item:hover {
            background: #1a2730;
        }
        QFrame#sidePanel {
            border: 1px solid #28343b; border-radius: 6px;
        }
    )");
    const QString grayInterface = QStringLiteral(R"(
        QMainWindow { background: #252b30; color: #d8e0e5; }
        QMenuBar {
            background: #292f34; color: #cbd5dc;
            border-bottom-color: #46515a;
        }
        QMenuBar::item:selected { background: #3a444c; }
        QMenu {
            background: #30373d; color: #e2e8ec;
            border-color: #56636d;
        }
        QMenu::item:selected {
            background: #3e5361; border-left-color: #7dbad7;
        }
        QMenu::separator { background: #4a555e; }
        QToolBar#mainToolbar, QToolBar#measureToolbar {
            background: #303940; border-bottom-color: #566670;
        }
        QToolBar#mainToolbar::separator,
        QToolBar#measureToolbar::separator { background: #71808a; }
        QToolButton { color: #d4dde3; }
        QToolButton:hover {
            background: #3d474f; border-color: #61717c;
        }
        QToolButton:checked {
            background: #3a5362; border-color: #81c4e2;
        }
        QToolButton#styleChoice {
            background: #252b30; border-color: #505d66;
            color: #d4dde3;
        }
        QToolButton#styleChoice:hover {
            background: #333d44; border-color: #7caac0;
        }
        QToolButton#styleChoice:checked {
            background: #294937; border-color: #61d68c;
            color: #effbf3;
        }
        QFrame#sidePanel { background: #292f34; border-color: #4a555d; }
        QFrame#canvasFrame {
            background: #2b3034; border-color: #59636a;
        }
        QDockWidget { background: #292f34; color: #d8e0e5; }
        QDockWidget::title {
            background: #30373d; border-bottom-color: #4b5760;
            color: #d8e0e5;
        }
        QLabel#panelTitle, QLabel#documentTitle { color: #f0f4f6; }
        QLabel#documentMeta { color: #a2afb7; }
        QLabel#hintLabel { color: #9ca9b1; }
        QLabel#summaryLabel {
            background: #252b30; border-color: #4a565e;
            color: #b9c4ca;
        }
        QTreeWidget, QTableWidget {
            background: #252b30; alternate-background-color: #2b3237;
            border-color: #505d66; color: #d7e0e5;
            selection-background-color: #3a5868; gridline-color: #414b52;
        }
        QHeaderView::section {
            background: #30383e; color: #b9c5cc;
            border-bottom-color: #505d66;
        }
        QComboBox {
            background: #30373d; border-color: #59666f;
            color: #e0e7eb;
        }
        QComboBox QAbstractItemView {
            background: #30373d; color: #e0e7eb;
            selection-background-color: #3d5564;
        }
        QPushButton {
            background: #313940; border-color: #596771;
            color: #dce4e8;
        }
        QPushButton:hover {
            background: #3d4951; border-color: #82b6cf;
        }
        QStatusBar {
            background: #30373d; color: #d6dee3;
            border-top-color: #53606a;
        }
        QCheckBox { color: #d1dae0; }
        QSplitter::handle { background: #4a555d; }
        QScrollBar:vertical, QScrollBar:horizontal { background: #252b30; }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: #56656f;
        }
        QProgressBar { background: #414a51; }
        QProgressBar::chunk { background: #7dbad7; }
        QTabBar#documentTabs {
            background: #303940; border-bottom-color: #566771;
        }
        QTabBar#layoutTabs {
            background: transparent; border: none;
        }
        QTabBar#documentTabs::tab, QTabBar#layoutTabs::tab {
            background: #252b30; color: #aab7bf;
            border-color: #505c64;
        }
        QTabBar#documentTabs::tab:hover, QTabBar#layoutTabs::tab:hover {
            background: #37434a; color: #eff4f6;
            border-color: #73838d;
        }
        QTabBar#documentTabs::tab:selected,
        QTabBar#layoutTabs::tab:selected {
            background: #3b4650; color: #ffffff;
            border-color: #7993a3;
        }
        QTreeWidget::item:selected, QTableWidget::item:selected {
            background: #3a5868; color: #ffffff;
        }
        QTreeWidget::item:hover, QTableWidget::item:hover {
            background: #333c42;
        }
    )");
    const QString blueprintInterface = QStringLiteral(R"(
        QMainWindow { background: #0d263a; color: #d8ecf7; }
        QMenuBar {
            background: #112e45; color: #c7e5f4;
            border-bottom-color: #28516c;
        }
        QMenuBar::item:selected { background: #244b64; }
        QMenu {
            background: #14344b; color: #e0f2fb;
            border-color: #37627c;
        }
        QMenu::item:selected {
            background: #24526e; border-left-color: #67d9f2;
        }
        QMenu::separator { background: #31586f; }
        QToolBar#mainToolbar, QToolBar#measureToolbar {
            background: #14364d; border-bottom-color: #38627b;
        }
        QToolBar#mainToolbar::separator,
        QToolBar#measureToolbar::separator { background: #52788e; }
        QToolButton { color: #d4eaf5; }
        QToolButton:hover {
            background: #214a63; border-color: #4f7890;
        }
        QToolButton:checked {
            background: #245a76; border-color: #72ddf1;
        }
        QToolButton#styleChoice {
            background: #102c40; border-color: #365b72;
            color: #cfe8f4;
        }
        QToolButton#styleChoice:hover {
            background: #1c4057; border-color: #5ca8c5;
        }
        QToolButton#styleChoice:checked {
            background: #194b55; border-color: #58d9cf;
            color: #efffff;
        }
        QFrame#sidePanel { background: #102b3e; border-color: #31566e; }
        QFrame#canvasFrame {
            background: #102f4a; border-color: #3e6b86;
        }
        QDockWidget { background: #102b3e; color: #d7ebf6; }
        QDockWidget::title {
            background: #17384f; border-bottom-color: #31566e;
            color: #d7ebf6;
        }
        QLabel#panelTitle, QLabel#documentTitle { color: #eefaff; }
        QLabel#documentMeta { color: #8fb4c9; }
        QLabel#hintLabel { color: #8eafc2; }
        QLabel#summaryLabel {
            background: #0c2436; border-color: #31566c;
            color: #b2cfdf;
        }
        QTreeWidget, QTableWidget {
            background: #0d273a; alternate-background-color: #123047;
            border-color: #31576e; color: #d4e9f4;
            selection-background-color: #245a76; gridline-color: #25465a;
        }
        QHeaderView::section {
            background: #17384e; color: #b6d5e5;
            border-bottom-color: #31566e;
        }
        QComboBox {
            background: #17384e; border-color: #3a6279;
            color: #e0f1fa;
        }
        QComboBox QAbstractItemView {
            background: #14344b; color: #e0f1fa;
            selection-background-color: #245a76;
        }
        QPushButton {
            background: #173a51; border-color: #3d657d;
            color: #dbedf7;
        }
        QPushButton:hover {
            background: #23516a; border-color: #65b7d3;
        }
        QStatusBar {
            background: #17384e; color: #d3e8f3;
            border-top-color: #37637b;
        }
        QCheckBox { color: #cce4f0; }
        QSplitter::handle { background: #31566c; }
        QScrollBar:vertical, QScrollBar:horizontal { background: #0d2638; }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: #3b667e;
        }
        QProgressBar { background: #28495e; }
        QProgressBar::chunk { background: #65cce8; }
        QTabBar#documentTabs {
            background: #14364c; border-bottom-color: #38627b;
        }
        QTabBar#layoutTabs {
            background: transparent; border: none;
        }
        QTabBar#documentTabs::tab, QTabBar#layoutTabs::tab {
            background: #0e293c; color: #99bdcf;
            border-color: #31546a;
        }
        QTabBar#documentTabs::tab:hover, QTabBar#layoutTabs::tab:hover {
            background: #1b4259; color: #effbff;
            border-color: #5388a1;
        }
        QTabBar#documentTabs::tab:selected,
        QTabBar#layoutTabs::tab:selected {
            background: #23516a; color: #ffffff;
            border-color: #66b9d4;
        }
        QTreeWidget::item:selected, QTableWidget::item:selected {
            background: #245a76; color: #ffffff;
        }
        QTreeWidget::item:hover, QTableWidget::item:hover {
            background: #183b51;
        }
    )");
    if (m_cadTheme == CadTheme::Gray) {
        themeStyleSheet += grayInterface;
    } else if (m_cadTheme == CadTheme::Blueprint) {
        themeStyleSheet += blueprintInterface;
    }
    setStyleSheet(themeStyleSheet);
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
        QSettings settings = applicationSettings();
        const QString themeId =
            theme == CadTheme::Light
                ? QStringLiteral("cadLight")
            : theme == CadTheme::Gray
                ? QStringLiteral("cadGray")
            : theme == CadTheme::Blueprint
                ? QStringLiteral("cadBlueprint")
                : QStringLiteral("cadDark");
        settings.setValue(
            QStringLiteral("uiTheme"), themeId);
        settings.sync();
    }
    applyStyle();
    for (const DocumentSession &session : std::as_const(m_documentSessions)) {
        if (session.view) {
            session.view->setDrawingTheme(
                theme == CadTheme::Light
                    ? CadView::DrawingTheme::CadLight
                : theme == CadTheme::Gray
                    ? CadView::DrawingTheme::CadGray
                : theme == CadTheme::Blueprint
                    ? CadView::DrawingTheme::CadBlueprint
                    : CadView::DrawingTheme::CadDark);
        }
    }
    if (m_view) {
        m_view->setDrawingTheme(
            theme == CadTheme::Light
                ? CadView::DrawingTheme::CadLight
            : theme == CadTheme::Gray
                ? CadView::DrawingTheme::CadGray
            : theme == CadTheme::Blueprint
                ? CadView::DrawingTheme::CadBlueprint
                : CadView::DrawingTheme::CadDark);
    }
    if (m_layerTree && m_view && m_view->document()) {
        populateLayers();
    }
    if (m_darkThemeAction && m_lightThemeAction
        && m_grayThemeAction && m_blueprintThemeAction) {
        (theme == CadTheme::Light
             ? m_lightThemeAction
         : theme == CadTheme::Gray
             ? m_grayThemeAction
         : theme == CadTheme::Blueprint
             ? m_blueprintThemeAction
             : m_darkThemeAction)->setChecked(true);
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
    if (!m_loadWatcher || !m_loadWatcher->isFinished()
        || m_loadWatcher->future().resultCount() == 0) {
        return;
    }
    m_loading = false;
    m_openAction->setEnabled(true);
    m_progress->hide();
    DrawingLoadResult result = m_loadWatcher->result();
    if (!result.document) {
        showLoadError(result.error);
        return;
    }
    setDrawingActionsEnabled(true);
    if (m_activeDocumentIndex >= 0) {
        auto *newView = new CadView;
        wireViewSignals(newView);
        m_documentStack->addWidget(newView);
        m_view = newView;
        m_view->setSnapEnabled(m_snapEnabledAction->isChecked());
        m_view->setOrthoEnabled(m_orthoAction->isChecked());
    }
    m_view->setGridVisible(m_gridAction->isChecked());
    m_view->setDocument(result.document, result.modelIndex,
                        std::move(result.layoutContents));
    DisplaySettings settings;
    settings.lengthUnit = result.document->sourceUnit;
    settings.areaUnit = MeasuringUnit::Meter;
    settings.coordinateUnit = result.document->sourceUnit;
    loadApplicationDisplaySettings(settings);
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
        m_annotationVisibilityAction->setIcon(themedIcon(icon));
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
    QTimer::singleShot(0, this, [this] {
        if (!m_loading && m_loadWatcher
            && m_loadWatcher->isFinished()) {
            const QSignalBlocker blocker(m_loadWatcher);
            m_loadWatcher->setFuture(QFuture<DrawingLoadResult>());
        }
    });
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
        setDrawingActionsEnabled(true);
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
    setDrawingActionsEnabled(true);
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
        setDrawingActionsEnabled(false);
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
        setDrawingActionsEnabled(false);
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
    setDrawingActionsEnabled(false);
}

void MainWindow::setDrawingActionsEnabled(bool enabled)
{
    if (m_mainToolbar) {
        m_mainToolbar->setEnabled(enabled);
    }
    if (m_measureToolbar) {
        m_measureToolbar->setEnabled(enabled);
    }
}

void MainWindow::dockMeasureToolbar(Qt::ToolBarArea area)
{
    if (!m_measureToolbar
        || (area != Qt::LeftToolBarArea
            && area != Qt::RightToolBarArea)) {
        return;
    }
    addToolBar(area, m_measureToolbar);
    m_measureToolbar->show();
    if (m_measureToolbarAction) {
        m_measureToolbarAction->setChecked(true);
    }
    QSettings settings = applicationSettings();
    settings.setValue(
        QStringLiteral("layout/measureToolbarArea"),
        area == Qt::LeftToolBarArea
            ? QStringLiteral("left") : QStringLiteral("right"));
    settings.setValue(
        QStringLiteral("layout/measureToolbarVisible"), true);
    settings.sync();
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
            QPixmap swatch(16, 16);
            swatch.fill(Qt::transparent);
            {
                QPainter painter(&swatch);
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setBrush(layerColor);
                painter.setPen(QPen(
                    m_cadTheme == CadTheme::Light
                        ? QColor(41, 58, 67, 210)
                        : QColor(232, 240, 244, 210),
                    1.0));
                painter.drawRoundedRect(
                    QRectF(1.5, 1.5, 13.0, 13.0), 3.0, 3.0);
            }
            item->setIcon(0, QIcon(swatch));
            item->setText(
                0, layer.name
                       + (layer.locked ? QStringLiteral("  [锁定]")
                                       : QString()));
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
    dialog.setWindowTitle(QStringLiteral("标注显示与测量设置"));
    dialog.setObjectName(QStringLiteral("annotationSettingsDialog"));
    dialog.setStyleSheet(QStringLiteral(R"(
        QDialog#annotationSettingsDialog {
            background: qlineargradient(
                x1:0, y1:0, x2:1, y2:1,
                stop:0 #f8fafb, stop:0.52 #f2f5f7, stop:1 #e7edf1);
            color: #17232b;
        }
        QDialog#annotationSettingsDialog QLabel {
            background: transparent; color: #24343e;
        }
        QLabel#dialogTitle {
            color: #123746; font-size: 20px; font-weight: 700;
        }
        QLabel#dialogSubtitle {
            color: #61747d; font-size: 11px;
        }
        QFrame#annotationIntroCard {
            background: qlineargradient(
                x1:0, y1:0, x2:1, y2:0,
                stop:0 #ffffff, stop:1 #eaf3f7);
            border: 1px solid #cddbe2; border-left: 3px solid #2b8cad;
            border-radius: 10px;
        }
        QFrame#annotationSettingsCard {
            background: #ffffff;
            border: 1px solid #d3dde3;
            border-radius: 10px;
        }
        QLabel#sectionTitle {
            color: #153b4c; font-size: 13px; font-weight: 700;
        }
        QLabel#sectionSubtitle {
            color: #6b7d86; font-size: 10px;
        }
        QLabel#fieldLabel {
            color: #354a55; font-size: 11px; font-weight: 600;
        }
        QLabel#unitColumnHeader {
            color: #7b8b93; font-size: 10px; font-weight: 600;
        }
        QLabel#settingsBadge {
            color: #176b4b; background: #e6f6ec;
            border: 1px solid #bfe5cc; border-radius: 9px;
            padding: 2px 8px; font-size: 10px; font-weight: 600;
        }
        QDialog#annotationSettingsDialog QComboBox,
        QDialog#annotationSettingsDialog QSpinBox,
        QDialog#annotationSettingsDialog QDoubleSpinBox {
            background: #ffffff; color: #17232b;
            border: 1px solid #b8c7cf; border-radius: 6px;
            padding: 5px 8px; min-height: 25px;
        }
        QDialog#annotationSettingsDialog QComboBox:hover,
        QDialog#annotationSettingsDialog QSpinBox:hover,
        QDialog#annotationSettingsDialog QDoubleSpinBox:hover {
            border-color: #5b9fc0;
        }
        QDialog#annotationSettingsDialog QComboBox QAbstractItemView {
            background: #ffffff; color: #17232b;
            selection-background-color: #dceffd;
            selection-color: #102d3d;
        }
        QDialog#annotationSettingsDialog QPushButton {
            background: #ffffff; color: #1d2b34;
            border: 1px solid #aebdc5; border-radius: 6px;
            padding: 7px 13px;
        }
        QDialog#annotationSettingsDialog QPushButton:hover {
            background: #eef6fb; border-color: #5a9fc4;
        }
        QDialog#annotationSettingsDialog QPushButton#colorPreviewButton {
            min-height: 34px; font-weight: 700;
            border: 2px solid rgba(255, 255, 255, 170);
            border-radius: 7px;
        }
        QDialog#annotationSettingsDialog QToolButton#styleChoice {
            background: #ffffff; border: 1px solid #b8c5cd;
            border-radius: 9px; color: #2e414c; padding: 7px;
        }
        QDialog#annotationSettingsDialog QToolButton#styleChoice:hover {
            background: #eef6fb; border-color: #5a9fc4;
        }
        QDialog#annotationSettingsDialog QToolButton#styleChoice:checked {
            background: #e6f7ed; border: 2px solid #1b9e57;
            color: #145d36;
        }
        QLabel#crosshairPreview {
            background: #10181e; border: 1px solid #9aaab3;
            border-radius: 7px;
        }
        QLabel#settingsFooterHint {
            color: #4f6772; background: #edf5f8;
            border: 1px solid #d0e1e8; border-radius: 7px;
            padding: 9px 12px;
        }
        QDialog#annotationSettingsDialog QDialogButtonBox QPushButton {
            min-width: 88px; padding: 7px 18px;
        }
        QDialog#annotationSettingsDialog QDialogButtonBox QPushButton:default {
            background: #1d718f; color: #ffffff;
            border-color: #155d78;
        }
    )"));
    dialog.resize(1020, 700);
    dialog.setMinimumSize(900, 620);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 18, 22, 16);
    layout->setSpacing(11);

    auto *introCard = new QFrame;
    introCard->setObjectName(QStringLiteral("annotationIntroCard"));
    auto *introLayout = new QHBoxLayout(introCard);
    introLayout->setContentsMargins(14, 10, 14, 10);
    introLayout->setSpacing(12);
    auto *dialogIcon = new QLabel;
    dialogIcon->setFixedSize(40, 40);
    dialogIcon->setPixmap(
        themedIcon(QStringLiteral("settings")).pixmap(36, 36));
    dialogIcon->setAlignment(Qt::AlignCenter);
    auto *dialogTitles = new QVBoxLayout;
    dialogTitles->setSpacing(2);
    auto *dialogTitle = new QLabel(QStringLiteral("标注显示与测量设置"));
    dialogTitle->setObjectName(QStringLiteral("dialogTitle"));
    auto *dialogSubtitle = new QLabel(
        QStringLiteral("按用途分组调整标注样式、测量单位、颜色和十字光标"));
    dialogSubtitle->setObjectName(QStringLiteral("dialogSubtitle"));
    dialogTitles->addWidget(dialogTitle);
    dialogTitles->addWidget(dialogSubtitle);
    introLayout->addWidget(dialogIcon);
    introLayout->addLayout(dialogTitles, 1);
    layout->addWidget(introCard);

    const auto addSectionHeader =
        [](QVBoxLayout *cardLayout, const QString &title,
           const QString &subtitle) -> QHBoxLayout * {
            auto *header = new QHBoxLayout;
            header->setSpacing(8);
            auto *titles = new QVBoxLayout;
            titles->setSpacing(1);
            auto *titleLabel = new QLabel(title);
            titleLabel->setObjectName(QStringLiteral("sectionTitle"));
            auto *subtitleLabel = new QLabel(subtitle);
            subtitleLabel->setObjectName(QStringLiteral("sectionSubtitle"));
            titles->addWidget(titleLabel);
            titles->addWidget(subtitleLabel);
            header->addLayout(titles, 1);
            cardLayout->addLayout(header);
            return header;
        };

    const auto makeUnitCombo = [](MeasuringUnit selected, bool area) {
        auto *combo = new QComboBox;
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
        spin->setFixedWidth(76);
        return spin;
    };
    auto *lengthPrecision = makePrecision(settings.lengthPrecision);
    auto *areaPrecision = makePrecision(settings.areaPrecision);
    auto *coordinatePrecision = makePrecision(settings.coordinatePrecision);

    auto *stylePanel = new QFrame;
    stylePanel->setObjectName(QStringLiteral("annotationSettingsCard"));
    auto *styleCardLayout = new QVBoxLayout(stylePanel);
    styleCardLayout->setContentsMargins(14, 12, 14, 14);
    styleCardLayout->setSpacing(9);
    QHBoxLayout *styleHeader = addSectionHeader(
        styleCardLayout, QStringLiteral("标注样式"),
        QStringLiteral("端点符号和尺寸线会随所选 CAD 样式变化"));
    auto *styleBadge = new QLabel(QStringLiteral("6 种常用样式"));
    styleBadge->setObjectName(QStringLiteral("settingsBadge"));
    styleHeader->addWidget(styleBadge);
    auto *styleGrid = new QGridLayout;
    styleGrid->setContentsMargins(0, 1, 0, 0);
    styleGrid->setHorizontalSpacing(9);
    styleGrid->setVerticalSpacing(9);
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
        button->setIconSize(QSize(78, 34));
        button->setMinimumSize(108, 64);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
        styleGrid->addWidget(button, 0, i);
    }
    for (int column = 0; column < styleLabels.size(); ++column) {
        styleGrid->setColumnStretch(column, 1);
    }
    styleCardLayout->addLayout(styleGrid);
    const int selectedStyle = qBound(
        0, int(settings.annotationStyle), styleLabels.size() - 1);
    styleGroup->button(selectedStyle)->setChecked(true);
    layout->addWidget(stylePanel);

    auto *settingsRow = new QGridLayout;
    settingsRow->setSpacing(13);
    settingsRow->setHorizontalSpacing(13);
    settingsRow->setVerticalSpacing(0);

    auto *unitCard = new QFrame;
    unitCard->setObjectName(QStringLiteral("annotationSettingsCard"));
    auto *unitCardLayout = new QVBoxLayout(unitCard);
    unitCardLayout->setContentsMargins(16, 14, 16, 15);
    unitCardLayout->setSpacing(11);
    addSectionHeader(
        unitCardLayout, QStringLiteral("单位与精度"),
        QStringLiteral("控制长度、面积和坐标值的显示方式"));
    auto *unitGrid = new QGridLayout;
    unitGrid->setContentsMargins(0, 1, 0, 0);
    unitGrid->setHorizontalSpacing(9);
    unitGrid->setVerticalSpacing(8);
    unitGrid->setColumnStretch(1, 1);
    const QStringList unitHeaders = {
        QStringLiteral("测量项目"), QStringLiteral("单位"),
        QStringLiteral("小数位")
    };
    for (int column = 0; column < unitHeaders.size(); ++column) {
        auto *header = new QLabel(unitHeaders[column]);
        header->setObjectName(QStringLiteral("unitColumnHeader"));
        unitGrid->addWidget(header, 0, column);
    }
    const auto addUnitRow =
        [unitGrid](int row, const QString &label, QComboBox *unit,
                   QSpinBox *precision) {
            auto *field = new QLabel(label);
            field->setObjectName(QStringLiteral("fieldLabel"));
            unitGrid->addWidget(field, row, 0);
            unitGrid->addWidget(unit, row, 1);
            unitGrid->addWidget(precision, row, 2);
        };
    addUnitRow(1, QStringLiteral("长度"), lengthUnit, lengthPrecision);
    addUnitRow(2, QStringLiteral("面积"), areaUnit, areaPrecision);
    addUnitRow(
        3, QStringLiteral("坐标"), coordinateUnit, coordinatePrecision);
    unitCardLayout->addLayout(unitGrid);
    auto *unitHint = new QLabel(
        QStringLiteral("建议施工图纸使用毫米，面积使用平方米。"));
    unitHint->setObjectName(QStringLiteral("sectionSubtitle"));
    unitHint->setWordWrap(true);
    unitCardLayout->addWidget(unitHint);
    unitCardLayout->addStretch();

    auto *colorCard = new QFrame;
    colorCard->setObjectName(QStringLiteral("annotationSettingsCard"));
    colorCard->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *colorCardLayout = new QVBoxLayout(colorCard);
    colorCardLayout->setContentsMargins(16, 14, 16, 15);
    colorCardLayout->setSpacing(12);
    addSectionHeader(
        colorCardLayout, QStringLiteral("标注颜色"),
        QStringLiteral("尺寸、面积和文字注释统一使用此颜色"));
    auto *colorButton = new QPushButton;
    colorButton->setObjectName(QStringLiteral("colorPreviewButton"));
    colorButton->setCursor(Qt::PointingHandCursor);
    colorButton->setMinimumHeight(42);
    QColor selectedColor = settings.annotationColor;
    const auto updateColorButton = [colorButton, &selectedColor] {
        const QString textColor =
            selectedColor.lightness() > 150
                ? QStringLiteral("#101820") : QStringLiteral("#f5f8fa");
        colorButton->setStyleSheet(
            QStringLiteral(
                "background:%1; color:%2; border-color:%3;")
                .arg(selectedColor.name(), textColor,
                     selectedColor.darker(135).name()));
        colorButton->setText(
            QStringLiteral("标注颜色   %1")
                .arg(selectedColor.name().toUpper()));
        colorButton->setToolTip(QStringLiteral("点击选择标注颜色"));
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
    colorCardLayout->addWidget(colorButton);
    colorCardLayout->addStretch();

    auto *crosshairCard = new QFrame;
    crosshairCard->setObjectName(QStringLiteral("annotationSettingsCard"));
    crosshairCard->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *crosshairCardLayout = new QVBoxLayout(crosshairCard);
    crosshairCardLayout->setContentsMargins(16, 14, 16, 15);
    crosshairCardLayout->setSpacing(12);
    auto *crosshairHeader = new QHBoxLayout;
    crosshairHeader->setSpacing(10);
    auto *crosshairTitles = new QVBoxLayout;
    crosshairTitles->setSpacing(1);
    auto *crosshairTitle = new QLabel(QStringLiteral("十字光标"));
    crosshairTitle->setObjectName(QStringLiteral("sectionTitle"));
    auto *crosshairSubtitle = new QLabel(
        QStringLiteral("颜色、屏幕长度和线宽"));
    crosshairSubtitle->setObjectName(QStringLiteral("sectionSubtitle"));
    crosshairTitles->addWidget(crosshairTitle);
    crosshairTitles->addWidget(crosshairSubtitle);
    auto *crosshairPreview = new QLabel;
    crosshairPreview->setObjectName(QStringLiteral("crosshairPreview"));
    crosshairPreview->setFixedSize(94, 94);
    crosshairPreview->setAlignment(Qt::AlignCenter);
    const QColor previewCanvasColor =
        m_view->canvasBackgroundColor();
    const QColor previewBorderColor =
        previewCanvasColor.lightness() > 128
            ? QColor(QStringLiteral("#8fa2ac"))
            : QColor(QStringLiteral("#8da4b3"));
    crosshairPreview->setStyleSheet(
        QStringLiteral(
            "QLabel#crosshairPreview { background:%1; "
            "border:1px solid %2; border-radius:7px; }")
            .arg(previewCanvasColor.name(),
                 previewBorderColor.name()));
    crosshairHeader->addLayout(crosshairTitles, 1);
    crosshairHeader->addWidget(crosshairPreview);
    crosshairCardLayout->addLayout(crosshairHeader);

    auto *crosshairColorButton = new QPushButton;
    crosshairColorButton->setObjectName(
        QStringLiteral("colorPreviewButton"));
    crosshairColorButton->setCursor(Qt::PointingHandCursor);
    crosshairColorButton->setMinimumHeight(42);
    QColor selectedCrosshairColor = settings.crosshairColor;
    auto *crosshairLength = new QSpinBox;
    crosshairLength->setRange(1, 100);
    crosshairLength->setSingleStep(1);
    crosshairLength->setSuffix(QStringLiteral(" %"));
    crosshairLength->setValue(
        qBound(1, settings.crosshairLength, 100));
    crosshairLength->setMinimumSize(104, 32);
    auto *crosshairWidth = new QDoubleSpinBox;
    crosshairWidth->setRange(0.5, 4.0);
    crosshairWidth->setSingleStep(0.25);
    crosshairWidth->setDecimals(2);
    crosshairWidth->setSuffix(QStringLiteral(" px"));
    crosshairWidth->setValue(
        qBound(0.5, settings.crosshairLineWidth, 4.0));
    crosshairWidth->setMinimumSize(104, 32);
    const auto updateCrosshairPreview =
        [crosshairPreview, crosshairLength, crosshairWidth,
         &selectedCrosshairColor, previewCanvasColor] {
            QPixmap preview(88, 88);
            preview.fill(Qt::transparent);
            QPainter painter(&preview);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const QPointF center(preview.width() * 0.5,
                                 preview.height() * 0.5);
            const double halfLength =
                13.0 + 31.0 * crosshairLength->value() / 100.0;
            const double lineWidth =
                qBound(0.5, crosshairWidth->value(), 4.0);
            const QColor outline =
                previewCanvasColor.lightness() > 128
                    ? QColor(0, 0, 0, 105)
                    : QColor(255, 255, 255, 42);
            const auto drawCrosshair =
                [&](const QColor &color, double width) {
                    painter.setPen(QPen(
                        color, width, Qt::SolidLine,
                        Qt::RoundCap));
                    painter.drawLine(
                        QPointF(center.x() - halfLength, center.y()),
                        QPointF(center.x() + halfLength, center.y()));
                    painter.drawLine(
                        QPointF(center.x(), center.y() - halfLength),
                        QPointF(center.x(), center.y() + halfLength));
                };
            drawCrosshair(outline, lineWidth + 2.0);
            drawCrosshair(selectedCrosshairColor, lineWidth);
            painter.setBrush(selectedCrosshairColor);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(center, 1.7, 1.7);
            crosshairPreview->setPixmap(preview);
        };
    const auto updateCrosshairControls =
        [crosshairColorButton, &selectedCrosshairColor,
         &updateCrosshairPreview] {
            const QString textColor =
                selectedCrosshairColor.lightness() > 150
                    ? QStringLiteral("#101820")
                    : QStringLiteral("#f5f8fa");
            crosshairColorButton->setStyleSheet(
                QStringLiteral(
                    "background:%1; color:%2; border-color:%3;")
                    .arg(selectedCrosshairColor.name(), textColor,
                         selectedCrosshairColor.darker(135).name()));
            crosshairColorButton->setText(
                QStringLiteral("十字光标颜色   %1")
                    .arg(selectedCrosshairColor.name().toUpper()));
            crosshairColorButton->setToolTip(
                QStringLiteral("点击选择十字光标颜色"));
            updateCrosshairPreview();
        };
    updateCrosshairControls();
    connect(crosshairColorButton, &QPushButton::clicked, &dialog,
            [this, &selectedCrosshairColor,
             &updateCrosshairControls] {
                const QColor picked = QColorDialog::getColor(
                    selectedCrosshairColor, this,
                    QStringLiteral("十字光标颜色"));
                if (picked.isValid()) {
                    selectedCrosshairColor = picked;
                    updateCrosshairControls();
                }
            });
    connect(crosshairLength, &QSpinBox::valueChanged, crosshairPreview,
            [&updateCrosshairPreview](int) {
                updateCrosshairPreview();
            });
    connect(crosshairWidth, &QDoubleSpinBox::valueChanged,
            crosshairPreview,
            [&updateCrosshairPreview](double) {
                updateCrosshairPreview();
            });
    crosshairCardLayout->addWidget(crosshairColorButton);
    auto *crosshairFields = new QHBoxLayout;
    crosshairFields->setContentsMargins(0, 2, 0, 0);
    crosshairFields->setSpacing(12);
    auto *sizeField = new QWidget;
    auto *sizeLayout = new QVBoxLayout(sizeField);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->setSpacing(6);
    auto *sizeLabel = new QLabel(QStringLiteral("屏幕长度"));
    sizeLabel->setObjectName(QStringLiteral("fieldLabel"));
    sizeLayout->addWidget(sizeLabel);
    sizeLayout->addWidget(crosshairLength);
    auto *widthField = new QWidget;
    auto *widthLayout = new QVBoxLayout(widthField);
    widthLayout->setContentsMargins(0, 0, 0, 0);
    widthLayout->setSpacing(6);
    auto *widthLabel = new QLabel(QStringLiteral("线宽"));
    widthLabel->setObjectName(QStringLiteral("fieldLabel"));
    widthLayout->addWidget(widthLabel);
    widthLayout->addWidget(crosshairWidth);
    crosshairFields->addWidget(sizeField, 1);
    crosshairFields->addWidget(widthField, 1);
    crosshairCardLayout->addLayout(crosshairFields);
    crosshairCardLayout->addStretch();
    settingsRow->addWidget(unitCard, 0, 0);
    settingsRow->addWidget(colorCard, 0, 1);
    settingsRow->addWidget(crosshairCard, 0, 2);
    settingsRow->setColumnStretch(0, 1);
    settingsRow->setColumnStretch(1, 1);
    settingsRow->setColumnStretch(2, 1);
    settingsRow->setRowStretch(0, 1);
    layout->addLayout(settingsRow, 1);

    auto *hint = new QLabel(
        QStringLiteral(
            "设置会立即应用到当前图纸并自动保存。尺寸文字参考图纸"
            "现有标注大小并随视图缩放；十字光标 100% 表示横竖线"
            "扩展到画布边界。"));
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("settingsFooterHint"));
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
    settings.crosshairColor = selectedCrosshairColor;
    settings.crosshairLength = crosshairLength->value();
    settings.crosshairLineWidth = crosshairWidth->value();
    m_view->setDisplaySettings(settings);
    saveApplicationDisplaySettings(settings);
    refreshUnitControls();
    refreshMeasurementTable();
    setDirty(true);
}

void MainWindow::showHelp()
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("helpDialog"));
    dialog.setWindowTitle(QStringLiteral("速看CAD 使用帮助"));
    dialog.resize(940, 760);
    dialog.setMinimumSize(760, 600);
    dialog.setStyleSheet(QStringLiteral(R"(
        QDialog#helpDialog {
            background: qlineargradient(
                x1:0, y1:0, x2:1, y2:1,
                stop:0 #f5f8fa, stop:1 #e8eef1);
        }
        QFrame#helpHeader {
            background: #ffffff;
            border: 1px solid #d3dfe4;
            border-radius: 11px;
        }
        QLabel#helpTitle {
            color: #143746;
            font-size: 21px;
            font-weight: 700;
        }
        QLabel#helpSubtitle {
            color: #627781;
            font-size: 12px;
        }
        QLabel#helpVersion {
            color: #0e6988;
            background: #e4f4f9;
            border: 1px solid #b8dce7;
            border-radius: 9px;
            padding: 4px 10px;
            font-size: 11px;
            font-weight: 600;
        }
        QTextBrowser#helpBrowser {
            background: #ffffff;
            color: #263842;
            border: 1px solid #d3dfe4;
            border-radius: 10px;
            padding: 15px 19px;
            selection-background-color: #d8ebf4;
        }
        QDialogButtonBox QPushButton {
            min-width: 78px;
            min-height: 31px;
            border-radius: 6px;
        }
    )"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);
    auto *header = new QFrame;
    header->setObjectName(QStringLiteral("helpHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(18, 13, 18, 13);
    headerLayout->setSpacing(14);
    auto *logo = new QLabel;
    logo->setFixedSize(44, 44);
    logo->setPixmap(
        QIcon(QStringLiteral(":/icons/logo.svg")).pixmap(44, 44));
    auto *headerText = new QVBoxLayout;
    headerText->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("速看CAD 使用帮助"));
    title->setObjectName(QStringLiteral("helpTitle"));
    auto *subtitle = new QLabel(
        QStringLiteral("工程施工看图、测量标注与多页打印"));
    subtitle->setObjectName(QStringLiteral("helpSubtitle"));
    headerText->addWidget(title);
    headerText->addWidget(subtitle);
    auto *version = new QLabel(
        QStringLiteral("v%1").arg(QCoreApplication::applicationVersion()));
    version->setObjectName(QStringLiteral("helpVersion"));
    headerLayout->addWidget(logo);
    headerLayout->addLayout(headerText);
    headerLayout->addStretch();
    headerLayout->addWidget(version);
    layout->addWidget(header);
    auto *browser = new QTextBrowser;
    browser->setObjectName(QStringLiteral("helpBrowser"));
    browser->setOpenExternalLinks(true);
    browser->setHtml(QStringLiteral(R"(
        <html><head><style>
        body {
            color:#263842; font-family:"Noto Sans CJK SC","PingFang SC";
            font-size:14px; line-height:1.72; margin:2px;
        }
        h2 {
            color:#123a4b; font-size:19px; margin:24px 0 9px 0;
            padding-bottom:5px; border-bottom:1px solid #dce6ea;
        }
        h2:first-child { margin-top:0; }
        h3 { color:#17647f; font-size:15px; margin:16px 0 5px 0; }
        p { margin:6px 0; }
        ul { margin:5px 0 10px 0; }
        li { margin:5px 0; }
        code, kbd {
            color:#125e78; background:#edf5f8;
            border:1px solid #d3e5ec; border-radius:4px;
            padding:1px 5px; font-family:"SF Mono","Menlo";
        }
        .tip {
            background:#ecf8f1; color:#24533a;
            border:1px solid #c6e7d3; border-left:4px solid #27a467;
            border-radius:6px; padding:10px 13px; margin:12px 0;
        }
        .note {
            background:#edf6fa; color:#245263;
            border:1px solid #cde4ed; border-left:4px solid #3a92b5;
            border-radius:6px; padding:10px 13px; margin:12px 0;
        }
        table { border-collapse:collapse; width:100%; margin:8px 0 12px 0; }
        th { background:#eef3f5; color:#274553; text-align:left; padding:7px 9px; }
        td { border-top:1px solid #e3eaed; padding:6px 9px; }
        </style></head><body>
        <h2>快速开始</h2>
        <p>速看CAD 直接读取 DWG、DXF、PDF 及其模型空间和布局。图纸、测量和校准数据只在本机处理。</p>
        <h3>打开与视图</h3>
        <ul>
          <li>启动页使用恒定饱和度与明度的 HSL 色相流光，主光从左后方 45° 柔和漫射，渐变按 40% 透明区、20% 指数过渡带和 40% 光核构成，明暗交界带占线宽 1/6；右上角“动效”开关可暂停或恢复并自动保存选择。最多显示 20 张最近图纸，每行 4 张。</li>
          <li>左键拖动图纸平移；滚轮以鼠标位置为中心缩放；<kbd>0</kbd> 显示全图。</li>
          <li>模型与布局分别保存各自的视口；切回时恢复切换前的中心和缩放，不强制复位。</li>
          <li>“视图 → 主题”提供 CAD 深色、CAD 浅色、CAD 深灰和经典蓝图；画布、界面与工具栏图标颜色同步切换，并在下次启动时恢复。</li>
          <li>独立测量工具栏默认关闭；可从“视图 → 测量工具”打开，并通过“测量工具停靠位置”切换左侧或右侧，也可直接拖动；位置和显隐状态会保存。</li>
          <li><kbd>Z</kbd> 窗口缩放；<kbd>F3</kbd> 对象捕捉；<kbd>F7</kbd> 栅格（默认开启）；<kbd>F8</kbd> 正交。</li>
          <li>十字光标为 CAD 自绘线；拖动时切换手形光标。图层颜色以带边框色块显示，名称保持高对比。</li>
        </ul>
        <h3>测量工具</h3>
        <ul>
          <li>支持对齐、线性、连续、批量、矩形、多边形、圆、半径、直径、角度、弧长、坐标、点到直线和定距测量。</li>
          <li>连续测量中可直接点击圆弧本体，系统自动选择最近端点、沿圆弧取整段弧长，并保留绘制真实圆弧。</li>
          <li>连续、批量、多边形和偏移面积可用 <kbd>Esc</kbd>、右键、<kbd>Enter</kbd> 或双击结束，再单击放置尺寸文字。</li>
          <li>连续测量逐段在线段上显示长度，并单独显示总长；批量尺寸朝鼠标方向放置。</li>
          <li>侧面积支持单段或连续多段线段、圆弧；逐段显示长度，结果同时显示总长和侧面积。</li>
          <li>所有标注按 CAD 规则自动判断是否使用引线：文字能放在尺寸线上时直接标注，过长或离开图元时改用引线。</li>
          <li>尺寸终端的实心箭头、开口箭头和建筑斜线采用紧凑 CAD 样式，并随图纸缩放。</li>
          <li>圆和半径测量绘制圆弧、半径、面积和周长，不显示圆心位置。</li>
          <li>直径测量绘制完整圆、穿过圆心的直径线、两端箭头和圆心十字点标记。</li>
          <li>矩形和多边形只绘制边界与尺寸，不填充测量区域。</li>
        </ul>
        <h3>对象捕捉</h3>
        <p>支持端点、中点、交点、垂足、圆心、象限点和最近点。捕捉目标按视口范围查询，视口外图元不会参与绘制或捕捉。</p>
        <h3>单位、比例和标注样式</h3>
        <ul>
          <li>提供 ISO 标准、建筑斜线、机械 ISO-25、工程开口箭头、小数圆点和紧凑端线六种样式。</li>
          <li>长度、面积和坐标分别设置单位与精度；尺寸文字参考图纸原有标注字高并随视图缩放。</li>
          <li>“标注显示与测量设置”可调整十字光标颜色、屏幕比例和线宽；预览背景与当前画布一致，默认使用白色、屏幕 100% 大小和 1.25 px 线宽。</li>
          <li>比例校准、测量和文字标注自动保存，重新打开图纸时恢复。</li>
        </ul>
        <h2>打印与 PDF</h2>
        <ul>
          <li>“框选打印与预览”只输出框选区域，框选之外保持纯白。</li>
          <li>分辨率提供 600、1200、2400 DPI 三档，默认 600 DPI。</li>
          <li>点击“添加一页并框选”返回图纸框选；按 <kbd>Esc</kbd> 可取消本次添加并返回原打印预览。</li>
          <li>多页按纵向连续排列，像多页 PDF 一样操作：滚轮连续滚动，<kbd>Ctrl</kbd>/<kbd>⌘</kbd>+滚轮或捏合缩放，拖拽平移，双击定位当前页。</li>
          <li>支持彩色或全黑打印。全黑模式将文字、尺寸、填充和所有 CAD 图元统一输出为黑色，背景保持白色。</li>
          <li>纸张、方向、颜色和分辨率改变后预览自动重建；可直接打印或保存为多页 PDF。</li>
        </ul>
        <h2>性能与文件</h2>
        <ul>
          <li>视口外图元不绘制；移动和缩放时按屏幕尺寸简化折线并合并重绘，停止后恢复高精度画面。</li>
          <li>画布默认启用硬件加速和多画布共享图形上下文，优先使用显卡完成缩放、平移和高分辨率绘制。</li>
          <li>交互刷新默认约 60 FPS；复杂视野会自动降到约 30 或 20 FPS，以降低 CPU/GPU 占用。</li>
          <li>损坏、非有限坐标、零长度、无点数、无效圆/圆弧和空文字等图元会在加载时从内存中删除。</li>
          <li>关闭图纸标签会释放图元、布局、空间索引、文字轮廓、矢量缓存和加载结果占用的内存。</li>
          <li>主题、标注样式、颜色、单位和十字光标设置保存到软件配置文件，下次启动自动恢复；图纸专属参数可在其后再覆盖全局值。</li>
          <li>解析缓存、缩略图和标注保存在系统缓存目录；删除最近记录不会删除原图纸。</li>
        </ul>
        <h3>常用快捷键</h3>
        <table>
          <tr><th>快捷键</th><th>作用</th></tr>
          <tr><td><kbd>V</kbd></td><td>选择 / 拖动标注</td></tr>
          <tr><td><kbd>E</kbd> / <kbd>A</kbd> / <kbd>L</kbd> / <kbd>C</kbd> / <kbd>B</kbd> / <kbd>P</kbd></td><td>删除 / 对齐 / 线性 / 连续 / 批量 / 多边形</td></tr>
          <tr><td><kbd>Shift+R</kbd> / <kbd>O</kbd> / <kbd>F</kbd> / <kbd>R</kbd> / <kbd>Shift+D</kbd></td><td>矩形 / 圆 / 填充 / 半径 / 直径</td></tr>
          <tr><td><kbd>G</kbd> / <kbd>K</kbd> / <kbd>X</kbd> / <kbd>J</kbd></td><td>角度 / 弧长 / 坐标 / 点到直线</td></tr>
          <tr><td><kbd>M</kbd> / <kbd>Shift+S</kbd> / <kbd>U</kbd> / <kbd>Shift+C</kbd></td><td>定距 / 侧面积 / 面积偏移 / 比例校准</td></tr>
          <tr><td><kbd>T</kbd> / <kbd>Z</kbd> / <kbd>0</kbd></td><td>文字 / 窗口缩放 / 显示全图</td></tr>
          <tr><td><kbd>Ctrl+P</kbd> / <kbd>Shift+Delete</kbd></td><td>打印预览 / 删除全部标注</td></tr>
          <tr><td><kbd>Delete</kbd> / <kbd>Esc</kbd></td><td>删除所选标注 / 取消或结束路径</td></tr>
          <tr><td><kbd>F1</kbd></td><td>打开本帮助</td></tr>
        </table>
        <p class="tip">图纸仅在本地读取和处理，不会上传。按 F1 可随时重新打开帮助。</p>
        <p class="note">提示：复杂图纸缩放时先显示低精度矢量，停止操作后自动恢复完整精度。打印预览中的缩放和滚动不会改变最终打印范围。</p>
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
    dialog.setObjectName(QStringLiteral("aboutDialog"));
    dialog.setWindowTitle(QStringLiteral("关于速看CAD"));
    dialog.setMinimumWidth(720);
    dialog.setStyleSheet(QStringLiteral(R"(
        QDialog#aboutDialog {
            background: qlineargradient(
                x1:0, y1:0, x2:1, y2:1,
                stop:0 #f6f9fa, stop:1 #e7eef1);
        }
        QLabel#aboutTitle {
            color:#123746; font-size:25px; font-weight:700;
        }
        QLabel#aboutVersion {
            color:#0d6988; background:#e3f3f8;
            border:1px solid #bddde7; border-radius:9px;
            padding:3px 9px; font-size:11px; font-weight:600;
        }
        QLabel#aboutDescription {
            color:#405964; font-size:13px; line-height:1.55;
        }
        QFrame#aboutDonation {
            background:#ffffff; border:1px solid #d3dfe4;
            border-radius:11px;
        }
        QLabel#aboutQr {
            background:#ffffff; border:1px solid #ccd9de;
            border-radius:7px; padding:5px;
        }
        QDialogButtonBox QPushButton {
            min-width:76px; min-height:31px; border-radius:6px;
        }
    )"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(28, 24, 28, 20);
    layout->setSpacing(18);

    auto *content = new QHBoxLayout;
    content->setSpacing(24);
    auto *info = new QVBoxLayout;
    info->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("速看CAD"));
    title->setObjectName(QStringLiteral("aboutTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(24.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    auto *version = new QLabel(
        QStringLiteral("版本 %1")
            .arg(QCoreApplication::applicationVersion()));
    version->setObjectName(QStringLiteral("aboutVersion"));
    auto *description = new QLabel(
        QStringLiteral(
            "速看CAD 是面向工程施工人员的本地图纸查看、测量与标注工具。"
            "可直接打开 DWG、DXF 和 PDF 图纸，查看模型与布局，"
            "完成尺寸、面积、角度等工程测量，并保存矢量化标注。"
            "所有图纸均在本机处理，不上传文件。"));
    description->setWordWrap(true);
    description->setMinimumWidth(360);
    description->setObjectName(QStringLiteral("aboutDescription"));
    auto *author = new QLabel(
        QStringLiteral("开发者：Mr. 王嘉"));
    auto *wechat = new QLabel(
        QStringLiteral("微信：SerendipityWangJia"));
    author->setTextInteractionFlags(Qt::TextSelectableByMouse);
    wechat->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *logo = new QLabel;
    logo->setPixmap(
        QIcon(QStringLiteral(":/icons/logo.svg")).pixmap(64, 64));
    logo->setFixedSize(64, 64);
    info->addWidget(logo);
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
    donationPanel->setObjectName(QStringLiteral("aboutDonation"));
    auto *donationLayout = new QVBoxLayout(donationPanel);
    donationLayout->setContentsMargins(14, 14, 14, 14);
    donationLayout->setSpacing(8);
    auto *donationTitle = new QLabel(QStringLiteral("微信打赏"));
    donationTitle->setAlignment(Qt::AlignCenter);
    donationTitle->setObjectName(QStringLiteral("panelTitle"));
    auto *qr = new QLabel;
    qr->setObjectName(QStringLiteral("aboutQr"));
    qr->setAlignment(Qt::AlignCenter);
    qr->setFixedSize(210, 210);
    QPixmap donationQr(
        QStringLiteral(":/images/wechat-donation.png"));
    if (!donationQr.isNull()) {
        qr->setPixmap(donationQr.scaled(
            200, 200, Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
        qr->setStyleSheet(
            QStringLiteral(
                "background:white;border:1px solid #d6dde1;"
                "border-radius:7px;padding:5px;"));
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
    m_printSession = PrintSession();
    m_printSession.active = true;
    m_printSession.selectingPage = true;
    m_view->beginTool(CadView::ToolMode::PrintArea);
}

void MainWindow::handlePrintArea(const QRectF &worldArea)
{
    if (!m_view->document() || !worldArea.isValid()) {
        m_view->beginTool(CadView::ToolMode::Select);
        return;
    }
    if (!m_printSession.active) {
        m_printSession = PrintSession();
        m_printSession.active = true;
    }
    m_lastPrintArea = worldArea.normalized();
    m_printSession.pages.append(
        {m_view->activeSpace(), m_lastPrintArea});
    m_printSession.currentPage = m_printSession.pages.size() - 1;
    m_printSession.selectingPage = false;
    m_view->beginTool(CadView::ToolMode::Select);
    showPrintPreview();
}

void MainWindow::handlePrintAreaCanceled()
{
    if (!m_printSession.active) {
        return;
    }
    m_printSession.selectingPage = false;
    m_view->beginTool(CadView::ToolMode::Select);
    if (m_printSession.pages.isEmpty()) {
        m_printSession.active = false;
        statusBar()->showMessage(
            QStringLiteral("已取消打印框选"), 2500);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("已取消添加页面，正在返回打印预览"), 2500);
    QTimer::singleShot(0, this, [this] {
        if (m_printSession.active
            && !m_printSession.pages.isEmpty()) {
            showPrintPreview();
        }
    });
}

void MainWindow::showPrintPreview()
{
    if (!m_view->document() || m_printSession.pages.isEmpty()) {
        return;
    }
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("printPreviewDialog"));
    dialog.setWindowTitle(
        QStringLiteral("打印预览 · %1")
            .arg(m_view->document()->fileName));
    dialog.resize(1460, 900);
    dialog.setMinimumSize(1100, 700);
    auto *mainLayout = new QHBoxLayout(&dialog);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(16);

    auto *previewColumn = new QVBoxLayout;
    previewColumn->setContentsMargins(0, 0, 0, 0);
    previewColumn->setSpacing(10);
    auto *previewHeader = new QFrame;
    previewHeader->setObjectName(QStringLiteral("previewHeader"));
    auto *previewHeaderLayout = new QHBoxLayout(previewHeader);
    previewHeaderLayout->setContentsMargins(16, 11, 16, 11);
    previewHeaderLayout->setSpacing(12);
    auto *previewTitle =
        new QLabel(QStringLiteral("连续页面预览"));
    previewTitle->setObjectName(QStringLiteral("previewTitle"));
    auto *previewPageLabel = new QLabel;
    previewPageLabel->setObjectName(QStringLiteral("previewPageStatus"));
    auto *previewHint = new QLabel(
        QStringLiteral("滚轮滚动 · Ctrl/⌘+滚轮缩放 · 拖拽平移"));
    previewHint->setObjectName(QStringLiteral("previewHint"));
    previewHeaderLayout->addWidget(previewTitle);
    previewHeaderLayout->addWidget(previewPageLabel);
    previewHeaderLayout->addStretch();
    previewHeaderLayout->addWidget(previewHint);
    previewColumn->addWidget(previewHeader);
    auto *previewCanvas = new PrintPreviewCanvas;
    previewCanvas->setObjectName(QStringLiteral("printPreview"));
    previewColumn->addWidget(previewCanvas, 1);
    mainLayout->addLayout(previewColumn, 1);

    QFrame *settingsPanel = panelFrame();
    settingsPanel->setObjectName(
        QStringLiteral("printSettingsPanel"));
    settingsPanel->setMinimumWidth(320);
    settingsPanel->setMaximumWidth(370);
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

    m_printSession.currentPage = qBound(
        0, m_printSession.currentPage,
        m_printSession.pages.size() - 1);
    auto *pageCombo = new QComboBox;
    for (int index = 0; index < m_printSession.pages.size(); ++index) {
        const CadPrintPage &page = m_printSession.pages[index];
        pageCombo->addItem(
            QStringLiteral("第 %1 页 · %2")
                .arg(index + 1)
                .arg(page.spaceName.isEmpty()
                         ? QStringLiteral("模型")
                         : page.spaceName),
            index);
    }
    pageCombo->setCurrentIndex(m_printSession.currentPage);
    auto *addPageButton =
        new QPushButton(QStringLiteral("＋ 添加一页并框选"));
    addPageButton->setObjectName(
        QStringLiteral("addPrintPageButton"));
    auto *removePageButton =
        new QPushButton(QStringLiteral("删除本页"));
    removePageButton->setObjectName(
        QStringLiteral("removePrintPageButton"));
    removePageButton->setEnabled(m_printSession.pages.size() > 1);
    auto *pageRow = new QHBoxLayout;
    pageRow->setContentsMargins(0, 0, 0, 0);
    pageRow->addWidget(pageCombo, 1);
    pageRow->addWidget(removePageButton);
    auto *pageSection = new QLabel(QStringLiteral("页面"));
    pageSection->setObjectName(QStringLiteral("printSectionTitle"));
    settingsLayout->addWidget(pageSection);
    settingsLayout->addLayout(pageRow);
    settingsLayout->addWidget(addPageButton);

    auto *printerCombo = new QComboBox;
    auto *refreshPrinterButton =
        new QPushButton(QStringLiteral("刷新"));
    auto *printerRow = new QHBoxLayout;
    printerRow->setContentsMargins(0, 0, 0, 0);
    printerRow->addWidget(printerCombo, 1);
    printerRow->addWidget(refreshPrinterButton);
    auto *outputSection = new QLabel(QStringLiteral("输出设置"));
    outputSection->setObjectName(QStringLiteral("printSectionTitle"));
    settingsLayout->addWidget(outputSection);
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
    const int selectedPaperIndex = paperCombo->findData(
        m_printSession.paperSize);
    paperCombo->setCurrentIndex(
        selectedPaperIndex >= 0 ? selectedPaperIndex : 0);
    const int selectedOrientationIndex =
        orientationCombo->findData(m_printSession.orientation);
    orientationCombo->setCurrentIndex(
        selectedOrientationIndex >= 0 ? selectedOrientationIndex : 0);
    auto *colorModeCombo = new QComboBox;
    colorModeCombo->addItem(
        QStringLiteral("黑白（所有 CAD 元素为黑色）"), true);
    colorModeCombo->addItem(
        QStringLiteral("彩色（使用 CAD 原色）"), false);
    colorModeCombo->setCurrentIndex(
        m_printSession.blackInk ? 0 : 1);
    auto *resolutionCombo = new QComboBox;
    resolutionCombo->addItem(QStringLiteral("低（600 DPI）"), 600);
    resolutionCombo->addItem(QStringLiteral("中（1200 DPI）"), 1200);
    resolutionCombo->addItem(QStringLiteral("高（2400 DPI）"), 2400);
    const int selectedResolutionIndex =
        resolutionCombo->findData(m_printSession.resolution);
    resolutionCombo->setCurrentIndex(
        selectedResolutionIndex >= 0 ? selectedResolutionIndex : 0);
    auto *copies = new QSpinBox;
    copies->setRange(1, 99);
    copies->setValue(qBound(1, m_printSession.copies, 99));
    auto *ruler = new QCheckBox(QStringLiteral("显示打印标尺"));
    ruler->setChecked(m_printSession.ruler);
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
    form->addRow(QStringLiteral("打印分辨率"), resolutionCombo);
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
    pdfButton->setObjectName(QStringLiteral("pdfPrintButton"));
    printButton->setObjectName(QStringLiteral("primaryPrintButton"));
    printButton->setDefault(true);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"));
    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(pdfButton);
    outputRow->addStretch();
    outputRow->addWidget(closeButton);
    outputRow->addWidget(printButton);
    settingsLayout->addLayout(outputRow);
    mainLayout->addWidget(settingsPanel);
    dialog.setStyleSheet(QStringLiteral(R"(
        QDialog#printPreviewDialog {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #f4f7f8, stop: 1 #e3eaee);
        }
        QFrame#previewHeader {
            background: #ffffff;
            border: 1px solid #d4dee3;
            border-radius: 9px;
        }
        QLabel#previewTitle {
            color: #173442;
            font-size: 15px;
            font-weight: 700;
        }
        QLabel#previewPageStatus {
            color: #0d6685;
            background: #e3f3f8;
            border: 1px solid #b9dce7;
            border-radius: 9px;
            padding: 3px 9px;
            font-size: 11px;
            font-weight: 600;
        }
        QLabel#previewHint {
            color: #667b85;
            font-size: 11px;
        }
        QFrame#printSettingsPanel {
            background: #ffffff;
            border: 1px solid #d4dee3;
            border-radius: 10px;
        }
        QLabel#printSectionTitle {
            color: #1f728f;
            font-size: 11px;
            font-weight: 700;
            letter-spacing: 1px;
            padding-top: 4px;
        }
        QDialog#printPreviewDialog QComboBox,
        QDialog#printPreviewDialog QSpinBox {
            min-height: 29px;
            background: #f8fafb;
            border: 1px solid #c8d5db;
            border-radius: 5px;
            padding: 1px 7px;
        }
        QPushButton#addPrintPageButton {
            min-height: 31px;
            color: #126d8c;
            background: #eaf7fb;
            border: 1px solid #b8dce7;
            border-radius: 5px;
            font-weight: 600;
        }
        QPushButton#addPrintPageButton:hover {
            background: #d9f1f8;
        }
        QPushButton#removePrintPageButton {
            color: #9d3f3f;
        }
        QPushButton#pdfPrintButton {
            min-height: 34px;
            padding: 0 13px;
        }
        QPushButton#primaryPrintButton {
            min-height: 34px;
            min-width: 82px;
            color: #ffffff;
            background: #167da1;
            border: 1px solid #126c8c;
            border-radius: 6px;
            font-weight: 700;
        }
        QPushButton#primaryPrintButton:hover {
            background: #126d8c;
        }
    )"));

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
    const auto refreshPageInfo = [&] {
        previewPageLabel->setText(
            QStringLiteral("%1 / %2")
                .arg(m_printSession.currentPage + 1)
                .arg(m_printSession.pages.size()));
        rangeInfo->setText(
            QStringLiteral("打印范围：第 %1 / %2 页的框选区域\n"
                           "框选外保持白色；未框选图纸不会输出。\n"
                           "滚轮滚动页面；Ctrl/⌘+滚轮或捏合缩放；"
                           "拖拽平移；双击定位当前页。")
                .arg(m_printSession.currentPage + 1)
                .arg(m_printSession.pages.size()));
    };
    const auto updatePreview = [&] {
        const QSizeF millimeters = paperMillimeters();
        const int resolution = resolutionCombo->currentData().toInt();
        const double longEdge =
            resolution <= 600 ? 1400.0
            : resolution <= 1200 ? 2000.0 : 2600.0;
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
        QVector<PrintPreviewCanvas::Page> previewPages;
        previewPages.reserve(m_printSession.pages.size());
        for (const CadPrintPage &page : m_printSession.pages) {
            previewPages.append(
                {m_view->renderPrintPageToImage(
                     page, pixelSize,
                     colorModeCombo->currentData().toBool()),
                 millimeters});
        }
        previewCanvas->setPages(
            std::move(previewPages),
            qBound(0, m_printSession.currentPage,
                   m_printSession.pages.size() - 1));
        refreshPageInfo();
    };
    const auto refreshPrinters = [&] {
        QString previous = printerCombo->currentData().toString();
        if (previous.isEmpty()) {
            previous = m_printSession.printerName;
        }
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
    bool addPageRequested = false;
    bool printed = false;
    const auto printOutput = [&](bool toPdf) {
        if (!toPdf && printerCombo->currentData().toString().isEmpty()) {
            printerStatus->setText(
                QStringLiteral("没有可用打印机，正在打开系统打印机设置。"));
            openSystemPrinterSettings();
            return;
        }
        const int resolution = resolutionCombo->currentData().toInt();
        m_printSession.paperSize = paperCombo->currentData().toInt();
        m_printSession.orientation =
            orientationCombo->currentData().toInt();
        m_printSession.resolution = resolution;
        m_printSession.copies = copies->value();
        m_printSession.blackInk =
            colorModeCombo->currentData().toBool();
        m_printSession.ruler = ruler->isChecked();
        m_printSession.printerName =
            printerCombo->currentData().toString();
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
        } else {
            output.setPrinterName(
                printerCombo->currentData().toString());
            output.setOutputFormat(QPrinter::NativeFormat);
        }
        output.setResolution(resolution);
        output.setPageSize(selectedPaper());
        output.setPageOrientation(selectedOrientation());
        output.setCopyCount(copies->value());
        if (!m_view->printPagesToPrinter(
                &output, m_printSession.pages,
                colorModeCombo->currentData().toBool())) {
            QMessageBox::warning(
                &dialog, QStringLiteral("打印失败"),
                toPdf ? QStringLiteral("无法创建 PDF 文件。")
                      : QStringLiteral("无法发送到所选打印机。"));
            return;
        }
        printed = true;
        m_printSession.active = false;
        statusBar()->showMessage(
            toPdf
                ? QStringLiteral("已打印 %1 页到 PDF：%2")
                      .arg(m_printSession.pages.size())
                      .arg(output.outputFileName())
                : QStringLiteral("已发送 %1 页到打印机：%2")
                      .arg(m_printSession.pages.size())
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
    connect(resolutionCombo, &QComboBox::currentIndexChanged,
            &dialog, [&] { updatePreview(); });
    connect(pageCombo, &QComboBox::currentIndexChanged,
            &dialog, [&](int index) {
                if (index < 0) {
                    return;
                }
                m_printSession.currentPage =
                    pageCombo->itemData(index).toInt();
                removePageButton->setEnabled(
                    m_printSession.pages.size() > 1);
                previewCanvas->scrollToPage(
                    m_printSession.currentPage);
                refreshPageInfo();
            });
    connect(removePageButton, &QPushButton::clicked, &dialog, [&] {
        if (m_printSession.pages.size() <= 1) {
            return;
        }
        m_printSession.pages.removeAt(m_printSession.currentPage);
        m_printSession.currentPage = qBound(
            0, m_printSession.currentPage,
            m_printSession.pages.size() - 1);
        const QSignalBlocker blocker(pageCombo);
        pageCombo->clear();
        for (int index = 0; index < m_printSession.pages.size(); ++index) {
            const CadPrintPage &page = m_printSession.pages[index];
            pageCombo->addItem(
                QStringLiteral("第 %1 页 · %2")
                    .arg(index + 1)
                    .arg(page.spaceName.isEmpty()
                             ? QStringLiteral("模型")
                             : page.spaceName),
                index);
        }
        pageCombo->setCurrentIndex(m_printSession.currentPage);
        removePageButton->setEnabled(m_printSession.pages.size() > 1);
        updatePreview();
    });
    connect(addPageButton, &QPushButton::clicked, &dialog, [&] {
        m_printSession.paperSize =
            paperCombo->currentData().toInt();
        m_printSession.orientation =
            orientationCombo->currentData().toInt();
        m_printSession.resolution =
            resolutionCombo->currentData().toInt();
        m_printSession.copies = copies->value();
        m_printSession.blackInk =
            colorModeCombo->currentData().toBool();
        m_printSession.ruler = ruler->isChecked();
        m_printSession.printerName =
            printerCombo->currentData().toString();
        m_printSession.selectingPage = true;
        addPageRequested = true;
        dialog.done(QDialog::Accepted);
    });
    connect(ruler, &QCheckBox::toggled, previewCanvas,
            &PrintPreviewCanvas::setRulerVisible);
    connect(pdfButton, &QPushButton::clicked, &dialog,
            [&] { printOutput(true); });
    connect(printButton, &QPushButton::clicked, &dialog,
            [&] { printOutput(false); });
    connect(closeButton, &QPushButton::clicked, &dialog,
            &QDialog::reject);
    previewCanvas->setVisiblePageHandler(
        [&, pageCombo](int pageIndex) {
            m_printSession.currentPage = pageIndex;
            const QSignalBlocker blocker(pageCombo);
            pageCombo->setCurrentIndex(pageIndex);
            refreshPageInfo();
        });
    previewCanvas->setRulerVisible(ruler->isChecked());
    refreshPrinters();
    updatePreview();
    dialog.exec();
    if (addPageRequested) {
        QTimer::singleShot(0, this, [this] {
            if (m_printSession.active) {
                m_view->beginTool(CadView::ToolMode::PrintArea);
            }
        });
    } else if (!printed) {
        m_printSession.active = false;
    }
}

void MainWindow::changeUnit(int index)
{
    if (!m_view->document() || index < 0) {
        return;
    }
    m_view->setDisplayUnit(
        static_cast<MeasuringUnit>(m_unitCombo->itemData(index).toInt()));
    saveApplicationDisplaySettings(m_view->displaySettings());
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
    saveApplicationDisplaySettings(m_view->displaySettings());
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
    if (m_measureToolbar) {
        QSettings settings = applicationSettings();
        settings.setValue(
            QStringLiteral("layout/measureToolbarArea"),
            toolBarArea(m_measureToolbar) == Qt::RightToolBarArea
                ? QStringLiteral("right") : QStringLiteral("left"));
        settings.setValue(
            QStringLiteral("layout/measureToolbarVisible"),
            m_measureToolbar->isVisible());
        settings.sync();
    }
    event->accept();
}
