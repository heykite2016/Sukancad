#ifndef CADLENS_MAINWINDOW_H
#define CADLENS_MAINWINDOW_H

#include "CadView.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QPageLayout>
#include <QPageSize>
#include <QVariantList>

#include <optional>

class QAction;
class QActionGroup;
class QCloseEvent;
class QComboBox;
class QDialog;
class QDockWidget;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QProgressBar;
class QStackedWidget;
class QToolBar;
class QToolButton;
class QTableWidget;
class QTableWidgetItem;
class QTabBar;
class QTimer;
class QThreadPool;
class QTreeWidget;
class QTreeWidgetItem;
class CommandLine;
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
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

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
    void appendChromeStyles(QString &styleSheet);
    void refreshActionIcons();
    QIcon themedIcon(const QString &name) const;
    void setCadTheme(CadTheme theme, bool persist = true);
    void applyCanvasQuality(
        RenderingQuality quality, bool persist = true);
    void setDrawingActionsEnabled(bool enabled);
    void dockMeasureToolbar(Qt::ToolBarArea area);
    void placeMeasurePalette(Qt::DockWidgetArea area);
    void showMeasurementPalette();
    void showFloatingDock(QDockWidget *dock, const QPoint &offset);
    void finishDrawingLoad();
    void openDrawingWithExternalReferences(
        const QString &path,
        const QHash<QString, QString> &externalReferencePaths);
    void wireViewSignals(CadView *view);
    void activateDocument(int index);
    void closeDocumentTab(int index);
    void refreshDocumentUi();
    void populateLayoutTabs();
    void beginSelect();
    void populateLayers();
    void refreshMeasurementTable();
    void refreshUnitControls();
    void refreshStatus();
    void updateLayerVisibility(QTreeWidgetItem *item, int column);
    void updateSnapModesFromMenu();
    void saveAnnotationsNow();
    void autoSaveAnnotationsNow();
    bool saveAnnotationsAutosaveBlocking(QString *errorMessage);
    void startNextAnnotationAutoSave();
    void saveAnnotationsAs();
    void restoreCalibrationRecord(CadDocument &document);
    void rememberCalibrationRecord(const CadDocument &document);
    void updateRecentDrawing(const QString &path);
    void refreshStartScreen();
    void saveRecentThumbnail(const QString &path, const QImage &image);
    void removeRecentDrawing(const QString &path);
    void clearRecentDrawings();
    void pruneSystemCachesToRecent(const QVariantList &recentRecords);
    void exportSvgAs();
    void requestTextAnnotation(const QPointF &position);
    void requestCalibration(double rawLength);
    void showAnnotationSettings();
    void showAnnotationSettingsLegacy();
    void showFileAssociationSettings();
    void showHelp();
    void showAbout();
    void beginSideArea();
    void requestPrint();
    void handlePrintArea(const QRectF &worldArea);
    void handlePrintAreaCanceled();
    void showPrintPreview();
    void printLayoutFrames();
    QVector<QRectF> detectLayoutFrames(const QString &spaceName) const;
    // AutoCAD 命令模式
    void executeCommand(const QString &raw);
    void echoCommand(const QString &text);
    void echoCommandError(const QString &text);
    void echoCommandOk(const QString &text);
    void setCommandPanelVisible(bool visible);
    void showCommandReference();
    void startToolMode(CadView::ToolMode mode, const QString &label);
    QStringList commandCompletions() const;
    // 命令行动态提示：输入过程中实时给出候选命令与用法
    void updateCommandSuggestions(const QString &text);
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
    QDockWidget *m_measurePaletteDock = nullptr;
    StartScreen *m_startScreen = nullptr;
    QStackedWidget *m_centralStack = nullptr;
    QStackedWidget *m_documentStack = nullptr;
    QTabBar *m_documentTabs = nullptr;
    QWidget *m_canvasPage = nullptr;
    QVector<DocumentSession> m_documentSessions;
    int m_activeDocumentIndex = -1;
    QTreeWidget *m_layerTree = nullptr;
    QTableWidget *m_measurementTable = nullptr;
    QStringList m_measurementTableIds;
    QTabBar *m_layoutTabs = nullptr;
    QDockWidget *m_layerDock = nullptr;
    QDockWidget *m_measurementDock = nullptr;
    QComboBox *m_unitCombo = nullptr;
    QComboBox *m_precisionCombo = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_drawingMetaLabel = nullptr;
    QLabel *m_toolbarBrandIconLabel = nullptr;
    QLabel *m_coordinateLabel = nullptr;
    QLabel *m_snapLabel = nullptr;
    QLabel *m_scaleLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QLabel *m_summaryLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    CommandLine *m_commandLine = nullptr;
    QToolButton *m_commandPanelButton = nullptr;
    QToolButton *m_snapStatusButton = nullptr;
    QAction *m_commandPanelAction = nullptr;
    QString m_lastCommand;
    QString m_lastCommandHint;
    QActionGroup *m_toolGroup = nullptr;
    QAction *m_selectAction = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_saveAsAction = nullptr;
    QAction *m_exportSvgAction = nullptr;
    QAction *m_printAction = nullptr;
    QAction *m_printFramesAction = nullptr;
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
    QAction *m_measurePaletteAction = nullptr;
    QAction *m_fitAction = nullptr;
    QAction *m_zoomWindowAction = nullptr;
    QAction *m_snapEnabledAction = nullptr;
    QAction *m_gridAction = nullptr;
    QAction *m_orthoAction = nullptr;
    QAction *m_lineWeightAction = nullptr;
    QAction *m_darkThemeAction = nullptr;
    QAction *m_lightThemeAction = nullptr;
    QAction *m_grayThemeAction = nullptr;
    QAction *m_blueprintThemeAction = nullptr;
    QVector<QAction *> m_snapActions;
    QFutureWatcher<DrawingLoadResult> *m_loadWatcher = nullptr;
    QTimer *m_annotationAutoSaveTimer = nullptr;
    QTimer *m_cursorStatusTimer = nullptr;
    QString m_lastDirectory;
    QString m_currentSelection;
    QPointF m_pendingCursorStatus;
    QString m_pendingSnapStatus;
    QString m_lastSnapStatus;
    double m_pendingZoomPercent = 100.0;
    bool m_cursorStatusPending = false;
    bool m_snapStatusPending = false;
    bool m_zoomStatusPending = false;
    bool m_refreshingTable = false;
    bool m_dirty = false;
    bool m_loading = false;
    bool m_workspaceLayoutReady = false;
    CadTheme m_cadTheme = CadTheme::Dark;
    QRectF m_lastPrintArea;
    RenderingQuality m_canvasQuality =
        RenderingQuality::Performance;
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
    struct AnnotationSaveSnapshot {
        QString drawingPath;
        double metersPerDrawingUnit = 0.001;
        bool calibrated = false;
        DisplaySettings displaySettings;
        QVector<Measurement> measurements;
        quint64 generation = 0;
    };
    struct AnnotationSaveResult {
        bool success = false;
        QString error;
        quint64 generation = 0;
    };
    QFutureWatcher<AnnotationSaveResult> *m_annotationSaveWatcher =
        nullptr;
    QThreadPool *m_annotationSavePool = nullptr;
    std::optional<AnnotationSaveSnapshot> m_pendingAnnotationSave;
    quint64 m_annotationSaveGeneration = 0;
    bool m_annotationSaveInFlight = false;
};

#endif
