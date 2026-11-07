#ifndef CADLENS_COMMANDLINE_H
#define CADLENS_COMMANDLINE_H

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QResizeEvent;
class QTextEdit;
class QToolButton;

// 命令候选：token 用于补全（ASCII），title/detail 用于动态提示显示。
struct CommandSuggestion {
    QString token;
    QString title;
    QString detail;
};

// AutoCAD 风格命令行：上方滚动历史，下方 "命令:" 输入行，右侧显示当前提示。
// 输入时在输入框上方弹出动态候选列表（↑↓ 选择，Tab 补全）。
// 只负责输入、历史与显示，具体命令语义由 MainWindow 解析。
class CommandLine : public QWidget {
    Q_OBJECT

public:
    enum class MessageKind {
        Info,     // 普通信息（灰）
        Echo,     // 命令回显（强调色）
        Success,  // 完成（绿）
        Error,    // 错误（红）
        Hint      // 交互提示（淡）
    };
    Q_ENUM(MessageKind)

    explicit CommandLine(QWidget *parent = nullptr);
    ~CommandLine() override;

    void appendMessage(const QString &text,
                       MessageKind kind = MessageKind::Info);
    void appendCommandEcho(const QString &command);
    void setHint(const QString &hint);
    void setCompletions(const QStringList &names);
    // 动态候选：输入过程中由 MainWindow 计算后推送
    void setSuggestions(const QVector<CommandSuggestion> &suggestions);
    void hideSuggestions();
    void setLogColors(const QColor &text, const QColor &muted,
                      const QColor &accent, const QColor &error,
                      const QColor &success);
    void setSuggestionColors(const QColor &panel, const QColor &border,
                             const QColor &text, const QColor &muted,
                             const QColor &accent);
    void clearLog();

    void focusInput();
    void typeIntoInput(const QString &text);
    QString currentText() const;

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);
    QStringList history() const { return m_history; }

signals:
    void commandSubmitted(const QString &command);
    void escapeRequested();
    void expandedChanged(bool expanded);
    void inputEdited(const QString &text);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void submit();
    void recallHistory(int delta);
    void completeFromPrefix();
    void applyExpansion();
    void scrollLogToBottom();
    void acceptSuggestion();
    void moveSuggestion(int delta);
    void repositionSuggestions();
    const QColor &colorFor(MessageKind kind) const;

    QTextEdit *m_log = nullptr;
    QLineEdit *m_input = nullptr;
    QLabel *m_prompt = nullptr;
    QLabel *m_hint = nullptr;
    QToolButton *m_expandButton = nullptr;
    QListWidget *m_suggestList = nullptr;
    QStringList m_history;
    QStringList m_completions;
    QVector<CommandSuggestion> m_suggestions;
    int m_historyCursor = 0;
    QString m_draft;
    bool m_expanded = false;

    QColor m_textColor = QColor(QStringLiteral("#c8d3da"));
    QColor m_mutedColor = QColor(QStringLiteral("#7c8b95"));
    QColor m_accentColor = QColor(QStringLiteral("#63b6df"));
    QColor m_errorColor = QColor(QStringLiteral("#e06c63"));
    QColor m_successColor = QColor(QStringLiteral("#54cf9b"));
    QColor m_panelColor = QColor(QStringLiteral("#141c22"));
    QColor m_borderColor = QColor(QStringLiteral("#24323b"));
};

#endif
