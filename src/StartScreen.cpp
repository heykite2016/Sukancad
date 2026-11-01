#include "StartScreen.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {

constexpr int kPreviewWidth = 296;
constexpr int kPreviewHeight = 172;

QPixmap placeholderThumbnail(const QSize &size, const QColor &line)
{
    QPixmap canvas(size);
    canvas.fill(QColor(QStringLiteral("#080b0e")));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(43, 57, 66), 0.7));
    for (int x = 14; x < size.width(); x += 26) {
        painter.drawLine(x, 0, x, size.height());
    }
    for (int y = 14; y < size.height(); y += 26) {
        painter.drawLine(0, y, size.width(), y);
    }
    const double w = qMin(size.width() * 0.42, 120.0);
    const double h = w * 1.18;
    const QRectF sheet((size.width() - w) / 2.0,
                       (size.height() - h) / 2.0 - 4.0, w, h);
    painter.setPen(QPen(line, 1.6));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(sheet);
    painter.drawLine(sheet.left() + w * 0.16, sheet.top() + h * 0.30,
                     sheet.right() - w * 0.16, sheet.top() + h * 0.30);
    painter.drawLine(sheet.left() + w * 0.16, sheet.top() + h * 0.52,
                     sheet.right() - w * 0.16, sheet.top() + h * 0.52);
    painter.drawLine(sheet.left() + w * 0.16, sheet.top() + h * 0.74,
                     sheet.left() + w * 0.62, sheet.top() + h * 0.74);
    return canvas;
}

QPixmap thumbnailPixmap(const RecentDrawing &drawing, const QSize &size,
                        const QColor &line)
{
    QPixmap source;
    if (!drawing.thumbnailPath.isEmpty()) {
        source.load(drawing.thumbnailPath);
    }
    if (source.isNull()) {
        return placeholderThumbnail(size, line);
    }
    QPixmap result(size);
    result.fill(QColor(QStringLiteral("#080b0e")));
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QPixmap scaled = source.scaled(
        size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    painter.drawPixmap(
        QPoint((size.width() - scaled.width()) / 2,
               (size.height() - scaled.height()) / 2),
        scaled);
    return result;
}

QString formatFileSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QStringLiteral("—");
    }
    const double kilobytes = bytes / 1024.0;
    if (kilobytes < 1024.0) {
        return QStringLiteral("%1 KB").arg(kilobytes, 0, 'f', 0);
    }
    const double megabytes = kilobytes / 1024.0;
    if (megabytes < 1024.0) {
        return QStringLiteral("%1 MB").arg(megabytes, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(megabytes / 1024.0, 0, 'f', 2);
}

QString drawingKind(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toUpper();
    return suffix.isEmpty() ? QStringLiteral("图纸") : suffix;
}

} // namespace

StartScreen::StartScreen(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("startScreen"));
    setMinimumSize(960, 560);
    setAttribute(Qt::WA_StyledBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 顶部品牌条 ------------------------------------------------------
    auto *header = new QFrame;
    header->setObjectName(QStringLiteral("startHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(26, 13, 26, 13);
    headerLayout->setSpacing(11);
    auto *logo = new QLabel;
    logo->setFixedSize(28, 28);
    logo->setPixmap(QIcon(QStringLiteral(":/icons/logo.svg")).pixmap(28, 28));
    headerLayout->addWidget(logo);
    auto *brand = new QLabel(QStringLiteral("速看CAD"));
    brand->setObjectName(QStringLiteral("startBrand"));
    headerLayout->addWidget(brand);
    auto *edition = new QLabel(QStringLiteral("工程看图 · 测量标注 · 命令模式"));
    edition->setObjectName(QStringLiteral("startEdition"));
    headerLayout->addWidget(edition);
    headerLayout->addStretch(1);
    auto *version = new QLabel(
        QStringLiteral("v") + QCoreApplication::applicationVersion());
    version->setObjectName(QStringLiteral("startVersion"));
    headerLayout->addWidget(version);
    root->addWidget(header);

    // ---- 主体 -----------------------------------------------------------
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("startBody"));
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(26, 22, 26, 18);
    bodyLayout->setSpacing(22);

    auto *rail = new QWidget;
    rail->setObjectName(QStringLiteral("startRail"));
    rail->setFixedWidth(236);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 0, 0, 0);
    railLayout->setSpacing(7);

    auto *startTitle = new QLabel(QStringLiteral("开始"));
    startTitle->setObjectName(QStringLiteral("startRailTitle"));
    railLayout->addWidget(startTitle);
    railLayout->addSpacing(2);

    auto *openButton = new QPushButton(
        QIcon(QStringLiteral(":/icons/open.svg")),
        QStringLiteral("  打开图纸…"));
    openButton->setObjectName(QStringLiteral("startPrimary"));
    openButton->setCursor(Qt::PointingHandCursor);
    openButton->setMinimumHeight(38);
    openButton->setToolTip(
        QStringLiteral("打开 DWG / DXF / PDF 图纸 (Ctrl+O)"));
    connect(openButton, &QPushButton::clicked, this,
            &StartScreen::browseRequested);
    railLayout->addWidget(openButton);

    auto *recentButton = new QPushButton(QStringLiteral("最近使用的图纸"));
    recentButton->setObjectName(QStringLiteral("startRailItem"));
    recentButton->setCursor(Qt::PointingHandCursor);
    connect(recentButton, &QPushButton::clicked, this, [this] {
        if (m_list && m_list->isVisible()) {
            m_list->setFocus();
            if (m_list->topLevelItemCount() > 0) {
                m_list->setCurrentItem(m_list->topLevelItem(0));
            }
        }
    });
    railLayout->addWidget(recentButton);

    auto *commandButton = new QPushButton(QStringLiteral("命令与快捷键"));
    commandButton->setObjectName(QStringLiteral("startRailItem"));
    commandButton->setCursor(Qt::PointingHandCursor);
    connect(commandButton, &QPushButton::clicked, this,
            &StartScreen::commandReferenceRequested);
    railLayout->addWidget(commandButton);

    railLayout->addSpacing(14);
    auto *railHint = new QLabel(
        QStringLiteral("支持格式\n"
                       "DWG · DXF · PDF\n\n"
                       "把图纸文件拖进窗口即可打开。\n"
                       "打开后可在底部命令行输入 ? 查看全部命令。"));
    railHint->setObjectName(QStringLiteral("startRailHint"));
    railHint->setWordWrap(true);
    railLayout->addWidget(railHint);
    railLayout->addStretch(1);
    auto *railFooter = new QLabel(QStringLiteral("本地读取 · 不上传图纸"));
    railFooter->setObjectName(QStringLiteral("startFooterNote"));
    railLayout->addWidget(railFooter);
    bodyLayout->addWidget(rail);

    auto *recentArea = new QWidget;
    auto *recentLayout = new QVBoxLayout(recentArea);
    recentLayout->setContentsMargins(0, 0, 0, 0);
    recentLayout->setSpacing(9);
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    auto *recentTitle = new QLabel(QStringLiteral("最近的图纸"));
    recentTitle->setObjectName(QStringLiteral("startSectionTitle"));
    titleRow->addWidget(recentTitle);
    titleRow->addStretch(1);
    m_countLabel = new QLabel;
    m_countLabel->setObjectName(QStringLiteral("startRecentHint"));
    titleRow->addWidget(m_countLabel);

    m_clearRecent = new QPushButton(QStringLiteral("清除列表"));
    m_clearRecent->setObjectName(QStringLiteral("startClearRecent"));
    m_clearRecent->setCursor(Qt::PointingHandCursor);
    m_clearRecent->setToolTip(
        QStringLiteral("一键清空“最近的图纸”列表（不会删除图纸文件）"));
    m_clearRecent->setFocusPolicy(Qt::NoFocus);
    connect(m_clearRecent, &QPushButton::clicked, this,
            &StartScreen::clearRecentRequested);
    titleRow->addWidget(m_clearRecent);
    recentLayout->addLayout(titleRow);

    // 列表 / 空状态共用一个栈：没有最近图纸时，空状态只占据列表的位置，
    // 右侧预览栏保持原位，不会掉到下面去。
    m_listStack = new QStackedWidget;
    m_listStack->setObjectName(QStringLiteral("startListStack"));

    m_emptyLabel = new QLabel(
        QStringLiteral("还没有最近打开的图纸\n\n"
                       "点击左侧「打开图纸…」，或把 DWG / DXF / PDF "
                       "文件拖到窗口里"));
    m_emptyLabel->setObjectName(QStringLiteral("startEmpty"));
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);

    auto *content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(18);

    m_list = new QTreeWidget;
    m_list->setObjectName(QStringLiteral("recentList"));
    m_list->setColumnCount(3);
    m_list->setHeaderLabels({QStringLiteral("名称"),
                             QStringLiteral("位置"),
                             QStringLiteral("上次打开")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAlternatingRowColors(true);
    m_list->setAllColumnsShowFocus(true);
    m_list->setIconSize(QSize(46, 30));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSortingEnabled(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_list->header()->resizeSection(2, 132);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSortIndicatorShown(true);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QList<QTreeWidgetItem *> selection = m_list->selectedItems();
        if (selection.isEmpty()) {
            updatePreview(QString());
            return;
        }
        updatePreview(
            selection.first()->data(0, Qt::UserRole).toString());
    });
    connect(m_list, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *, int) { openSelected(); });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { openSelected(); });
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(m_emptyLabel);
    content->addWidget(m_listStack, 1);

    auto *preview = new QFrame;
    preview->setObjectName(QStringLiteral("startPreview"));
    preview->setFixedWidth(326);
    auto *previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(14, 14, 14, 14);
    previewLayout->setSpacing(9);
    m_previewImage = new QLabel;
    m_previewImage->setObjectName(QStringLiteral("startPreviewImage"));
    m_previewImage->setFixedHeight(kPreviewHeight + 4);
    m_previewImage->setAlignment(Qt::AlignCenter);
    previewLayout->addWidget(m_previewImage);
    m_previewTitle = new QLabel;
    m_previewTitle->setObjectName(QStringLiteral("startPreviewTitle"));
    m_previewTitle->setWordWrap(true);
    previewLayout->addWidget(m_previewTitle);
    m_previewPath = new QLabel;
    m_previewPath->setObjectName(QStringLiteral("startPreviewPath"));
    m_previewPath->setWordWrap(true);
    previewLayout->addWidget(m_previewPath);
    m_previewMeta = new QLabel;
    m_previewMeta->setObjectName(QStringLiteral("startPreviewMeta"));
    m_previewMeta->setWordWrap(true);
    previewLayout->addWidget(m_previewMeta);
    m_previewPlaceholder = new QLabel(
        QStringLiteral("未选择图纸\n\n"
                       "在上方列表中单击一张图纸查看预览，双击直接打开。"));
    m_previewPlaceholder->setObjectName(
        QStringLiteral("startPreviewPlaceholder"));
    m_previewPlaceholder->setAlignment(Qt::AlignCenter);
    m_previewPlaceholder->setWordWrap(true);
    previewLayout->addWidget(m_previewPlaceholder, 1);
    auto *previewButtons = new QHBoxLayout;
    previewButtons->setContentsMargins(0, 0, 0, 0);
    previewButtons->setSpacing(8);
    m_previewOpen = new QPushButton(QStringLiteral("打开"));
    m_previewOpen->setObjectName(QStringLiteral("startPreviewOpen"));
    m_previewOpen->setCursor(Qt::PointingHandCursor);
    m_previewRemove = new QPushButton(QStringLiteral("移出列表"));
    m_previewRemove->setObjectName(QStringLiteral("startPreviewRemove"));
    m_previewRemove->setCursor(Qt::PointingHandCursor);
    m_previewRemove->setToolTip(QStringLiteral("从最近打开列表中移除（不删除文件）"));
    previewButtons->addWidget(m_previewOpen, 1);
    previewButtons->addWidget(m_previewRemove);
    previewLayout->addLayout(previewButtons);
    connect(m_previewOpen, &QPushButton::clicked, this,
            &StartScreen::openSelected);
    connect(m_previewRemove, &QPushButton::clicked, this,
            &StartScreen::removeSelected);
    content->addWidget(preview, 0);

    recentLayout->addLayout(content, 1);
    bodyLayout->addWidget(recentArea, 1);
    root->addWidget(body, 1);

    applyTheme();
    updatePreview(QString());
}

void StartScreen::setRecentDrawings(
    const QVector<RecentDrawing> &drawings)
{
    m_recentDrawings = drawings;
    rebuildRecentList();
}

void StartScreen::rebuildRecentList()
{
    if (!m_list) {
        return;
    }
    const bool hasDrawings = !m_recentDrawings.isEmpty();
    if (m_listStack) {
        m_listStack->setCurrentWidget(
            hasDrawings ? static_cast<QWidget *>(m_list) : m_emptyLabel);
    }
    if (m_clearRecent) {
        m_clearRecent->setVisible(hasDrawings);
    }
    m_list->setVisible(hasDrawings);
    m_emptyLabel->setVisible(!hasDrawings);
    m_countLabel->setText(
        hasDrawings
            ? QStringLiteral("共 %1 个 · 双击打开 · 单击预览")
                  .arg(m_recentDrawings.size())
            : QStringLiteral("暂无记录"));

    const QString previousPath = [this] {
        const QList<QTreeWidgetItem *> selection = m_list->selectedItems();
        return selection.isEmpty()
            ? QString() : selection.first()->data(0, Qt::UserRole).toString();
    }();

    const bool sorting = m_list->isSortingEnabled();
    m_list->setSortingEnabled(false);
    m_list->clear();
    for (const RecentDrawing &drawing : std::as_const(m_recentDrawings)) {
        const QFileInfo info(drawing.path);
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, drawing.title);
        item->setIcon(
            0, QIcon(thumbnailPixmap(drawing, QSize(46, 30),
                                     m_darkTheme
                                         ? QColor(QStringLiteral("#5fb6e0"))
                                         : QColor(QStringLiteral("#2b7fa6")))));
        item->setText(1, info.absolutePath());
        item->setText(
            2, drawing.lastOpened.isValid()
                   ? drawing.lastOpened.toString(
                         QStringLiteral("yyyy-MM-dd  HH:mm"))
                   : QStringLiteral("—"));
        item->setData(0, Qt::UserRole, drawing.path);
        item->setToolTip(0, drawing.path);
        item->setToolTip(1, info.absolutePath());
        item->setSizeHint(0, QSize(0, 36));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    }
    m_list->setSortingEnabled(sorting);
    m_list->sortByColumn(2, Qt::DescendingOrder);

    if (!previousPath.isEmpty()) {
        for (int index = 0; index < m_list->topLevelItemCount(); ++index) {
            QTreeWidgetItem *item = m_list->topLevelItem(index);
            if (item->data(0, Qt::UserRole).toString() == previousPath) {
                m_list->setCurrentItem(item);
                break;
            }
        }
    }
    if (m_list->selectedItems().isEmpty() && hasDrawings) {
        m_list->setCurrentItem(m_list->topLevelItem(0));
    }
    if (!hasDrawings) {
        updatePreview(QString());
    }
}

void StartScreen::updatePreview(const QString &path)
{
    const RecentDrawing *drawing = nullptr;
    for (const RecentDrawing &candidate : std::as_const(m_recentDrawings)) {
        if (candidate.path == path) {
            drawing = &candidate;
            break;
        }
    }
    const bool hasDrawing = drawing != nullptr;
    m_previewPlaceholder->setVisible(!hasDrawing);
    m_previewImage->setVisible(hasDrawing);
    m_previewTitle->setVisible(hasDrawing);
    m_previewPath->setVisible(hasDrawing);
    m_previewMeta->setVisible(hasDrawing);
    m_previewOpen->setEnabled(hasDrawing);
    m_previewRemove->setEnabled(hasDrawing);
    if (!hasDrawing) {
        m_previewImage->clear();
        m_previewTitle->clear();
        m_previewPath->clear();
        m_previewMeta->clear();
        return;
    }
    const QFileInfo info(drawing->path);
    m_previewImage->setPixmap(
        thumbnailPixmap(*drawing, QSize(kPreviewWidth, kPreviewHeight),
                        m_darkTheme ? QColor(QStringLiteral("#5fb6e0"))
                                    : QColor(QStringLiteral("#2b7fa6"))));
    m_previewTitle->setText(drawing->title);
    m_previewPath->setText(info.absolutePath());
    m_previewMeta->setText(
        QStringLiteral("%1 · %2 · %3")
            .arg(drawingKind(drawing->path),
                 formatFileSize(info.size()),
                 drawing->lastOpened.isValid()
                     ? drawing->lastOpened.toString(
                           QStringLiteral("yyyy-MM-dd HH:mm"))
                     : QStringLiteral("—")));
}

void StartScreen::openSelected()
{
    const QList<QTreeWidgetItem *> selection = m_list->selectedItems();
    if (selection.isEmpty()) {
        return;
    }
    const QString path = selection.first()->data(0, Qt::UserRole).toString();
    if (!path.isEmpty()) {
        emit openRequested(path);
    }
}

void StartScreen::removeSelected()
{
    const QList<QTreeWidgetItem *> selection = m_list->selectedItems();
    if (selection.isEmpty()) {
        return;
    }
    const QString path = selection.first()->data(0, Qt::UserRole).toString();
    if (!path.isEmpty()) {
        emit removeRecentRequested(path);
    }
}

void StartScreen::setMotionEnabled(bool enabled)
{
    m_motionEnabled = enabled;
}

void StartScreen::setCadTheme(bool dark)
{
    if (m_darkTheme == dark && !styleSheet().isEmpty()) {
        return;
    }
    m_darkTheme = dark;
    applyTheme();
    if (m_list && m_list->topLevelItemCount() > 0) {
        rebuildRecentList();
    }
}

void StartScreen::applyTheme()
{
    QString sheet;
    if (m_darkTheme) {
        sheet = QStringLiteral(R"(
            QWidget#startScreen { background: @bg; color: @text; }
            QWidget#startBody { background: transparent; }
            QFrame#startHeader {
                background: @header; border-bottom: 1px solid @border;
            }
            QLabel#startBrand {
                color: @text; font-size: 18px; font-weight: 700;
                letter-spacing: 2px;
            }
            QLabel#startEdition { color: @muted; font-size: 11px; }
            QLabel#startVersion { color: @muted; font-size: 11px; }
            QWidget#startRail { background: transparent; }
            QLabel#startRailTitle {
                color: @muted; font-size: 11px; font-weight: 600;
                letter-spacing: 2px;
            }
            QPushButton#startPrimary {
                background: @accent; color: @accentText;
                border: 1px solid @accent; border-radius: 6px;
                padding: 9px 14px; font-size: 13px; font-weight: 600;
                text-align: left;
            }
            QPushButton#startPrimary:hover { background: @accentHover; }
            QPushButton#startPrimary:pressed { background: @accent; }
            QPushButton#startRailItem {
                background: transparent; color: @text;
                border: 1px solid transparent; border-radius: 6px;
                padding: 8px 12px; font-size: 12px; text-align: left;
            }
            QPushButton#startRailItem:hover {
                background: @hover; border-color: @border;
            }
            QLabel#startRailHint { color: @muted; font-size: 11px; }
            QLabel#startFooterNote { color: @muted; font-size: 10px; }
            QLabel#startSectionTitle {
                color: @text; font-size: 14px; font-weight: 600;
            }
            QLabel#startRecentHint { color: @muted; font-size: 11px; }
            QPushButton#startClearRecent {
                background: transparent; color: @muted;
                border: 1px solid @border; border-radius: 5px;
                padding: 3px 10px; font-size: 11px;
            }
            QPushButton#startClearRecent:hover {
                background: @hover; color: @text; border-color: @muted;
            }
            QPushButton#startClearRecent:pressed { background: @panel; }
            QTreeWidget#recentList {
                background: @panel; border: 1px solid @border;
                border-radius: 8px; color: @text;
                alternate-background-color: @alt; outline: none;
            }
            QTreeWidget#recentList::item { padding: 5px 4px; }
            QTreeWidget#recentList::item:hover { background: @hover; }
            QTreeWidget#recentList::item:selected {
                background: @selected; color: @text;
            }
            QTreeWidget#recentList QHeaderView::section {
                background: @header; color: @muted; border: none;
                border-bottom: 1px solid @border;
                padding: 7px 8px; font-size: 11px;
            }
            QFrame#startPreview {
                background: @panel; border: 1px solid @border;
                border-radius: 8px;
            }
            QLabel#startPreviewImage {
                background: @preview; border: 1px solid @border;
                border-radius: 6px;
            }
            QLabel#startPreviewTitle {
                color: @text; font-size: 14px; font-weight: 600;
            }
            QLabel#startPreviewPath, QLabel#startPreviewMeta {
                color: @muted; font-size: 11px;
            }
            QLabel#startPreviewPlaceholder {
                color: @muted; font-size: 12px;
            }
            QPushButton#startPreviewOpen {
                background: @accent; color: @accentText;
                border: 1px solid @accent; border-radius: 6px;
                padding: 7px 14px; font-weight: 600;
            }
            QPushButton#startPreviewOpen:hover { background: @accentHover; }
            QPushButton#startPreviewRemove {
                background: transparent; color: @muted;
                border: 1px solid @border; border-radius: 6px;
                padding: 7px 12px;
            }
            QPushButton#startPreviewRemove:hover {
                background: @hover; color: @text;
            }
            QPushButton#startPreviewOpen:disabled,
            QPushButton#startPreviewRemove:disabled { color: @muted; }
            QLabel#startEmpty {
                color: @muted; font-size: 12px;
                border: 1px dashed @border; border-radius: 8px;
                background: @panel; padding: 28px;
            }
        )");
        sheet.replace(QStringLiteral("@bg"), QStringLiteral("#0c1116"));
        sheet.replace(QStringLiteral("@header"), QStringLiteral("#111a20"));
        sheet.replace(QStringLiteral("@panel"), QStringLiteral("#0f171c"));
        sheet.replace(QStringLiteral("@alt"), QStringLiteral("#121c22"));
        sheet.replace(QStringLiteral("@preview"), QStringLiteral("#080c10"));
        sheet.replace(QStringLiteral("@border"), QStringLiteral("#26333c"));
        sheet.replace(QStringLiteral("@text"), QStringLiteral("#d9e2e8"));
        sheet.replace(QStringLiteral("@muted"), QStringLiteral("#7f8d97"));
        sheet.replace(QStringLiteral("@hover"), QStringLiteral("#18252d"));
        sheet.replace(QStringLiteral("@selected"), QStringLiteral("#1d3a4a"));
        // 注意：@accentHover / @accentText 必须先于 @accent 替换，
        // 否则 @accent 会吞掉前缀，留下 "…Hover" 之类的非法色值。
        sheet.replace(QStringLiteral("@accentHover"), QStringLiteral("#3497c4"));
        sheet.replace(QStringLiteral("@accentText"), QStringLiteral("#f2fbff"));
        sheet.replace(QStringLiteral("@accent"), QStringLiteral("#2b7fa6"));
    } else {
        sheet = QStringLiteral(R"(
            QWidget#startScreen { background: @bg; color: @text; }
            QWidget#startBody { background: transparent; }
            QFrame#startHeader {
                background: @header; border-bottom: 1px solid @border;
            }
            QLabel#startBrand {
                color: @text; font-size: 18px; font-weight: 700;
                letter-spacing: 2px;
            }
            QLabel#startEdition { color: @muted; font-size: 11px; }
            QLabel#startVersion { color: @muted; font-size: 11px; }
            QWidget#startRail { background: transparent; }
            QLabel#startRailTitle {
                color: @muted; font-size: 11px; font-weight: 600;
                letter-spacing: 2px;
            }
            QPushButton#startPrimary {
                background: @accent; color: @accentText;
                border: 1px solid @accent; border-radius: 6px;
                padding: 9px 14px; font-size: 13px; font-weight: 600;
                text-align: left;
            }
            QPushButton#startPrimary:hover { background: @accentHover; }
            QPushButton#startRailItem {
                background: transparent; color: @text;
                border: 1px solid transparent; border-radius: 6px;
                padding: 8px 12px; font-size: 12px; text-align: left;
            }
            QPushButton#startRailItem:hover {
                background: @hover; border-color: @border;
            }
            QLabel#startRailHint { color: @muted; font-size: 11px; }
            QLabel#startFooterNote { color: @muted; font-size: 10px; }
            QLabel#startSectionTitle {
                color: @text; font-size: 14px; font-weight: 600;
            }
            QLabel#startRecentHint { color: @muted; font-size: 11px; }
            QPushButton#startClearRecent {
                background: transparent; color: @muted;
                border: 1px solid @border; border-radius: 5px;
                padding: 3px 10px; font-size: 11px;
            }
            QPushButton#startClearRecent:hover {
                background: @hover; color: @text; border-color: @muted;
            }
            QPushButton#startClearRecent:pressed { background: @panel; }
            QTreeWidget#recentList {
                background: @panel; border: 1px solid @border;
                border-radius: 8px; color: @text;
                alternate-background-color: @alt; outline: none;
            }
            QTreeWidget#recentList::item { padding: 5px 4px; }
            QTreeWidget#recentList::item:hover { background: @hover; }
            QTreeWidget#recentList::item:selected {
                background: @selected; color: @text;
            }
            QTreeWidget#recentList QHeaderView::section {
                background: @header; color: @muted; border: none;
                border-bottom: 1px solid @border;
                padding: 7px 8px; font-size: 11px;
            }
            QFrame#startPreview {
                background: @panel; border: 1px solid @border;
                border-radius: 8px;
            }
            QLabel#startPreviewImage {
                background: @preview; border: 1px solid @border;
                border-radius: 6px;
            }
            QLabel#startPreviewTitle {
                color: @text; font-size: 14px; font-weight: 600;
            }
            QLabel#startPreviewPath, QLabel#startPreviewMeta {
                color: @muted; font-size: 11px;
            }
            QLabel#startPreviewPlaceholder {
                color: @muted; font-size: 12px;
            }
            QPushButton#startPreviewOpen {
                background: @accent; color: @accentText;
                border: 1px solid @accent; border-radius: 6px;
                padding: 7px 14px; font-weight: 600;
            }
            QPushButton#startPreviewOpen:hover { background: @accentHover; }
            QPushButton#startPreviewRemove {
                background: transparent; color: @muted;
                border: 1px solid @border; border-radius: 6px;
                padding: 7px 12px;
            }
            QPushButton#startPreviewRemove:hover {
                background: @hover; color: @text;
            }
            QLabel#startEmpty {
                color: @muted; font-size: 12px;
                border: 1px dashed @border; border-radius: 8px;
                background: @panel; padding: 28px;
            }
        )");
        sheet.replace(QStringLiteral("@bg"), QStringLiteral("#f4f6f8"));
        sheet.replace(QStringLiteral("@header"), QStringLiteral("#ffffff"));
        sheet.replace(QStringLiteral("@panel"), QStringLiteral("#ffffff"));
        sheet.replace(QStringLiteral("@alt"), QStringLiteral("#f7fafb"));
        sheet.replace(QStringLiteral("@preview"), QStringLiteral("#eef3f6"));
        sheet.replace(QStringLiteral("@border"), QStringLiteral("#cfd9de"));
        sheet.replace(QStringLiteral("@text"), QStringLiteral("#1b2a33"));
        sheet.replace(QStringLiteral("@muted"), QStringLiteral("#5f7079"));
        sheet.replace(QStringLiteral("@hover"), QStringLiteral("#eef4f8"));
        sheet.replace(QStringLiteral("@selected"), QStringLiteral("#dceaf3"));
        // 注意：@accentHover / @accentText 必须先于 @accent 替换，
        // 否则 @accent 会吞掉前缀，留下 "…Hover" 之类的非法色值。
        sheet.replace(QStringLiteral("@accentHover"), QStringLiteral("#2881ac"));
        sheet.replace(QStringLiteral("@accentText"), QStringLiteral("#ffffff"));
        sheet.replace(QStringLiteral("@accent"), QStringLiteral("#1f6c92"));
    }
    setStyleSheet(sheet);
    update();
}

void StartScreen::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_list && m_list->topLevelItemCount() > 0
        && m_list->selectedItems().isEmpty()) {
        m_list->setCurrentItem(m_list->topLevelItem(0));
    }
}
