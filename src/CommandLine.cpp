#include "CommandLine.h"

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kMaxVisibleSuggestions = 8;
}

CommandLine::CommandLine(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("commandPanel"));
    setFocusPolicy(Qt::ClickFocus);
    setAttribute(Qt::WA_StyledBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(9, 5, 9, 6);
    root->setSpacing(3);

    m_log = new QTextEdit(this);
    m_log->setObjectName(QStringLiteral("commandLog"));
    m_log->setReadOnly(true);
    m_log->setFrameShape(QFrame::NoFrame);
    m_log->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_log->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_log->setLineWrapMode(QTextEdit::WidgetWidth);
    m_log->setUndoRedoEnabled(false);
    m_log->setFocusPolicy(Qt::NoFocus);
    m_log->setFixedHeight(72);
    QFont logFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    logFont.setPointSizeF(10.5);
    m_log->setFont(logFont);
    m_log->document()->setMaximumBlockCount(400);
    m_log->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_log);

    auto *inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(0, 0, 0, 0);
    inputRow->setSpacing(7);

    m_prompt = new QLabel(QStringLiteral("命令:"), this);
    m_prompt->setObjectName(QStringLiteral("commandPrompt"));
    inputRow->addWidget(m_prompt, 0, Qt::AlignVCenter);

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("commandInput"));
    m_input->setPlaceholderText(
        QStringLiteral("输入命令（如 L 对齐、REC 矩形、Z 缩放、? 命令列表）"));
    m_input->setClearButtonEnabled(false);
    m_input->setMinimumHeight(26);
    m_input->installEventFilter(this);
    inputRow->addWidget(m_input, 1);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("commandHint"));
    m_hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_hint->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    inputRow->addWidget(m_hint, 0, Qt::AlignVCenter);

    m_expandButton = new QToolButton(this);
    m_expandButton->setObjectName(QStringLiteral("commandToggle"));
    m_expandButton->setText(QStringLiteral("▾"));
    m_expandButton->setToolTip(QStringLiteral("展开 / 收起命令历史 (Ctrl+9)"));
    m_expandButton->setAccessibleName(QStringLiteral("展开或收起命令历史"));
    m_expandButton->setCursor(Qt::PointingHandCursor);
    m_expandButton->setFixedSize(22, 22);
    m_expandButton->setFocusPolicy(Qt::NoFocus);
    connect(m_expandButton, &QToolButton::clicked, this, [this] {
        setExpanded(!m_expanded);
    });
    inputRow->addWidget(m_expandButton, 0, Qt::AlignVCenter);

    root->addLayout(inputRow);

    // 动态候选浮层：输入框上方弹出，不抢焦点，键盘 ↑↓ 选择、Tab 补全。
    m_suggestList = new QListWidget(nullptr);
    m_suggestList->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    m_suggestList->setObjectName(QStringLiteral("commandSuggestions"));
    m_suggestList->setAttribute(Qt::WA_ShowWithoutActivating, true);
    m_suggestList->setFocusPolicy(Qt::NoFocus);
    m_suggestList->setUniformItemSizes(true);
    m_suggestList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_suggestList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_suggestList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QFont suggestFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    suggestFont.setPointSizeF(11.0);
    m_suggestList->setFont(suggestFont);
    setSuggestionColors(m_panelColor, m_borderColor, m_textColor,
                        m_mutedColor, m_accentColor);

    connect(m_input, &QLineEdit::returnPressed, this, &CommandLine::submit);
    connect(m_input, &QLineEdit::textChanged, this,
            [this](const QString &text) { emit inputEdited(text); });

    m_textColor = QColor(QStringLiteral("#c8d3da"));
    setHint(QStringLiteral("就绪"));
}

CommandLine::~CommandLine()
{
    delete m_suggestList;
    m_suggestList = nullptr;
}

void CommandLine::setLogColors(const QColor &text, const QColor &muted,
                               const QColor &accent, const QColor &error,
                               const QColor &success)
{
    m_textColor = text;
    m_mutedColor = muted;
    m_accentColor = accent;
    m_errorColor = error;
    m_successColor = success;
    setSuggestionColors(m_panelColor, m_borderColor, m_textColor,
                        m_mutedColor, m_accentColor);
}

void CommandLine::setSuggestionColors(const QColor &panel,
                                      const QColor &border,
                                      const QColor &text,
                                      const QColor &muted,
                                      const QColor &accent)
{
    m_panelColor = panel;
    m_borderColor = border;
    if (!m_suggestList) {
        return;
    }
    m_suggestList->setStyleSheet(
        QStringLiteral(
            "QListWidget { background: %1; border: 1px solid %2;"
            " border-radius: 6px; padding: 4px; color: %3; outline: none; }"
            "QListWidget::item { padding: 4px 9px; border-radius: 4px; }"
            "QListWidget::item:selected { background: %5; color: %1; }"
            "QScrollBar:vertical { width: 8px; background: transparent; }"
            "QScrollBar::handle:vertical { background: %4; border-radius: 4px;"
            " min-height: 18px; }")
            .arg(panel.name(), border.name(), text.name(), muted.name(),
                 accent.name()));
}

const QColor &CommandLine::colorFor(MessageKind kind) const
{
    switch (kind) {
    case MessageKind::Echo:
        return m_accentColor;
    case MessageKind::Error:
        return m_errorColor;
    case MessageKind::Success:
        return m_successColor;
    case MessageKind::Hint:
        return m_mutedColor;
    case MessageKind::Info:
    default:
        return m_textColor;
    }
}

void CommandLine::appendMessage(const QString &text, MessageKind kind)
{
    if (!m_log || text.isEmpty()) {
        return;
    }
    const QString color = colorFor(kind).name(QColor::HexRgb);
    QString html = text.toHtmlEscaped();
    html.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    // 保留行首缩进，命令行里都是等宽对齐的列表
    html.replace(QStringLiteral("  "), QStringLiteral("&nbsp;&nbsp;"));
    m_log->append(QStringLiteral(
                      "<span style=\"color:%1;\">%2</span>")
                      .arg(color, html));
    scrollLogToBottom();
}

void CommandLine::appendCommandEcho(const QString &command)
{
    appendMessage(QStringLiteral("命令: %1").arg(command),
                  MessageKind::Echo);
}

void CommandLine::setHint(const QString &hint)
{
    if (m_hint) {
        m_hint->setText(hint);
        m_hint->setToolTip(hint);
    }
}

void CommandLine::setCompletions(const QStringList &names)
{
    m_completions = names;
}

void CommandLine::setSuggestions(
    const QVector<CommandSuggestion> &suggestions)
{
    m_suggestions = suggestions;
    if (!m_suggestList || !m_input) {
        return;
    }
    const bool shouldShow =
        !suggestions.isEmpty() && isVisible() && m_input->hasFocus();
    if (!shouldShow) {
        hideSuggestions();
        return;
    }
    m_suggestList->clear();
    for (const CommandSuggestion &suggestion : suggestions) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  %2   %3")
                .arg(suggestion.token.leftJustified(11, QLatin1Char(' ')),
                     suggestion.title, suggestion.detail),
            m_suggestList);
        item->setData(Qt::UserRole, suggestion.token);
    }
    m_suggestList->setCurrentRow(0);
    // 先显示再定位：setGeometry 前的 isVisible() 为 false 会导致浮层
    // 直接出现在 (0,0) 附近。
    m_suggestList->show();
    repositionSuggestions();
    m_suggestList->raise();
}

void CommandLine::hideSuggestions()
{
    if (m_suggestList) {
        m_suggestList->hide();
    }
}

void CommandLine::moveSuggestion(int delta)
{
    if (!m_suggestList || m_suggestList->count() == 0) {
        return;
    }
    const int row = qBound(0, m_suggestList->currentRow() + delta,
                           m_suggestList->count() - 1);
    m_suggestList->setCurrentRow(row);
    if (QListWidgetItem *item = m_suggestList->currentItem()) {
        m_suggestList->scrollToItem(item);
    }
}

void CommandLine::acceptSuggestion()
{
    if (!m_suggestList || !m_suggestList->isVisible() || !m_input) {
        return;
    }
    QListWidgetItem *item = m_suggestList->currentItem();
    if (!item) {
        return;
    }
    const QString token = item->data(Qt::UserRole).toString();
    hideSuggestions();
    if (token.isEmpty() || token == m_input->text()) {
        return;
    }
    m_input->setText(token);
    m_input->end(false);
}

void CommandLine::repositionSuggestions()
{
    if (!m_suggestList || !m_input || m_suggestList->count() == 0) {
        return;
    }
    const int rows =
        qBound(1, m_suggestList->count(), kMaxVisibleSuggestions);
    int rowHeight = m_suggestList->sizeHintForRow(0);
    if (rowHeight <= 0) {
        rowHeight = m_suggestList->fontMetrics().height() + 10;
    }
    const int height =
        rows * rowHeight + 2 * m_suggestList->frameWidth() + 10;
    const int width = qMax(380, m_input->width());
    const QPoint origin = m_input->mapToGlobal(QPoint(0, 0));
    QScreen *screen = QGuiApplication::screenAt(origin);
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    const QRect available =
        screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    int y = origin.y() - height - 4;
    if (y < available.top()) {
        y = origin.y() + m_input->height() + 4;
    }
    const int x = qBound(available.left(), origin.x(),
                         qMax(available.left(),
                              available.right() - width));
    m_suggestList->setGeometry(x, y, width, height);
}

void CommandLine::clearLog()
{
    if (m_log) {
        m_log->clear();
    }
}

void CommandLine::focusInput()
{
    if (!m_input) {
        return;
    }
    if (!m_expanded) {
        setExpanded(true);
    }
    m_input->setFocus(Qt::ShortcutFocusReason);
    m_input->end(false);
}

QString CommandLine::currentText() const
{
    return m_input ? m_input->text() : QString();
}

void CommandLine::typeIntoInput(const QString &text)
{
    if (!m_input || text.isEmpty()) {
        return;
    }
    if (QApplication::focusWidget() != m_input) {
        m_input->setFocus(Qt::OtherFocusReason);
        m_input->end(false);
    }
    if (QApplication::keyboardModifiers() == Qt::NoModifier) {
        // 画布里直接敲字母 → 清空旧内容重新输入，符合 CAD 习惯
        m_input->clear();
    }
    m_input->insert(text);
}

void CommandLine::setExpanded(bool expanded)
{
    if (m_expanded == expanded) {
        return;
    }
    m_expanded = expanded;
    applyExpansion();
    emit expandedChanged(m_expanded);
}

void CommandLine::applyExpansion()
{
    if (m_log) {
        m_log->setVisible(m_expanded);
    }
    if (m_expandButton) {
        m_expandButton->setText(
            m_expanded ? QStringLiteral("▾") : QStringLiteral("▴"));
    }
    if (!m_expanded) {
        hideSuggestions();
    }
    updateGeometry();
}

void CommandLine::scrollLogToBottom()
{
    if (!m_log) {
        return;
    }
    QScrollBar *bar = m_log->verticalScrollBar();
    if (bar) {
        bar->setValue(bar->maximum());
    }
}

void CommandLine::submit()
{
    if (!m_input) {
        return;
    }
    const QString text = m_input->text().trimmed();
    m_input->clear();
    hideSuggestions();
    if (!text.isEmpty()) {
        if (m_history.isEmpty() || m_history.last() != text) {
            m_history.append(text);
        }
        if (m_history.size() > 60) {
            m_history.removeFirst();
        }
    }
    m_historyCursor = m_history.size();
    m_draft.clear();
    emit commandSubmitted(text);
}

void CommandLine::recallHistory(int delta)
{
    if (!m_input || m_history.isEmpty()) {
        return;
    }
    if (m_historyCursor == m_history.size()) {
        m_draft = m_input->text();
    }
    m_historyCursor =
        qBound(0, m_historyCursor + delta, int(m_history.size()));
    if (m_historyCursor == m_history.size()) {
        m_input->setText(m_draft);
    } else {
        m_input->setText(m_history.at(m_historyCursor));
    }
    m_input->end(false);
}

void CommandLine::completeFromPrefix()
{
    if (!m_input || m_completions.isEmpty()) {
        return;
    }
    const QString prefix = m_input->text().trimmed().toUpper();
    if (prefix.isEmpty()) {
        return;
    }
    QStringList matches;
    for (const QString &name : std::as_const(m_completions)) {
        if (name.startsWith(prefix)) {
            matches.append(name);
        }
    }
    if (matches.isEmpty()) {
        return;
    }
    // 共同前缀直接补齐，多候选时列出提示
    QString common = matches.first();
    for (const QString &name : std::as_const(matches)) {
        int index = 0;
        while (index < common.size() && index < name.size()
               && common.at(index) == name.at(index)) {
            ++index;
        }
        common.truncate(index);
    }
    if (common.size() > prefix.size()) {
        m_input->setText(common);
        m_input->end(false);
    }
    if (matches.size() > 1) {
        appendMessage(matches.join(QStringLiteral("  ")),
                      MessageKind::Hint);
    }
}

bool CommandLine::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input) {
        if (event->type() == QEvent::FocusOut) {
            hideSuggestions();
        } else if (event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            const bool suggesting =
                m_suggestList && m_suggestList->isVisible()
                && m_suggestList->count() > 0;
            switch (keyEvent->key()) {
            case Qt::Key_Escape:
                if (suggesting) {
                    hideSuggestions();
                    return true;
                }
                m_input->clear();
                emit escapeRequested();
                return true;
            case Qt::Key_Up:
                if (suggesting) {
                    moveSuggestion(-1);
                    return true;
                }
                recallHistory(-1);
                return true;
            case Qt::Key_Down:
                if (suggesting) {
                    moveSuggestion(1);
                    return true;
                }
                recallHistory(1);
                return true;
            case Qt::Key_Tab:
                if (suggesting) {
                    acceptSuggestion();
                    return true;
                }
                completeFromPrefix();
                return true;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommandLine::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_hint) {
        m_hint->setVisible(width() >= 620);
    }
    repositionSuggestions();
}

void CommandLine::hideEvent(QHideEvent *event)
{
    hideSuggestions();
    QWidget::hideEvent(event);
}
