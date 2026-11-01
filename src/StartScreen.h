#ifndef CADLENS_STARTSCREEN_H
#define CADLENS_STARTSCREEN_H

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

struct RecentDrawing {
    QString path;
    QString title;
    QString thumbnailPath;
    QDateTime lastOpened;
};

// CAD 起始页：左侧「开始」操作栏 + 中间最近图纸列表 + 右侧预览面板。
class StartScreen : public QWidget {
    Q_OBJECT

public:
    explicit StartScreen(QWidget *parent = nullptr);

    void setRecentDrawings(const QVector<RecentDrawing> &drawings);
    void setCadTheme(bool dark);
    bool motionEnabled() const { return m_motionEnabled; }
    void setMotionEnabled(bool enabled);

signals:
    void browseRequested();
    void openRequested(const QString &path);
    void removeRecentRequested(const QString &path);
    void clearRecentRequested();
    void motionEnabledChanged(bool enabled);
    void commandReferenceRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void rebuildRecentList();
    void updatePreview(const QString &path);
    void applyTheme();
    void openSelected();
    void removeSelected();

    QVector<RecentDrawing> m_recentDrawings;
    QStackedWidget *m_listStack = nullptr;
    QTreeWidget *m_list = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QLabel *m_countLabel = nullptr;
    QPushButton *m_clearRecent = nullptr;
    QLabel *m_previewImage = nullptr;
    QLabel *m_previewTitle = nullptr;
    QLabel *m_previewPath = nullptr;
    QLabel *m_previewMeta = nullptr;
    QPushButton *m_previewOpen = nullptr;
    QPushButton *m_previewRemove = nullptr;
    QLabel *m_previewPlaceholder = nullptr;
    bool m_darkTheme = true;
    bool m_motionEnabled = false;
};

#endif
