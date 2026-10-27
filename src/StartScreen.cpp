#include "StartScreen.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

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

StartScreen::StartScreen(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("startScreen"));
    setMinimumSize(960, 540);

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
        QFrame#startHero {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                        stop:0 #11191e, stop:1 #0d1215);
            border: 1px solid #2b3942; border-left: 2px solid #5da7d3;
            border-radius: 2px;
        }
        QLabel#startWelcome {
            color: #eef5f8; font-size: 26px; font-weight: 600;
        }
        QLabel#startDescription {
            color: #8f9da6; font-size: 13px; max-width: 520px;
        }
        QPushButton#startOpenButton {
            background: #1b526d; color: #f2f8fb;
            border: 1px solid #67b2d8; border-radius: 2px;
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
            border-radius: 2px; padding: 0; text-align: left;
        }
        QPushButton#recentCard:hover {
            background: #162229; border-color: #5b9ec0;
        }
        QPushButton#recentCard:pressed { background: #0c1215; }
        QLabel#recentTitle {
            color: #dce5e9; font-size: 12px; font-weight: 600;
        }
        QLabel#recentPath, QLabel#recentDate {
            color: #667680; font-size: 10px;
        }
        QToolButton#recentRemove {
            background: transparent; color: #74858e;
            border: 1px solid transparent; border-radius: 2px;
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
            QFrame#startHero {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                            stop:0 #ffffff, stop:1 #f5f8f9);
                border: 1px solid #bcc8ce;
                border-left: 2px solid #287da6;
                border-radius: 2px;
            }
            QLabel#startWelcome {
                color: #153847; font-size: 26px; font-weight: 600;
            }
            QLabel#startDescription {
                color: #5c707a; font-size: 13px; max-width: 520px;
            }
            QPushButton#startOpenButton {
                background: #1f6788; color: #ffffff;
                border: 1px solid #287da6; border-radius: 2px;
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
                border-radius: 2px; padding: 0; text-align: left;
            }
            QPushButton#recentCard:hover {
                background: #f7fafb; border-color: #5b9fc2;
            }
            QPushButton#recentCard:pressed { background: #edf3f6; }
            QLabel#recentTitle {
                color: #203b49; font-size: 12px; font-weight: 600;
            }
            QLabel#recentPath, QLabel#recentDate {
                color: #687b84; font-size: 10px;
            }
            QToolButton#recentRemove {
                background: transparent; color: #73858e;
                border: 1px solid transparent; border-radius: 2px;
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

void StartScreen::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
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
