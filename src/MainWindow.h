#ifndef CADLENS_MAINWINDOW_H
#define CADLENS_MAINWINDOW_H

#include "CadView.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QPageLayout>
#include <QPageSize>
#include <QVariantList>

class QAction;
class QActionGroup;
class QCloseEvent;
class QComboBox;
class QDialog;
class QDockWidget;
class QLabel;
class QProgressBar;
class QStackedWidget;
class QToolBar;
class QTableWidget;
class QTableWidgetItem;
class QTabBar;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class StartScreen;

struct DrawingLoadResult {
    std::shared_ptr<CadDocument> document;
    std::shared_ptr<SpatialIndex> modelIndex;
    QHash<QString, QVector<CadEntity>> layoutContents;
    QHash<QString, QString> externalReferencePaths;
    QImage thumbnail;
    QString error;
};

struct DocumentSession {
    CadView *view = nullptr;
    QString path;
    QString title;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void openDrawing(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class CadTheme {
        Dark,
        Light,
        Gray,
        Blueprint
    };

    void createActions();
    void createInterface();
    void createMenus();
    void applyStyle();
    void refreshActionIcons();
    QIcon themedIcon(const QString &name) const;
    void setCadTheme(CadTheme theme, bool persist = true);
    void setDrawingActionsEnabled(bool enabled);
    void dockMeasureToolbar(Qt::ToolBarArea area);
    void finishDrawingLoad();
    void openDrawingWithExternalReferences(
        const QString &path,
        const QHash<QString, QString> &externalReferencePaths);
    void wireViewSignals(CadView *view);
    void activateDocument(int index);
    void closeDocumentTab(int index);
    void refreshDocumentUi();
    void populateLayoutTabs();
    void beginSelectAndFit();
    void populateLayers();
    void refreshMeasurementTable();
    void refreshUnitControls();
    void refreshStatus();
    void updateLayerVisibility(QTreeWidgetItem *item, int column);
    void updateSnapModesFromMenu();
    void saveAnnotationsNow();
    void autoSaveAnnotationsNow();
    void saveAnnotationsAs();
    void restoreCalibrationRecord(CadDocument &document);
    void rememberCalibrationRecord(const CadDocument &document);
    void updateRecentDrawing(const QString &path);
    void refreshStartScreen();
    void saveRecentThumbnail(const QString &path, const QImage &image);
    void removeRecentDrawing(const QString &path);
    void pruneSystemCachesToRecent(const QVariantList &recentRecords);
    void exportSvgAs();
    void requestTextAnnotation(const QPointF &position);
    void requestCalibration(double rawLength);
    void showAnnotationSettings();
    void showFileAssociationSettings();
    void showHelp();
    void showAbout();
    void beginFixedDistance();
    void beginAreaOffset();
    void beginSideArea();
    void requestPrint();
    void handlePrintArea(const QRectF &worldArea);
    void handlePrintAreaCanceled();
    void showPrintPreview();
    void changeUnit(int index);
    void changePrecision(int index);
    void setDirty(bool dirty);
    void showLoadError(const QString &message);
    QString measurementsSummary() const;
    QAction *makeToolAction(const QString &text, const QString &icon,
                            CadView::ToolMode mode,
                            const QKeySequence &shortcut = {});
    QAction *makeAction(const QString &text, const QString &icon,
                        const QKeySequence &shortcut = {});

    CadView *m_view = nullptr;
    QToolBar *m_mainToolbar = nullptr;
    QToolBar *m_measureToolbar = nullptr;
    StartScreen *m_startScreen = nullptr;
    QStackedWidget *m_centralStack = nullptr;
    QStackedWidget *m_documentStack = nullptr;
    QTabBar *m_documentTabs = nullptr;
    QWidget *m_canvasPage = nullptr;
    QVector<DocumentSession> m_documentSessions;
    int m_activeDocumentIndex = -1;
    QTreeWidget *m_layerTree = nullptr;
    QTableWidget *m_measurementTable = nullptr;
    QTabBar *m_layoutTabs = nullptr;
    QDialog *m_layerDialog = nullptr;
    QDockWidget *m_measurementDock = nullptr;
    QComboBox *m_unitCombo = nullptr;
    QComboBox *m_precisionCombo = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_drawingMetaLabel = nullptr;
    QLabel *m_coordinateLabel = nullptr;
    QLabel *m_snapLabel = nullptr;
    QLabel *m_scaleLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QActionGroup *m_toolGroup = nullptr;
    QAction *m_selectAction = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_exportSvgAction = nullptr;
    QAction *m_printAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_eraseAction = nullptr;
    QAction *m_deleteAllAction = nullptr;
    QAction *m_annotationVisibilityAction = nullptr;
    QAction *m_annotationSettingsAction = nullptr;
    QAction *m_fileAssociationAction = nullptr;
    QAction *m_layerPanelAction = nullptr;
    QAction *m_annotationManagerAction = nullptr;
    QAction *m_measureToolbarAction = nullptr;
    QAction *m_fitAction = nullptr;
    QAction *m_zoomWindowAction = nullptr;
    QAction *m_snapEnabledAction = nullptr;
    QAction *m_gridAction = nullptr;
    QAction *m_orthoAction = nullptr;
    QAction *m_darkThemeAction = nullptr;
    QAction *m_lightThemeAction = nullptr;
    QAction *m_grayThemeAction = nullptr;
    QAction *m_blueprintThemeAction = nullptr;
    QVector<QAction *> m_snapActions;
    QFutureWatcher<DrawingLoadResult> *m_loadWatcher = nullptr;
    QTimer *m_annotationAutoSaveTimer = nullptr;
    QString m_lastDirectory;
    QString m_currentSelection;
    bool m_refreshingTable = false;
    bool m_dirty = false;
    bool m_loading = false;
    CadTheme m_cadTheme = CadTheme::Dark;
    QRectF m_lastPrintArea;
    struct PrintSession {
        QVector<CadPrintPage> pages;
        int currentPage = 0;
        int paperSize = int(QPageSize::A4);
        int orientation = int(QPageLayout::Landscape);
        int resolution = 600;
        int copies = 1;
        bool blackInk = true;
        bool ruler = true;
        bool selectingPage = false;
        bool active = false;
        QString printerName;
    };
    PrintSession m_printSession;
};

#endif
