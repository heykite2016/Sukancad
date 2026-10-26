#ifndef CADLENS_STARTSCREEN_H
#define CADLENS_STARTSCREEN_H

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QLabel;
class QResizeEvent;
class QScrollArea;

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

signals:
    void browseRequested();
    void openRequested(const QString &path);
    void removeRecentRequested(const QString &path);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void rebuildRecentCards();
    int columnCountForWidth(int width) const;

    QVector<RecentDrawing> m_recentDrawings;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_recentHost = nullptr;
    QGridLayout *m_recentGrid = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QString m_darkStyleSheet;
    int m_columnCount = 0;
    bool m_darkTheme = true;
};

#endif
