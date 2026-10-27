#ifndef CADLENS_STARTSCREEN_H
#define CADLENS_STARTSCREEN_H

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QHideEvent;
class QLabel;
class QResizeEvent;
class QScrollArea;
class QShowEvent;
class QToolButton;
class StartScreenLineBackground;

struct RecentDrawing {
    QString path;
    QString title;
    QString thumbnailPath;
    QDateTime lastOpened;
};

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
    void motionEnabledChanged(bool enabled);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildRecentCards();
    int columnCountForWidth(int width) const;

    QVector<RecentDrawing> m_recentDrawings;
    QScrollArea *m_scrollArea = nullptr;
    StartScreenLineBackground *m_background = nullptr;
    QWidget *m_recentHost = nullptr;
    QGridLayout *m_recentGrid = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QToolButton *m_motionToggle = nullptr;
    QString m_darkStyleSheet;
    int m_columnCount = 0;
    bool m_darkTheme = true;
    bool m_motionEnabled = true;
};

#endif
