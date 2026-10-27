#include "StartScreen.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

constexpr double kStartupTau = 6.28318530717958647692;

QPixmap thumbnailPixmap(const RecentDrawing &drawing,
                        const QSize &size)
{
    QPixmap source;
    if (!drawing.thumbnailPath.isEmpty()) {
        source.load(drawing.thumbnailPath);
    }
    QPixmap result(size);
    result.fill(QColor(QStringLiteral("#080b0e")));
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!source.isNull()) {
        const QPixmap scaled = source.scaled(
            size, Qt::KeepAspectRatioByExpanding,
            Qt::SmoothTransformation);
        painter.drawPixmap(
            QPoint((size.width() - scaled.width()) / 2,
                   (size.height() - scaled.height()) / 2),
            scaled);
        return result;
    }
    painter.setPen(QPen(QColor(43, 57, 66), 0.7));
    for (int x = 12; x < size.width(); x += 24) {
        painter.drawLine(x, 0, x, size.height());
    }
    for (int y = 12; y < size.height(); y += 24) {
        painter.drawLine(0, y, size.width(), y);
    }
    const QPoint center(size.width() / 2, size.height() / 2 - 8);
    painter.setPen(QPen(QColor(93, 167, 211), 2.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(QRectF(center.x() - 26.0, center.y() - 31.0,
                            52.0, 62.0));
    painter.drawLine(center.x() - 14, center.y() - 12,
                     center.x() + 14, center.y() - 12);
    painter.drawLine(center.x() - 14, center.y(),
                     center.x() + 14, center.y());
    painter.drawLine(center.x() - 14, center.y() + 12,
                     center.x() + 5, center.y() + 12);
    painter.setPen(QColor(116, 130, 140));
    painter.drawText(QRect(0, size.height() - 28, size.width(), 20),
                     Qt::AlignCenter, QStringLiteral("DWG / DXF / PDF"));
    return result;
}

QString elided(const QString &text, int width, const QFont &font)
{
    return QFontMetrics(font).elidedText(
        text, Qt::ElideMiddle, qMax(20, width));
}

} // namespace

class StartScreenLineBackground final : public QWidget {
public:
    explicit StartScreenLineBackground(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::NoFocus);
        buildArtwork();
        m_animationTimer.setTimerType(Qt::PreciseTimer);
        m_animationTimer.setInterval(16);
        connect(&m_animationTimer, &QTimer::timeout, this, [this] {
            const double elapsedSeconds =
                m_frameClock.nsecsElapsed() / 1.0e9;
            m_phase = std::fmod(
                m_resumePhase + elapsedSeconds * 0.13,
                kStartupTau);
            update();
        });
    }

    void setDarkTheme(bool dark)
    {
        if (m_dark == dark) {
            return;
        }
        m_dark = dark;
        m_backgroundCache = QImage();
        update();
    }

    void setActive(bool active)
    {
        if (!active) {
            m_animationTimer.stop();
            return;
        }
        if (isVisible() && !m_animationTimer.isActive()) {
            m_resumePhase = m_phase;
            m_frameClock.restart();
            m_animationTimer.start();
        }
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (m_backgroundCache.size() != size()) {
            m_backgroundCache =
                QImage(size(), QImage::Format_RGB32);
            const QColor backgroundTop =
                m_dark ? QColor(QStringLiteral("#05070d"))
                       : QColor(QStringLiteral("#f7f9fb"));
            const QColor backgroundBottom =
                m_dark ? QColor(QStringLiteral("#0d1020"))
                       : QColor(QStringLiteral("#e9eef3"));
            QLinearGradient background(
                rect().topLeft(), rect().bottomRight());
            background.setColorAt(0.0, backgroundTop);
            background.setColorAt(
                0.54, backgroundTop.lighter(112));
            background.setColorAt(1.0, backgroundBottom);
            QPainter cachePainter(&m_backgroundCache);
            cachePainter.fillRect(rect(), background);
        }
        painter.drawImage(0, 0, m_backgroundCache);

        const auto hslColor =
            [](double hue, int alpha) {
                hue = std::fmod(hue, 1.0);
                if (hue < 0.0) {
                    hue += 1.0;
                }
                QColor color = QColor::fromHslF(
                    hue, 0.90, 0.60);
                color.setAlpha(qBound(0, alpha, 255));
                return color;
            };

        const QRectF viewport = rect();
        const QPointF center = viewport.center();
        const double minimumDimension =
            qMin(viewport.width(), viewport.height());
        const double overscan =
            qMax(viewport.width(), viewport.height()) * 0.55;
        const double left = -overscan;
        const double right = viewport.width() + overscan;

        painter.save();
        if (m_dark) {
            painter.setCompositionMode(QPainter::CompositionMode_Plus);
        }
        for (int index = 0; index < 2; ++index) {
            const double glowHue =
                m_phase / kStartupTau
                + index * 0.37;
            QColor glowColor =
                hslColor(glowHue, m_dark ? 34 : 18);
            QColor transparentGlow = glowColor;
            transparentGlow.setAlpha(0);
            const QPointF glowCenter(
                viewport.width()
                    * (0.16
                       + index * 0.17
                       + 0.045
                           * std::sin(m_phase + index * 1.57)),
                viewport.height()
                    * (0.14
                       + index * 0.17
                       + 0.04
                           * std::cos(m_phase + index * 1.23)));
            const double glowRadius =
                minimumDimension * (0.34 + index * 0.035);
            QRadialGradient glow(glowCenter, glowRadius);
            glow.setColorAt(0.0, glowColor);
            glow.setColorAt(
                0.55, hslColor(glowHue, m_dark ? 10 : 7));
            glow.setColorAt(1.0, transparentGlow);
            painter.setPen(Qt::NoPen);
            painter.setBrush(glow);
            painter.drawEllipse(glowCenter, glowRadius, glowRadius);
        }
        painter.restore();

        const auto appendCubic =
            [](QPainterPath &path, const QPointF &from,
               const QPointF &to) {
                const double handle = (to.x() - from.x()) * 0.38;
                path.cubicTo(
                    QPointF(from.x() + handle, from.y()),
                    QPointF(to.x() - handle, to.y()), to);
            };
        const auto buildOpenPath =
            [&appendCubic](
                const std::array<QPointF, 8> &anchors) {
                QPainterPath path;
                path.moveTo(anchors.front());
                for (int index = 1; index < int(anchors.size());
                     ++index) {
                    appendCubic(
                        path, anchors[index - 1], anchors[index]);
                }
                return path;
            };

        for (const FlowCurve &curve : std::as_const(m_curves)) {
            painter.save();
            painter.translate(center);
            const double curveAngle =
                curve.tilt
                + 0.8
                    * std::sin(
                        m_phase + curve.phase * 1.31);
            painter.rotate(curveAngle);
            painter.translate(-center);

            constexpr int anchorCount = 8;
            std::array<QPointF, anchorCount> topAnchors;
            std::array<QPointF, anchorCount> centerAnchors;
            std::array<QPointF, anchorCount> bottomAnchors;
            const double baseY =
                center.y() + curve.lane * viewport.height();
            const double motion =
                m_phase * curve.speedFactor + curve.phase;
            for (int index = 0; index < anchorCount; ++index) {
                const double normalized =
                    double(index) / double(anchorCount - 1);
                const double x = left + normalized * (right - left);
                const double y =
                    baseY
                    + std::sin(
                          normalized * kStartupTau * curve.frequency
                          + motion)
                        * curve.amplitude
                    + std::sin(
                          normalized * kStartupTau
                              * (curve.frequency + 0.72)
                          - motion * 0.62)
                        * curve.amplitude * 0.38;
                const double fold =
                    0.48
                    + 0.52
                        * (0.5
                           + 0.5
                               * std::sin(
                                   normalized * kStartupTau * 1.7
                                   + motion * 0.8
                                   + curve.phase));
                const double ribbonWidth =
                    curve.ribbonWidth * fold;
                centerAnchors[index] = QPointF(x, y);
                topAnchors[index] =
                    QPointF(x, y - ribbonWidth * 0.5);
                bottomAnchors[index] =
                    QPointF(x, y + ribbonWidth * 0.5);
            }

            const QPainterPath topPath =
                buildOpenPath(topAnchors);
            const QPainterPath centerPath =
                buildOpenPath(centerAnchors);
            QPainterPath ribbonPath = topPath;
            ribbonPath.lineTo(bottomAnchors.back());
            for (int index = anchorCount - 2; index >= 0; --index) {
                appendCubic(
                    ribbonPath, bottomAnchors[index + 1],
                    bottomAnchors[index]);
            }
            ribbonPath.closeSubpath();

            const double animatedHue = std::fmod(
                curve.hue + m_phase / kStartupTau, 1.0);
            const double inverseAngle =
                -curveAngle * kStartupTau / 360.0;
            const QPointF lightDirection(
                std::cos(kStartupTau / 8.0 + inverseAngle),
                std::sin(kStartupTau / 8.0 + inverseAngle));
            const QPointF lightCenter(
                viewport.center().x(), baseY);
            const double lightRadius =
                curve.ribbonWidth * 0.5;
            QLinearGradient diffuseLight(
                lightCenter - lightDirection * lightRadius,
                lightCenter + lightDirection * lightRadius);
            constexpr int diffuseStops = 72;
            constexpr double terminatorCenter = 0.42;
            constexpr double terminatorHalfWidth =
                1.0 / 12.0;
            for (int stop = 0; stop <= diffuseStops; ++stop) {
                const double position =
                    double(stop) / double(diffuseStops);
                const double smoothPosition = qBound(
                    0.0,
                    (position
                     - (terminatorCenter - terminatorHalfWidth))
                        / (terminatorHalfWidth * 2.0),
                    1.0);
                const double eased =
                    smoothPosition * smoothPosition
                    * (3.0 - 2.0 * smoothPosition);
                const double litAlpha =
                    (m_dark ? 178.0 : 122.0)
                    * (0.55 + curve.depth * 0.45);
                const double shadowAlpha =
                    (m_dark ? 20.0 : 13.0)
                    * (0.65 + curve.depth * 0.35);
                const int alpha = qRound(
                    litAlpha
                    + (shadowAlpha - litAlpha) * eased);
                diffuseLight.setColorAt(
                    position,
                    hslColor(animatedHue, alpha));
            }
            const double coreAlpha =
                m_dark ? 232.0 : 178.0;
            QLinearGradient lightGradient(
                left, 0.0, right, 0.0);
            constexpr int gradientStops = 80;
            for (int stop = 0; stop <= gradientStops; ++stop) {
                const double position =
                    double(stop) / double(gradientStops);
                double intensity = 0.0;
                if (position > 0.40 && position < 0.60) {
                    const double transition =
                        (position - 0.40) / 0.20;
                    intensity =
                        (std::exp(5.0 * transition) - 1.0)
                        / (std::exp(5.0) - 1.0);
                } else if (position >= 0.60) {
                    intensity = 1.0;
                }
                const int alpha =
                    qRound(coreAlpha * intensity);
                lightGradient.setColorAt(
                    position,
                    hslColor(
                        animatedHue + position * 0.16,
                        alpha));
            }

            painter.setBrush(Qt::NoBrush);
            if (m_dark) {
                painter.setCompositionMode(
                    QPainter::CompositionMode_Plus);
            }
            const auto drawHalo =
                [&](double width, double opacity) {
                    painter.setOpacity(
                        opacity * (0.55 + curve.depth * 0.45));
                    painter.setPen(QPen(
                        QBrush(lightGradient), width,
                        Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
                    painter.drawPath(centerPath);
                };
            drawHalo(30.0, 0.025);
            drawHalo(20.0, 0.045);
            drawHalo(12.0, 0.075);
            drawHalo(6.5, 0.12);

            painter.setOpacity(0.72 * curve.depth);
            painter.setPen(Qt::NoPen);
            painter.fillPath(ribbonPath, diffuseLight);

            painter.save();
            painter.translate(-1.15, 0.45);
            painter.setOpacity(0.11);
            painter.setPen(QPen(
                QBrush(lightGradient), 3.0,
                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(centerPath);
            painter.restore();

            painter.setOpacity(
                0.82 + curve.depth * 0.18);
            painter.setPen(QPen(
                QBrush(lightGradient),
                1.15 + curve.depth * 0.75,
                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(centerPath);

            painter.setOpacity(
                m_dark ? 0.35 : 0.24);
            painter.setPen(QPen(
                QBrush(lightGradient), 0.9,
                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(topPath);
            painter.setOpacity(1.0);
            painter.restore();
        }

        if (!m_noiseTexture.isNull()) {
            painter.save();
            if (m_dark) {
                painter.setCompositionMode(
                    QPainter::CompositionMode_Plus);
            }
            painter.setOpacity(m_dark ? 0.035 : 0.022);
            painter.drawTiledPixmap(rect(), m_noiseTexture);
            painter.restore();
        }
    }

private:
    struct FlowCurve {
        double lane = 0.0;
        double amplitude = 70.0;
        double frequency = 1.0;
        double phase = 0.0;
        double tilt = -12.0;
        double ribbonWidth = 48.0;
        double depth = 1.0;
        double hue = 0.0;
        int speedFactor = 1;
    };

    void buildArtwork()
    {
        QRandomGenerator *random = QRandomGenerator::global();
        m_curves.clear();
        for (int index = 0; index < 6; ++index) {
            const auto randomDouble = [random] {
                return random->generateDouble();
            };
            FlowCurve curve;
            curve.lane =
                (double(index) - 3.0) * 0.115
                + (randomDouble() - 0.5) * 0.045;
            curve.amplitude =
                48.0 + randomDouble() * 86.0;
            curve.frequency =
                0.72 + randomDouble() * 0.82;
            curve.phase = randomDouble() * kStartupTau;
            curve.tilt = -27.0 + randomDouble() * 43.0;
            curve.depth =
                0.42 + randomDouble() * 0.58;
            curve.ribbonWidth =
                (30.0 + randomDouble() * 68.0)
                * (0.72 + curve.depth * 0.48);
            curve.hue = std::fmod(
                double(index) * 0.137
                    + randomDouble() * 0.72,
                1.0);
            curve.speedFactor = 1;
            m_curves.append(curve);
        }
        std::sort(
            m_curves.begin(), m_curves.end(),
            [](const FlowCurve &first, const FlowCurve &second) {
                return first.depth < second.depth;
            });

        QImage noise(96, 96, QImage::Format_ARGB32_Premultiplied);
        noise.fill(Qt::transparent);
        for (int y = 0; y < noise.height(); ++y) {
            for (int x = 0; x < noise.width(); ++x) {
                if (random->bounded(100) >= 9) {
                    continue;
                }
                const int alpha =
                    18 + int(random->bounded(58));
                noise.setPixelColor(
                    x, y, QColor(210, 228, 235, alpha));
            }
        }
        m_noiseTexture = QPixmap::fromImage(noise);
    }

    QTimer m_animationTimer;
    QElapsedTimer m_frameClock;
    QImage m_backgroundCache;
    QPixmap m_noiseTexture;
    QVector<FlowCurve> m_curves;
    double m_phase = 0.0;
    double m_resumePhase = 0.0;
    bool m_dark = true;
};

StartScreen::StartScreen(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("startScreen"));
    setMinimumSize(960, 540);
    m_background = new StartScreenLineBackground(this);
    m_background->setObjectName(
        QStringLiteral("startupLineBackground"));
    m_background->setGeometry(rect());
    m_background->lower();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(64, 38, 64, 28);
    root->setSpacing(24);

    auto *brandRow = new QHBoxLayout;
    brandRow->setSpacing(14);
    auto *logo = new QLabel;
    logo->setFixedSize(58, 58);
    logo->setPixmap(
        QIcon(QStringLiteral(":/icons/logo.svg"))
            .pixmap(58, 58));
    brandRow->addWidget(logo);
    auto *brandText = new QVBoxLayout;
    brandText->setSpacing(1);
    auto *name = new QLabel(QStringLiteral("速看CAD"));
    name->setObjectName(QStringLiteral("startBrand"));
    auto *tagline = new QLabel(
        QStringLiteral("工程施工看图 · 精确测量 · 矢量标注"));
    tagline->setObjectName(QStringLiteral("startTagline"));
    brandText->addWidget(name);
    brandText->addWidget(tagline);
    brandRow->addLayout(brandText);
    brandRow->addStretch();
    m_motionToggle = new QToolButton;
    m_motionToggle->setObjectName(QStringLiteral("startMotionToggle"));
    m_motionToggle->setCheckable(true);
    m_motionToggle->setChecked(m_motionEnabled);
    m_motionToggle->setCursor(Qt::PointingHandCursor);
    m_motionToggle->setToolTip(
        QStringLiteral("暂停或恢复启动页丝绸背景动画"));
    m_motionToggle->setAccessibleName(
        QStringLiteral("启动页线条动效开关"));
    const auto updateMotionToggle =
        [this](bool enabled) {
            m_motionToggle->setText(
                enabled ? QStringLiteral("动效  开")
                        : QStringLiteral("动效  关"));
        };
    updateMotionToggle(m_motionEnabled);
    connect(m_motionToggle, &QToolButton::toggled, this,
            [this, updateMotionToggle](bool enabled) {
                m_motionEnabled = enabled;
                updateMotionToggle(enabled);
                if (m_background) {
                    m_background->setActive(enabled);
                }
                emit motionEnabledChanged(enabled);
            });
    brandRow->addWidget(m_motionToggle);
    auto *version = new QLabel(
        QStringLiteral("v") + QCoreApplication::applicationVersion());
    version->setObjectName(QStringLiteral("startVersion"));
    brandRow->addWidget(version);
    root->addLayout(brandRow);

    auto *workspace = new QHBoxLayout;
    workspace->setSpacing(24);
    auto *leftColumn = new QVBoxLayout;
    leftColumn->setSpacing(16);
    auto *rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(10);

    auto *hero = new QFrame;
    hero->setObjectName(QStringLiteral("startHero"));
    hero->setMaximumWidth(430);
    hero->setSizePolicy(
        QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(34, 28, 34, 28);
    auto *heroText = new QVBoxLayout;
    heroText->setSpacing(8);
    auto *welcome = new QLabel(QStringLiteral("打开工程图纸"));
    welcome->setObjectName(QStringLiteral("startWelcome"));
    auto *description = new QLabel(
        QStringLiteral("直接读取 DWG / DXF 模型、布局与 PDF 页面，"
                       "继续上次的比例和测量标注。"));
    description->setObjectName(QStringLiteral("startDescription"));
    description->setWordWrap(true);
    auto *openButton = new QPushButton(
        QIcon(QStringLiteral(":/icons/open.svg")),
        QStringLiteral("打开 DWG / DXF / PDF 图纸"));
    openButton->setObjectName(QStringLiteral("startOpenButton"));
    openButton->setCursor(Qt::PointingHandCursor);
    openButton->setMinimumSize(190, 44);
    openButton->setMaximumWidth(250);
    connect(openButton, &QPushButton::clicked, this,
            &StartScreen::browseRequested);
    heroText->insertStretch(0);
    heroText->addWidget(welcome);
    heroText->addWidget(description);
    heroText->addSpacing(8);
    heroText->addWidget(openButton);
    heroText->addStretch();
    heroLayout->addLayout(heroText, 1);
    leftColumn->addWidget(hero, 1);

    auto *recentHeader = new QHBoxLayout;
    auto *recentTitle = new QLabel(QStringLiteral("最近打开"));
    recentTitle->setObjectName(QStringLiteral("startSectionTitle"));
    recentHeader->addWidget(recentTitle);
    recentHeader->addStretch();
    auto *recentHint = new QLabel(
        QStringLiteral("每行 4 张 · 最多 20 条 · 单击缩略图继续工作"));
    recentHint->setObjectName(QStringLiteral("startRecentHint"));
    recentHeader->addWidget(recentHint);
    rightColumn->addLayout(recentHeader);

    m_scrollArea = new QScrollArea;
    m_scrollArea->setObjectName(QStringLiteral("startRecentScroll"));
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_recentHost = new QWidget;
    m_recentHost->setObjectName(QStringLiteral("startRecentHost"));
    m_recentGrid = new QGridLayout(m_recentHost);
    m_recentGrid->setContentsMargins(0, 0, 0, 0);
    m_recentGrid->setHorizontalSpacing(16);
    m_recentGrid->setVerticalSpacing(16);
    m_recentGrid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_scrollArea->setWidget(m_recentHost);
    rightColumn->addWidget(m_scrollArea, 1);

    m_emptyLabel = new QLabel(
        QStringLiteral("还没有最近打开的图纸\n"
                       "从上方选择“打开 DWG / DXF / PDF 图纸”开始"));
    m_emptyLabel->setObjectName(QStringLiteral("startEmpty"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    rightColumn->addWidget(m_emptyLabel, 1);

    workspace->addLayout(leftColumn, 0);
    workspace->addLayout(rightColumn, 1);
    root->addLayout(workspace, 1);

    auto *footer = new QLabel(
        QStringLiteral("速看CAD · 本地读取 · 不上传图纸"));
    footer->setObjectName(QStringLiteral("startFooter"));
    root->addWidget(footer);

    m_darkStyleSheet = QStringLiteral(R"(
        QWidget#startScreen {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                        stop:0 #090c0f, stop:0.55 #0e1418, stop:1 #111a20);
            color: #d9e1e6;
        }
        QLabel#startBrand {
            color: #eef5f8; font-size: 22px; font-weight: 700;
            letter-spacing: 3px;
        }
        QLabel#startTagline { color: #7d8b94; font-size: 12px; }
        QLabel#startVersion { color: #53616a; font-size: 11px; }
        QToolButton#startMotionToggle {
            background: rgba(17, 27, 34, 220);
            color: #a7b7c1; border: 1px solid #30434f;
            border-radius: 13px; padding: 5px 12px;
            font-size: 10px; font-weight: 600;
        }
        QToolButton#startMotionToggle:hover {
            background: #162630; border-color: #568ba4;
            color: #dceaf0;
        }
        QToolButton#startMotionToggle:checked {
            background: #123934; color: #b5f9e9;
            border-color: #43c7ac;
        }
        QFrame#startHero {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                        stop:0 #11191e, stop:1 #0d1215);
            border: 1px solid #2b3942; border-left: 2px solid #5da7d3;
            border-radius: 12px;
        }
        QLabel#startWelcome {
            color: #eef5f8; font-size: 26px; font-weight: 600;
        }
        QLabel#startDescription {
            color: #8f9da6; font-size: 13px; max-width: 520px;
        }
        QPushButton#startOpenButton {
            background: #1b526d; color: #f2f8fb;
            border: 1px solid #67b2d8; border-radius: 8px;
            padding: 9px 16px; font-size: 13px; font-weight: 600;
            text-align: left;
        }
        QPushButton#startOpenButton:hover {
            background: #236b8d; border-color: #8ad0f1;
        }
        QPushButton#startOpenButton:pressed { background: #17465d; }
        QLabel#startSectionTitle {
            color: #e4ecef; font-size: 16px; font-weight: 600;
        }
        QLabel#startRecentHint { color: #5e6d76; font-size: 11px; }
        QScrollArea#startRecentScroll, QWidget#startRecentHost {
            background: transparent; border: none;
        }
        QPushButton#recentCard {
            background: #10171b; border: 1px solid #29363e;
            border-radius: 10px; padding: 0; text-align: left;
        }
        QPushButton#recentCard:hover {
            background: #162229; border-color: #5b9ec0;
        }
        QPushButton#recentCard:pressed { background: #0c1215; }
        QLabel#recentPreview {
            background: #080b0e;
            border: 1px solid #223039;
            border-radius: 6px;
        }
        QLabel#recentTitle {
            color: #dce5e9; font-size: 12px; font-weight: 600;
        }
        QLabel#recentPath, QLabel#recentDate {
            color: #667680; font-size: 10px;
        }
        QToolButton#recentRemove {
            background: transparent; color: #74858e;
            border: 1px solid transparent; border-radius: 9px;
            font-size: 15px; padding: 0;
        }
        QToolButton#recentRemove:hover {
            background: #552a2b; color: #ffc2bd;
            border-color: #9c4d4b;
        }
        QLabel#startEmpty {
            color: #65747d; font-size: 12px; padding: 26px;
            border: 1px dashed #27343b; background: #0c1114;
        }
        QLabel#startFooter { color: #4e5b63; font-size: 10px; }
    )");
    setStyleSheet(m_darkStyleSheet);
}

void StartScreen::setCadTheme(bool dark)
{
    if (m_darkTheme == dark && !styleSheet().isEmpty()) {
        return;
    }
    m_darkTheme = dark;
    if (m_background) {
        m_background->setDarkTheme(dark);
    }
    if (dark) {
        setStyleSheet(m_darkStyleSheet);
    } else {
        setStyleSheet(QStringLiteral(R"(
            QWidget#startScreen {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                            stop:0 #f4f6f7, stop:0.55 #edf1f3,
                            stop:1 #e6ecef);
                color: #20343f;
            }
            QLabel#startBrand {
                color: #153847; font-size: 22px; font-weight: 700;
                letter-spacing: 3px;
            }
            QLabel#startTagline { color: #63757e; font-size: 12px; }
            QLabel#startVersion { color: #7d8b92; font-size: 11px; }
            QToolButton#startMotionToggle {
                background: rgba(255, 255, 255, 225);
                color: #5c707b; border: 1px solid #b8c7cf;
                border-radius: 13px; padding: 5px 12px;
                font-size: 10px; font-weight: 600;
            }
            QToolButton#startMotionToggle:hover {
                background: #edf6fa; border-color: #5799b9;
                color: #244653;
            }
            QToolButton#startMotionToggle:checked {
                background: #e3f7ef; color: #176a4d;
                border-color: #3db78b;
            }
            QFrame#startHero {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                            stop:0 #ffffff, stop:1 #f5f8f9);
                border: 1px solid #bcc8ce;
                border-left: 2px solid #287da6;
                border-radius: 12px;
            }
            QLabel#startWelcome {
                color: #153847; font-size: 26px; font-weight: 600;
            }
            QLabel#startDescription {
                color: #5c707a; font-size: 13px; max-width: 520px;
            }
            QPushButton#startOpenButton {
                background: #1f6788; color: #ffffff;
                border: 1px solid #287da6; border-radius: 8px;
                padding: 9px 16px; font-size: 13px; font-weight: 600;
                text-align: left;
            }
            QPushButton#startOpenButton:hover {
                background: #287da6; border-color: #155f86;
            }
            QPushButton#startOpenButton:pressed { background: #185573; }
            QLabel#startSectionTitle {
                color: #203b49; font-size: 16px; font-weight: 600;
            }
            QLabel#startRecentHint { color: #71818a; font-size: 11px; }
            QScrollArea#startRecentScroll, QWidget#startRecentHost {
                background: transparent; border: none;
            }
            QPushButton#recentCard {
                background: #ffffff; border: 1px solid #c4ced3;
                border-radius: 10px; padding: 0; text-align: left;
            }
            QPushButton#recentCard:hover {
                background: #f7fafb; border-color: #5b9fc2;
            }
            QPushButton#recentCard:pressed { background: #edf3f6; }
            QLabel#recentPreview {
                background: #eef2f4;
                border: 1px solid #c9d4da;
                border-radius: 6px;
            }
            QLabel#recentTitle {
                color: #203b49; font-size: 12px; font-weight: 600;
            }
            QLabel#recentPath, QLabel#recentDate {
                color: #687b84; font-size: 10px;
            }
            QToolButton#recentRemove {
                background: transparent; color: #73858e;
                border: 1px solid transparent; border-radius: 9px;
                font-size: 15px; padding: 0;
            }
            QToolButton#recentRemove:hover {
                background: #fbe9e8; color: #a22d28;
                border-color: #d5a29f;
            }
            QLabel#startEmpty {
                color: #6b7c84; font-size: 12px; padding: 26px;
                border: 1px dashed #b8c4ca; background: #ffffff;
            }
            QLabel#startFooter { color: #7c8c94; font-size: 10px; }
        )"));
    }
    update();
}

void StartScreen::setMotionEnabled(bool enabled)
{
    m_motionEnabled = enabled;
    if (m_motionToggle) {
        const QSignalBlocker blocker(m_motionToggle);
        m_motionToggle->setChecked(enabled);
        m_motionToggle->setText(
            enabled ? QStringLiteral("动效  开")
                    : QStringLiteral("动效  关"));
    }
    if (m_background) {
        m_background->setActive(enabled);
    }
    update();
}

void StartScreen::setRecentDrawings(
    const QVector<RecentDrawing> &drawings)
{
    m_recentDrawings = drawings;
    m_emptyLabel->setVisible(m_recentDrawings.isEmpty());
    m_scrollArea->setVisible(!m_recentDrawings.isEmpty());
    m_columnCount = 0;
    rebuildRecentCards();
}

int StartScreen::columnCountForWidth(int width) const
{
    Q_UNUSED(width);
    return 4;
}

void StartScreen::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_background) {
        m_background->setGeometry(rect());
        m_background->lower();
        m_background->setActive(m_motionEnabled);
    }
}

void StartScreen::hideEvent(QHideEvent *event)
{
    if (m_background) {
        m_background->setActive(false);
    }
    QWidget::hideEvent(event);
}

void StartScreen::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_background) {
        m_background->setGeometry(rect());
        m_background->lower();
    }
    const int columns = columnCountForWidth(width());
    if (columns != m_columnCount) {
        m_columnCount = columns;
        rebuildRecentCards();
    }
}

void StartScreen::rebuildRecentCards()
{
    if (!m_recentGrid) {
        return;
    }
    while (QLayoutItem *item = m_recentGrid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_recentDrawings.isEmpty()) {
        return;
    }
    const int columns = m_columnCount > 0
        ? m_columnCount : columnCountForWidth(width());
    m_columnCount = columns;
    for (int i = 0; i < m_recentDrawings.size(); ++i) {
        const RecentDrawing &drawing = m_recentDrawings[i];
        auto *card = new QPushButton;
        card->setObjectName(QStringLiteral("recentCard"));
        card->setCursor(Qt::PointingHandCursor);
        card->setMinimumSize(185, 196);
        card->setMaximumWidth(216);
        card->setFocusPolicy(Qt::StrongFocus);
        card->setToolTip(drawing.path);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(7, 7, 7, 10);
        cardLayout->setSpacing(6);
        auto *preview = new QLabel;
        preview->setObjectName(QStringLiteral("recentPreview"));
        preview->setFixedHeight(104);
        preview->setAlignment(Qt::AlignCenter);
        preview->setPixmap(thumbnailPixmap(drawing, QSize(204, 102)));
        cardLayout->addWidget(preview);
        auto *title = new QLabel;
        title->setObjectName(QStringLiteral("recentTitle"));
        title->setText(elided(drawing.title, 250, title->font()));
        cardLayout->addWidget(title);
        auto *path = new QLabel;
        path->setObjectName(QStringLiteral("recentPath"));
        path->setText(elided(QFileInfo(drawing.path).absolutePath(),
                             250, path->font()));
        cardLayout->addWidget(path);
        auto *footerRow = new QHBoxLayout;
        footerRow->setContentsMargins(0, 0, 0, 0);
        footerRow->setSpacing(4);
        auto *date = new QLabel;
        date->setObjectName(QStringLiteral("recentDate"));
        date->setText(
            drawing.lastOpened.isValid()
                ? drawing.lastOpened.toString(
                      QStringLiteral("yyyy-MM-dd  HH:mm"))
                : QString());
        footerRow->addWidget(date, 1);
        auto *remove = new QToolButton(card);
        remove->setObjectName(QStringLiteral("recentRemove"));
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(QStringLiteral("从最近打开列表中移除"));
        remove->setAccessibleName(QStringLiteral("从最近打开列表中移除"));
        remove->setFixedSize(20, 20);
        remove->setCursor(Qt::PointingHandCursor);
        footerRow->addWidget(remove);
        cardLayout->addLayout(footerRow);
        connect(remove, &QToolButton::clicked, this,
                [this, path = drawing.path] {
                    emit removeRecentRequested(path);
                });
        connect(card, &QPushButton::clicked, this,
                [this, path = drawing.path] {
                    emit openRequested(path);
                });
        m_recentGrid->addWidget(card, i / columns, i % columns);
    }
    for (int column = 0; column < columns; ++column) {
        m_recentGrid->setColumnStretch(column, 1);
    }
}
