#include "MainWindow.h"
#include "VecDatabase.h"
#include "VectorExplorerDialog.h"
#include "VectorSupport.h"

#include <functional>
#include <memory>

#include <QAction>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPoint>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QPair>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUrl>
#include <QVersionNumber>
#include <QTextCursor>
#include <QTextStream>
#include <QVariantMap>

#ifdef Q_OS_WASM
#include <emscripten.h>
#endif

#include "CreateIndexDialog.h"
#include "CreateTableDialog.h"
#include "ExportDialog.h"
#include "InsertRowDialog.h"
#include "SettingsDialog.h"

namespace {

// Removes and deletes every widget/layout currently inside layout,
// so the query builder panel can be rebuilt each time the focus changes.
void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        }
        delete item;
    }
}

// QSettings keys for the Settings dialog's preferences, plus their
// defaults - kept together so loadSettings()/saveSettings() can't drift
// out of sync with each other.
const QString kSettingsUiFontSize = "appearance/uiFontSize";
const QString kSettingsEditorFontSize = "appearance/editorFontSize";
const QString kSettingsConfirmDestructive = "behavior/confirmDestructiveActions";
const QString kSettingsDefaultDir = "files/defaultDialogDir";
const QString kSettingsQueryHistory = "history/queries";
constexpr int kMaxHistoryEntries = 100;

// Browser only: QSettings writes into the in-memory filesystem, so push it
// to IndexedDB after each save (see mountPersistentStorage() in main.cpp).
// No-op on desktop, where QSettings is already persistent.
void flushPersistentStorage(QSettings& settings)
{
    settings.sync();
#ifdef Q_OS_WASM
    EM_ASM({ if (typeof FS !== 'undefined') { FS.syncfs(false, function (err) {}); } });
#endif
}
constexpr int kDefaultUiFontSize = 12;
constexpr int kDefaultEditorFontSize = 11;
constexpr bool kDefaultConfirmDestructive = true;

// Wraps a value in single quotes for use as a SQL literal, unless it
// already looks like a number - keeps the "add condition" flow simple
// without needing a full type-aware value editor.
QString sqlLiteral(const QString& rawValue)
{
    bool isNumber = false;
    rawValue.toDouble(&isNumber);
    if (isNumber || rawValue.startsWith('\'')) {
        return rawValue;
    }
    return "'" + rawValue + "'";
}

// Splits a block of SQL text into individual statements on ';', while
// ignoring semicolons that appear inside single-quoted string literals
// (with '' as the SQL-standard escaped quote). This isn't a full SQL
// parser - it doesn't know about /* block comments */ or multi-statement
// trigger bodies - but it's enough for round-tripping our own exports
// and for typical hand-written import scripts.
QStringList splitSqlStatements(const QString& sql)
{
    QStringList statements;
    QString current;
    bool inString = false;

    for (int i = 0; i < sql.size(); ++i) {
        const QChar ch = sql[i];
        current += ch;

        if (ch == '\'') {
            if (inString && i + 1 < sql.size() && sql[i + 1] == '\'') {
                // Escaped quote ('') inside a string literal - consume
                // both characters without toggling string state.
                current += sql[i + 1];
                ++i;
                continue;
            }
            inString = !inString;
            continue;
        }

        if (ch == ';' && !inString) {
            const QString trimmed = current.trimmed();
            if (!trimmed.isEmpty()) {
                statements << trimmed;
            }
            current.clear();
        }
    }

    const QString trailing = current.trimmed();
    if (!trailing.isEmpty()) {
        statements << trailing;
    }

    return statements;
}

// Escapes a single field for CSV/TSV output per RFC 4180: wrapped in
// double quotes (with embedded quotes doubled) whenever it contains the
// delimiter, a quote, or a newline; left alone otherwise.
QString delimitedFieldEscape(const QString& field, QChar delimiter)
{
    if (field.contains(delimiter) || field.contains('"') || field.contains('\n') || field.contains('\r')) {
        QString escaped = field;
        escaped.replace("\"", "\"\"");
        return "\"" + escaped + "\"";
    }
    return field;
}

// Renders one results-grid cell for a SQL INSERT statement: empty means
// NULL, a numeric-looking value goes out unquoted, everything else is
// quoted with '' escaping. Unlike Schema-aware quoteForExport() (used
// for the full-database export), this works purely off the display text
// already sitting in the grid - no QVariant/type info survives once a
// query's results have been converted to strings for display.
QString sqlLiteralForResultsExport(const QString& text)
{
    if (text.isEmpty()) {
        return "NULL";
    }
    bool isNumber = false;
    text.toDouble(&isNumber);
    if (isNumber) {
        return text;
    }
    QString escaped = text;
    escaped.replace("'", "''");
    return "'" + escaped + "'";
}

#ifdef Q_OS_WASM
// Browser: sends a file from the virtual filesystem to the user as a download.
void wasmDownload(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    QFileDialog::saveFileContent(f.readAll(), QFileInfo(path).fileName());
}

// Browser: databases are kept in /persist, the IndexedDB-backed folder that
// main.cpp mounts at start-up. (The rest of the virtual filesystem, /tmp
// included, is wiped on every page reload.)
const QString kWasmDbDir = QStringLiteral("/persist/databases");
#endif

// Called after every change to the open database. Browser: copies the
// database file from the in-memory filesystem into IndexedDB, so the edit
// is still there after a page reload. Desktop: nothing to do - the commit
// already wrote the file.
void persistDatabase()
{
#ifdef Q_OS_WASM
    EM_ASM({
        if (typeof FS !== 'undefined') {
            FS.syncfs(false, function (err) {
                if (err) { console.error('WinSQLite: saving to browser storage failed', err); }
            });
        }
    });
#endif
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("WinSQLite");
    loadSettings();
    loadHistory();
    buildUi();
    buildMenus();
    applyDarkTheme();
}

MainWindow::~MainWindow()
{
    stopQueryThread();
}

void MainWindow::buildUi()
{
    resize(1150, 720);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // Top bar
    auto* topBar = new QWidget(central);
    topBar->setObjectName("topBar");
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(16, 10, 16, 10);
    auto* titleLabel = new QLabel("No database open", topBar);
    titleLabel->setObjectName("titleLabel");
    m_titleLabel = titleLabel;
    topLayout->addWidget(titleLabel);
    topLayout->addStretch();
    m_sqlModeBtn = new QPushButton("SQL mode", topBar);
    m_sqlModeBtn->setCheckable(true);
    m_sqlModeBtn->setEnabled(false);
    m_historyBtn = new QPushButton("History", topBar);
    m_historyBtn->setToolTip("Re-run or edit a previously executed query");
    m_historyBtn->setEnabled(false);
    m_view3dBtn = new QPushButton("3D view", topBar);
    m_view3dBtn->setToolTip("Explore the sqlite-vec (vec0) vectors of this database in 3D");
    m_view3dBtn->setEnabled(false);
    m_executeBtn = new QPushButton("Execute", topBar);
    m_executeBtn->setObjectName("primaryButton");
    m_executeBtn->setEnabled(false);
    auto* donateBtn = new QPushButton("Donate", topBar);
    donateBtn->setToolTip("Support WinSQLite - opens the Microsoft Store page");
    connect(donateBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl("https://apps.microsoft.com/detail/9nrd3hmxzw41?hl=en-US&gl=US&ocid=pdpshare"));
    });
    topLayout->addWidget(donateBtn);
    topLayout->addWidget(m_view3dBtn);
    topLayout->addWidget(m_historyBtn);
    topLayout->addWidget(m_sqlModeBtn);
    topLayout->addWidget(m_executeBtn);
    rootLayout->addWidget(topBar);

    // Outer horizontal splitter: the table list on the left spans the
    // full window height, independent of the results panel below - only
    // the right side (diagram + query builder + results) splits vertically.
    auto* outerSplitter = new QSplitter(Qt::Horizontal, central);
    outerSplitter->setObjectName("outerSplitter");
    outerSplitter->setHandleWidth(8);
    outerSplitter->setChildrenCollapsible(false);
    outerSplitter->setOpaqueResize(true);

    // Left panel - table list, full height top to bottom.
    auto* leftPanel = new QWidget(outerSplitter);
    leftPanel->setObjectName("leftPanel");
    leftPanel->setMinimumWidth(140);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(12, 14, 12, 12);
    auto* tablesLabel = new QLabel("TABLES", leftPanel);
    tablesLabel->setObjectName("sectionLabel");
    leftLayout->addWidget(tablesLabel);
    m_tableTree = new QTreeWidget(leftPanel);
    m_tableTree->setObjectName("tableTree");
    m_tableTree->setHeaderHidden(true);
    m_tableTree->setColumnCount(1);
    m_tableTree->setIndentation(14);
    leftLayout->addWidget(m_tableTree);
    outerSplitter->addWidget(leftPanel);

    // Right side: everything except the table list. Vertical splitter -
    // the diagram/query-builder body on top, query results at the bottom.
    auto* rightSide = new QWidget(outerSplitter);
    auto* rightSideLayout = new QVBoxLayout(rightSide);
    rightSideLayout->setContentsMargins(0, 0, 0, 0);
    rightSideLayout->setSpacing(0);

    auto* mainSplitter = new QSplitter(Qt::Vertical, rightSide);
    mainSplitter->setObjectName("mainSplitter");
    mainSplitter->setHandleWidth(8);
    mainSplitter->setChildrenCollapsible(false);

    // Diagram + query builder body.
    auto* body = new QSplitter(Qt::Horizontal, mainSplitter);
    body->setObjectName("bodySplitter");
    body->setHandleWidth(8);
    body->setChildrenCollapsible(false);
    body->setOpaqueResize(true);

    // Center panel - diagram
    m_diagramView = new DiagramView(body);
    m_diagramView->setMinimumWidth(200);
    body->addWidget(m_diagramView);

    // Right panel - query builder
    auto* rightPanel = new QWidget(body);
    rightPanel->setObjectName("rightPanel");
    rightPanel->setMinimumWidth(340);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(14, 14, 14, 14);
    auto* builderLabel = new QLabel("QUERY BUILDER", rightPanel);
    builderLabel->setObjectName("sectionLabel");
    rightLayout->addWidget(builderLabel);

    m_rightStack = new QStackedWidget(rightPanel);

    // Page 0: the step-based builder view.
    auto* builderPage = new QWidget(m_rightStack);
    auto* builderPageLayout = new QVBoxLayout(builderPage);
    builderPageLayout->setContentsMargins(0, 0, 0, 0);

    auto* stepsContainer = new QWidget(builderPage);
    m_stepsLayout = new QVBoxLayout(stepsContainer);
    m_stepsLayout->setContentsMargins(0, 8, 0, 8);
    builderPageLayout->addWidget(stepsContainer);

    m_addStepBtn = new QPushButton("+  Add step", builderPage);
    builderPageLayout->addWidget(m_addStepBtn);

    m_sqlPreview = new QPlainTextEdit(builderPage);
    m_sqlPreview->setObjectName("sqlPreview");
    m_sqlPreview->setReadOnly(true);
    builderPageLayout->addWidget(m_sqlPreview, 1);

    m_rightStack->addWidget(builderPage);

    // Page 1: raw, editable SQL - the "escape hatch". Edits here are not
    // parsed back into the QueryModel; leaving SQL mode discards them and
    // restores whatever the builder currently represents.
    auto* sqlPage = new QWidget(m_rightStack);
    auto* sqlPageLayout = new QVBoxLayout(sqlPage);
    sqlPageLayout->setContentsMargins(0, 8, 0, 0);
    m_sqlEditor = new SqlEditor(sqlPage);
    m_sqlEditor->setObjectName("sqlPreview");
    sqlPageLayout->addWidget(m_sqlEditor, 1);

    m_rightStack->addWidget(sqlPage);

    rightLayout->addWidget(m_rightStack, 1);

    body->addWidget(rightPanel);
    body->setStretchFactor(0, 1);
    body->setStretchFactor(1, 1);
    body->setSizes({460, 460});

    mainSplitter->addWidget(body);

    // Results panel
    auto* resultsPanel = new QWidget(mainSplitter);
    resultsPanel->setObjectName("resultsPanel");
    auto* resultsLayout = new QVBoxLayout(resultsPanel);
    resultsLayout->setContentsMargins(16, 10, 16, 12);

    auto* resultsHeader = new QHBoxLayout();
    auto* resultsLabel = new QLabel("RESULTS", resultsPanel);
    resultsLabel->setObjectName("sectionLabel");
    resultsHeader->addWidget(resultsLabel);
    resultsHeader->addStretch();
    m_statusLabel = new QLabel(resultsPanel);
    m_statusLabel->setObjectName("statusLabel");
    resultsHeader->addWidget(m_statusLabel);
    m_exportResultsBtn = new QPushButton("Export…", resultsPanel);
    m_exportResultsBtn->setEnabled(false);  // enabled once a query returns at least one column
    resultsHeader->addWidget(m_exportResultsBtn);
    m_maximizeResultsBtn = new QPushButton("Maximize", resultsPanel);
    m_maximizeResultsBtn->setToolTip("Show results over the whole window (F11)");
    resultsHeader->addWidget(m_maximizeResultsBtn);
    resultsLayout->addLayout(resultsHeader);

    m_resultsTable = new QTableWidget(resultsPanel);
    m_resultsTable->setObjectName("resultsTable");
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setAlternatingRowColors(true);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    resultsLayout->addWidget(m_resultsTable, 1);

    mainSplitter->addWidget(resultsPanel);
    mainSplitter->setStretchFactor(0, 3);
    mainSplitter->setStretchFactor(1, 2);
    mainSplitter->setSizes({420, 220});

    rightSideLayout->addWidget(mainSplitter, 1);
    outerSplitter->addWidget(rightSide);
    outerSplitter->setStretchFactor(0, 0);
    outerSplitter->setStretchFactor(1, 1);
    outerSplitter->setSizes({200, 800});

    rootLayout->addWidget(outerSplitter, 1);

    // Kept for onToggleMaximizeResults().
    m_bodySplitter = body;
    m_leftPanel = leftPanel;

    setCentralWidget(central);

    connect(m_tableTree, &QTreeWidget::itemClicked, this, &MainWindow::onTreeItemClicked);
    connect(m_diagramView, &DiagramView::joinCandidateClicked, this, &MainWindow::onJoinCandidateClicked);
    connect(m_addStepBtn, &QPushButton::clicked, this, &MainWindow::onAddStepClicked);
    connect(m_sqlModeBtn, &QPushButton::toggled, this, &MainWindow::onSqlModeToggled);
    connect(m_executeBtn, &QPushButton::clicked, this, &MainWindow::onExecuteClicked);
    connect(m_view3dBtn, &QPushButton::clicked, this, &MainWindow::onView3dClicked);
    connect(m_historyBtn, &QPushButton::clicked, this, &MainWindow::onHistoryClicked);

    // vec0 tables: double-click or right-click opens the 3D explorer.
    connect(m_tableTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && item->data(0, Qt::UserRole + 1).toBool()) {
            QTreeWidgetItem* top = item->parent() ? item->parent() : item;
            openVectorSpace(top->data(0, Qt::UserRole + 2).toString());
        }
    });
    m_tableTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tableTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_tableTree->itemAt(pos);
        if (!item) {
            return;
        }
        QMenu menu(this);

        if (item->data(0, Qt::UserRole + 1).toBool()) {
            QTreeWidgetItem* top = item->parent() ? item->parent() : item;
            const QString name = top->data(0, Qt::UserRole + 2).toString();
            QAction* open3d = menu.addAction(tr("Open 3D view"));
            if (menu.exec(m_tableTree->viewport()->mapToGlobal(pos)) == open3d) {
                openVectorSpace(name);
            }
            return;
        }

        // Regular tables: right-click the table, one of its columns or one
        // of its indexes. Index rows carry the index name in UserRole + 3
        // (see populateTableTree()); every row carries its table in UserRole.
        const QString indexName = item->data(0, Qt::UserRole + 3).toString();
        const QString tableName = item->data(0, Qt::UserRole).toString();
        if (tableName.isEmpty()) {
            return;
        }
        QAction* editIndexAct = nullptr;
        QAction* editTableAct = menu.addAction(tr("Edit table \"%1\"…").arg(tableName));
        if (!indexName.isEmpty()) {
            editIndexAct = menu.addAction(tr("Edit index \"%1\"…").arg(indexName));
        }
        QAction* chosen = menu.exec(m_tableTree->viewport()->mapToGlobal(pos));
        if (!chosen) {
            return;
        }
        if (chosen == editTableAct) {
            editTable(tableName);
        } else if (chosen == editIndexAct) {
            for (const auto& index : m_schema.indexes()) {
                if (index.name == indexName) {
                    editIndex(index);
                    break;
                }
            }
        }
    });
    connect(m_sqlEditor, &QPlainTextEdit::textChanged, this, &MainWindow::onSqlTextChanged);
    connect(m_resultsTable, &QTableWidget::itemChanged, this, &MainWindow::onResultCellEdited);
    connect(m_exportResultsBtn, &QPushButton::clicked, this, &MainWindow::onExportResultsTriggered);
    connect(m_maximizeResultsBtn, &QPushButton::clicked, this, &MainWindow::onToggleMaximizeResults);
}

void MainWindow::buildMenus()
{
    auto* databaseMenu = menuBar()->addMenu("&Database");

    QAction* newDbAction = databaseMenu->addAction("&New Database…");
    newDbAction->setShortcut(QKeySequence::New);
    connect(newDbAction, &QAction::triggered, this, &MainWindow::onNewDatabaseTriggered);

    QAction* openDbAction = databaseMenu->addAction("&Open Database…");
    openDbAction->setShortcut(QKeySequence::Open);
    connect(openDbAction, &QAction::triggered, this, &MainWindow::onOpenDatabaseTriggered);

#ifdef Q_OS_WASM
    // Databases are kept in the browser's storage between visits; this opens
    // one of them without having to pick the file from the disk again.
    QAction* openSavedDbAction = databaseMenu->addAction("Open Sa&ved in Browser…");
    connect(openSavedDbAction, &QAction::triggered, this, [this]() {
        const QStringList saved = QDir(kWasmDbDir).entryList({"*.sqlite", "*.db", "*.sqlite3"}, QDir::Files, QDir::Name);
        if (saved.isEmpty()) {
            QMessageBox::information(this, "Open Saved Database", "No databases are saved in this browser yet.");
            return;
        }
        bool ok = false;
        const QString name = QInputDialog::getItem(this, "Open Saved Database", "Database:", saved, 0, false, &ok);
        if (!ok) {
            return;
        }
        if (!openDatabase(kWasmDbDir + "/" + name)) {
            QMessageBox::critical(this, "Error", "Unable to open \"" + name + "\".\n\n" + m_lastOpenError);
        }
    });
#endif

    QAction* saveDbAction = databaseMenu->addAction("&Save Database");
    saveDbAction->setShortcut(QKeySequence::Save);
    connect(saveDbAction, &QAction::triggered, this, &MainWindow::onSaveDatabaseTriggered);

    databaseMenu->addSeparator();

    QAction* exportDbAction = databaseMenu->addAction("&Export Database…");
    connect(exportDbAction, &QAction::triggered, this, &MainWindow::onExportDatabaseTriggered);

    QAction* importDbAction = databaseMenu->addAction("&Import SQL…");
    connect(importDbAction, &QAction::triggered, this, &MainWindow::onImportDatabaseTriggered);

    databaseMenu->addSeparator();
    QAction* exitAction = databaseMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    auto* schemaMenu = menuBar()->addMenu("&Schema");

    QAction* newTableAction = schemaMenu->addAction("New &Table…");
    newTableAction->setShortcut(QKeySequence("Ctrl+T"));
    connect(newTableAction, &QAction::triggered, this, &MainWindow::onNewTableTriggered);

    QAction* newIndexAction = schemaMenu->addAction("New &Index…");
    newIndexAction->setShortcut(QKeySequence("Ctrl+I"));
    connect(newIndexAction, &QAction::triggered, this, &MainWindow::onNewIndexTriggered);

    schemaMenu->addSeparator();

    QAction* editTableAction = schemaMenu->addAction("&Edit Table…");
    connect(editTableAction, &QAction::triggered, this, &MainWindow::onEditTableTriggered);

    QAction* editIndexAction = schemaMenu->addAction("Edit I&ndex…");
    connect(editIndexAction, &QAction::triggered, this, &MainWindow::onEditIndexTriggered);

    schemaMenu->addSeparator();

    QAction* dropTableAction = schemaMenu->addAction("Drop Table…");
    connect(dropTableAction, &QAction::triggered, this, &MainWindow::onDropTableTriggered);

    QAction* dropIndexAction = schemaMenu->addAction("Drop Index…");
    connect(dropIndexAction, &QAction::triggered, this, &MainWindow::onDropIndexTriggered);

    auto* dataMenu = menuBar()->addMenu("D&ata");
    QAction* insertRowAction = dataMenu->addAction("&Insert Row…");
    insertRowAction->setShortcut(QKeySequence("Ctrl+Shift+I"));
    connect(insertRowAction, &QAction::triggered, this, &MainWindow::onInsertRowTriggered);

    QAction* exportResultsAction = dataMenu->addAction("Export &Results…");
    exportResultsAction->setShortcut(QKeySequence("Ctrl+Shift+E"));
    connect(exportResultsAction, &QAction::triggered, this, &MainWindow::onExportResultsTriggered);

    // Not in m_schemaActions: it only rearranges the window, so it works
    // with or without a database open.
    QAction* maximizeResultsAction = dataMenu->addAction("&Maximize Results");
    maximizeResultsAction->setShortcut(QKeySequence(Qt::Key_F11));
    connect(maximizeResultsAction, &QAction::triggered, this, &MainWindow::onToggleMaximizeResults);

    // These all act on the currently open database, so they start
    // disabled - openDatabase() enables them once one is loaded.
    m_schemaActions = {saveDbAction,    newTableAction,  newIndexAction,     editTableAction,     editIndexAction, dropTableAction,
                        dropIndexAction, insertRowAction,    exportDbAction,      exportResultsAction,
                        importDbAction};
    setSchemaActionsEnabled(false);

    // Tools > Vector space (3D)... / Create demo vector database... (sqlite-vec).
    // Independent of the main connection: it opens its own vec-enabled connection.
    vec::installVectorExplorer(this, [this] { return m_db.databaseName(); });

    // Settings is independent of whether a database is open, so its
    // action is never added to m_schemaActions / disabled above.
    auto* settingsMenu = menuBar()->addMenu("&Settings");
    QAction* preferencesAction = settingsMenu->addAction("&Preferences…");
    preferencesAction->setShortcut(QKeySequence("Ctrl+,"));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::onSettingsTriggered);
}

void MainWindow::applyDarkTheme()
{
    // m_uiFontSize/m_editorFontSize come from the Settings dialog (see
    // loadSettings()/onSettingsTriggered()); the title bar scales two
    // points larger than the base UI size rather than being a separate
    // setting of its own.
    const int titleFontSize = m_uiFontSize + 2;

    setStyleSheet(QString(R"(
        QMainWindow, QWidget { background-color: #17171A; color: #D3D1C7; font-size: %1px; }
        #topBar { background-color: #201F22; border-bottom: 1px solid #333230; }
        #titleLabel { font-size: %2px; font-weight: 600; color: #F1EFE8; }
        QSplitter#bodySplitter::handle, QSplitter#outerSplitter::handle, QSplitter#mainSplitter::handle {
            background-color: #2C2C2A;
            border-left: 1px solid #444441;
            border-right: 1px solid #444441;
        }
        QSplitter#mainSplitter::handle {
            border-left: none;
            border-right: none;
            border-top: 1px solid #444441;
            border-bottom: 1px solid #444441;
        }
        QSplitter#bodySplitter::handle:hover, QSplitter#outerSplitter::handle:hover, QSplitter#mainSplitter::handle:hover { background-color: #378ADD; }
        #leftPanel, #rightPanel, #resultsPanel { background-color: #1C1B1E; }
        #rightPanel { border-left: 1px solid #333230; }
        #leftPanel { border-right: 1px solid #333230; }
        #resultsPanel { border-top: 1px solid #333230; }
        #sectionLabel { color: #888780; font-size: 10px; letter-spacing: 1px; padding: 4px 0; }
        #statusLabel { color: #888780; font-size: 10px; }
        QTreeWidget#tableTree { background: transparent; border: none; }
        QTreeWidget#tableTree::item { padding: 5px 4px; border-radius: 6px; }
        QTreeWidget#tableTree::item:selected { background-color: #0C447C; color: #E6F1FB; }
        QTreeWidget#tableTree::branch { background: transparent; }
        QPlainTextEdit#sqlPreview {
            background-color: #201F22; border: 1px solid #333230; border-radius: 6px;
            font-family: Consolas, monospace; font-size: %3px; color: #FAC775; padding: 8px;
        }
        QTableWidget#resultsTable {
            background-color: #201F22; border: 1px solid #333230; border-radius: 6px;
            gridline-color: #333230; alternate-background-color: #232226;
        }
        QTableWidget#resultsTable::item { padding: 4px 8px; }
        QTableWidget#resultsTable::item:selected { background-color: #0C447C; color: #E6F1FB; }
        QHeaderView::section {
            background-color: #201F22; color: #888780; border: none;
            border-bottom: 1px solid #333230; padding: 6px 8px; font-size: 10px;
        }
        QPushButton {
            background-color: #201F22; border: 1px solid #444441; border-radius: 6px;
            padding: 6px 12px; color: #D3D1C7;
        }
        QPushButton:hover { background-color: #2C2C2A; }
        QPushButton:checked { background-color: #0C447C; border: 1px solid #378ADD; color: #E6F1FB; }
        QPushButton#primaryButton { background-color: #185FA5; border: 1px solid #378ADD; color: white; }
        QPushButton#primaryButton:hover { background-color: #0C447C; }
        QPushButton#removeStepButton {
            background-color: transparent; border: none; color: #888780;
            font-weight: 600; padding: 0px;
        }
        QPushButton#removeStepButton:hover { color: #E5484D; }
        QToolTip {
            background-color: #2C2C2A; color: #F1EFE8;
            border: 1px solid #5F5E5A; padding: 4px 6px;
        }
        QCheckBox { spacing: 8px; }
        QCheckBox::indicator {
            width: 16px; height: 16px; border-radius: 3px;
            border: 1px solid #888780; background-color: #201F22;
        }
        QCheckBox::indicator:hover { border: 1px solid #378ADD; }
        QCheckBox::indicator:checked {
            background-color: #185FA5; border: 1px solid #378ADD;
            image: url(:/resources/check.png);
        }
    )").arg(m_uiFontSize).arg(titleFontSize).arg(m_editorFontSize));
}

bool MainWindow::openDatabase(const QString& path)
{
    // Tear down whatever's currently loaded (connection, thread, UI
    // state) first, so this also works for switching databases mid-
    // session via the Database menu, not just the very first load.
    if (m_db.isOpen()) {
        m_db.close();
    }
    if (QSqlDatabase::contains(m_db.connectionName())) {
        const QString connectionName = m_db.connectionName();
        m_db = QSqlDatabase();  // release our handle before removing it
        QSqlDatabase::removeDatabase(connectionName);
    }
    resetUiForNewDatabase();
    m_lastOpenError.clear();

    m_db = QSqlDatabase::addDatabase("QSQLITE", "main_gui_connection");
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_lastOpenError = m_db.lastError().text();
        return false;
    }

    if (!m_schema.load(m_db)) {
        m_lastOpenError = m_schema.errorMessage();
        return false;
    }

    startQueryThread(path);
    m_titleLabel->setText(QFileInfo(path).fileName());
    setWindowTitle("WinSQLite — " + QFileInfo(path).fileName());
    setSchemaActionsEnabled(true);
    m_sqlModeBtn->setEnabled(true);
    m_historyBtn->setEnabled(true);
    m_executeBtn->setEnabled(true);

    syncSchemaUi();

    const QString initialFocus = defaultFocusTable();
    if (!initialFocus.isEmpty()) {
        startQueryFrom(initialFocus);
    }

    if (m_schema.hasVectorTables()) {
        m_statusLabel->setText("Vector tables (vec0) found - explore them via Tools > Vector space (3D)...");
    }

    return true;
}

void MainWindow::resetUiForNewDatabase()
{
    m_focusTable.clear();
    m_queryModel = QueryModel();
    m_sqlModeActive = false;
    m_sqlModeBtn->setChecked(false);
    m_rightStack->setCurrentIndex(0);
    m_sqlEditor->clear();
    m_diagramView->clear();
    clearLayout(m_stepsLayout);
    m_sqlPreview->clear();
    m_resultsTable->clear();
    m_resultsTable->setRowCount(0);
    m_resultsTable->setColumnCount(0);
    m_exportResultsBtn->setEnabled(false);
    m_statusLabel->clear();
    m_tableTree->clear();
    m_view3dBtn->setEnabled(false);

    // Don't carry a maximized results panel over into a new database.
    if (m_resultsMaximized) {
        onToggleMaximizeResults();
    }
}

void MainWindow::syncSchemaUi()
{
    populateTableTree();
    m_view3dBtn->setEnabled(m_schema.hasVectorTables());

    // Feed the raw-SQL autocomplete with every table and column name now
    // that the schema is known, on top of the built-in SQL keywords.
    QStringList schemaWords;
    for (const auto& table : m_schema.tables()) {
        schemaWords << table.name;
        for (const auto& col : table.columns) {
            schemaWords << col.name;
        }
    }
    // vec0 tables aren't in tables() but are queryable in SQL mode, so
    // offer them (and their columns) plus the common sqlite-vec functions
    // in autocomplete too.
    for (const auto& table : m_schema.vectorTables()) {
        schemaWords << table.name;
        for (const auto& col : table.columns) {
            schemaWords << col.name;
        }
    }
    if (m_schema.hasVectorTables()) {
        schemaWords << "MATCH" << "distance" << "vec_version" << "vec_f32" << "vec_to_json"
                    << "vec_length" << "vec_distance_L2" << "vec_distance_cosine";
    }
    m_sqlEditor->setSchemaWords(schemaWords);
}

QString MainWindow::defaultFocusTable() const
{
    if (m_schema.tables().isEmpty()) {
        return QString();
    }

    // First table that has at least one relation, so the initial diagram
    // isn't a lone box - falls back to the first table.
    for (const auto& table : m_schema.tables()) {
        if (!m_schema.relationsFor(table.name).isEmpty()) {
            return table.name;
        }
    }
    return m_schema.tables().first().name;
}

void MainWindow::populateTableTree()
{
    m_tableTree->clear();

    for (const auto& table : m_schema.tables()) {
        auto* tableItem = new QTreeWidgetItem(m_tableTree, {table.name});
        QFont boldFont = tableItem->font(0);
        boldFont.setBold(true);
        tableItem->setFont(0, boldFont);
        tableItem->setData(0, Qt::UserRole, table.name);

        for (const auto& col : table.columns) {
            QString label = col.name + "  :  " + col.type;
            if (col.isPrimaryKey) {
                label += "  [PK]";
            }
            auto* colItem = new QTreeWidgetItem(tableItem, {label});
            colItem->setData(0, Qt::UserRole, table.name);
            colItem->setForeground(0, QColor("#888780"));
        }

        for (const auto& index : m_schema.indexesFor(table.name)) {
            QString label = "⚡ " + index.name + "  (" + index.columns.join(", ") + ")";
            if (index.isUnique) {
                label += "  [UNIQUE]";
            }
            auto* indexItem = new QTreeWidgetItem(tableItem, {label});
            indexItem->setData(0, Qt::UserRole, table.name);
            indexItem->setData(0, Qt::UserRole + 3, index.name);  // marks an index row (see context menu)
            indexItem->setForeground(0, QColor("#1D9E75"));
        }
    }

    // sqlite-vec (vec0) virtual tables. Qt's bundled SQLite has no vec0
    // module, so they can't be queried or joined from here: they are listed
    // (name + columns) but carry no UserRole table name, only the
    // UserRole + 1 marker that onTreeItemClicked() looks for.
    for (const auto& table : m_schema.vectorTables()) {
        auto* tableItem = new QTreeWidgetItem(m_tableTree, {table.name + "  [vec0]"});
        QFont boldFont = tableItem->font(0);
        boldFont.setBold(true);
        tableItem->setFont(0, boldFont);
        tableItem->setForeground(0, QColor("#1D9E75"));
        tableItem->setData(0, Qt::UserRole + 1, true);
        tableItem->setData(0, Qt::UserRole + 2, table.name);  // real table name, for the click handler
        tableItem->setToolTip(0, "Click: preview rows in SQL mode  |  Double-click or right-click: 3D view");

        for (const auto& col : table.columns) {
            QString label = col.name;
            if (!col.type.isEmpty()) {
                label += "  :  " + col.type;
            }
            if (col.isPrimaryKey) {
                label += "  [PK]";
            }
            auto* colItem = new QTreeWidgetItem(tableItem, {label});
            colItem->setData(0, Qt::UserRole + 1, true);
            colItem->setForeground(0, QColor("#888780"));
        }
    }

    m_tableTree->collapseAll();
}

void MainWindow::onView3dClicked()
{
    const auto& vecTables = m_schema.vectorTables();
    if (vecTables.isEmpty()) {
        QMessageBox::information(this, "3D view", "This database has no sqlite-vec (vec0) tables.");
        return;
    }

    // Which table: the one the last SQL result came from (so its rows light
    // up in 3D), else the vec0 table selected in the list, else the only
    // one, else ask.
    QString name;
    if (!m_lastResultRowids.isEmpty() && !m_lastResultVecTable.isEmpty()) {
        name = m_lastResultVecTable;
    }
    if (name.isEmpty()) {
        if (QTreeWidgetItem* current = m_tableTree->currentItem()) {
            QTreeWidgetItem* top = current->parent() ? current->parent() : current;
            if (top->data(0, Qt::UserRole + 1).toBool()) {
                name = top->data(0, Qt::UserRole + 2).toString();
            }
        }
    }
    if (name.isEmpty()) {
        if (vecTables.size() == 1) {
            name = vecTables.first().name;
        } else {
            QStringList names;
            for (const auto& table : vecTables) {
                names << table.name;
            }
            bool ok = false;
            name = QInputDialog::getItem(this, "3D view", "vec0 table:", names, 0, false, &ok);
            if (!ok) {
                return;
            }
        }
    }
    openVectorSpace(name);
}

void MainWindow::openVectorSpace(const QString& tableName)
{
    if (!m_db.isOpen() || tableName.isEmpty()) {
        return;
    }
    if (!vec::Database::isValidIdentifier(tableName)) {
        QMessageBox::warning(this, "3D view",
                             "The 3D view only supports table names made of letters, digits and underscores.");
        return;
    }

    // ---- Hover text for the points ------------------------------------
    // Find the table that holds the text belonging to this vec0 table by
    // name: vec_documents -> documents, message_embeddings -> messages, ...
    QString baseName = tableName;
    for (const QString& prefix : {QStringLiteral("vec0_"), QStringLiteral("vec_")}) {
        if (baseName.startsWith(prefix, Qt::CaseInsensitive)) {
            baseName.remove(0, prefix.size());
            break;
        }
    }
    for (const QString& suffix : {QStringLiteral("_embeddings"), QStringLiteral("_embedding"),
                                  QStringLiteral("_vectors"), QStringLiteral("_vector"),
                                  QStringLiteral("_vec"), QStringLiteral("_emb")}) {
        if (baseName.endsWith(suffix, Qt::CaseInsensitive)) {
            baseName.chop(suffix.size());
            break;
        }
    }
    QStringList candidates = {baseName, baseName + "s"};
    if (baseName.endsWith('s', Qt::CaseInsensitive)) {
        candidates << baseName.left(baseName.size() - 1);
    }

    const Table* sourceTable = nullptr;
    for (const QString& candidate : candidates) {
        for (const auto& table : m_schema.tables()) {
            if (table.name.compare(candidate, Qt::CaseInsensitive) == 0) {
                sourceTable = &table;
                break;
            }
        }
        if (sourceTable) {
            break;
        }
    }

    vec::ScatterView3D::LabelFn labelFn;  // empty = the 3D view's default "rowid N"
    if (sourceTable) {
        const auto hasColumn = [sourceTable](const QString& name) {
            for (const auto& col : sourceTable->columns) {
                if (col.name.compare(name, Qt::CaseInsensitive) == 0) {
                    return col.name;  // real spelling
                }
            }
            return QString();
        };

        // Which columns to show, in order, each cut to a max length:
        // a headline (title/name), a category, and a body snippet.
        QVector<QPair<QString, int>> shown;
        const auto addFirstOf = [&](const QStringList& names, int maxLen) {
            for (const QString& name : names) {
                const QString real = hasColumn(name);
                if (!real.isEmpty()) {
                    shown.push_back({real, maxLen});
                    return;
                }
            }
        };
        addFirstOf({"category", "type", "kind", "role"}, 30);
        addFirstOf({"title", "name", "subject", "label"}, 70);
        addFirstOf({"content", "text", "message", "body", "description", "summary"}, 110);

        if (shown.isEmpty()) {  // no well-known names: take the first text columns
            for (const auto& col : sourceTable->columns) {
                if (!col.isPrimaryKey
                    && (col.type.contains("TEXT", Qt::CaseInsensitive) || col.type.contains("CHAR", Qt::CaseInsensitive))) {
                    shown.push_back({col.name, 100});
                    if (shown.size() == 2) {
                        break;
                    }
                }
            }
        }

        QString idColumnName = "rowid";
        for (const auto& col : sourceTable->columns) {
            if (col.isPrimaryKey) {
                idColumnName = col.name;
                break;
            }
        }

        if (!shown.isEmpty()) {
            QStringList quotedColumns;
            for (const auto& entry : shown) {
                quotedColumns << "\"" + entry.first + "\"";
            }
            const QString sql = QString("SELECT %1 FROM \"%2\" WHERE \"%3\" = ?")
                                    .arg(quotedColumns.join(", "), sourceTable->name,
                                         idColumnName == "rowid" ? QString("rowid") : idColumnName);
            // Hovering asks for the same point again and again: cache the text.
            auto cache = std::make_shared<QHash<qint64, QString>>();
            labelFn = [this, sql, shown, cache](qint64 rowid) -> QString {
                if (const auto it = cache->constFind(rowid); it != cache->constEnd()) {
                    return it.value();
                }
                QString text = QString("rowid %1").arg(rowid);  // fallback
                if (m_db.isOpen()) {
                    QSqlQuery query(m_db);
                    query.prepare(sql);
                    query.addBindValue(rowid);
                    if (query.exec() && query.next()) {
                        QStringList parts;
                        for (int i = 0; i < shown.size(); ++i) {
                            QString value = query.value(i).toString().simplified();
                            if (value.size() > shown.at(i).second) {
                                value = value.left(shown.at(i).second) + QStringLiteral("…");
                            }
                            if (!value.isEmpty()) {
                                parts << value;
                            }
                        }
                        if (!parts.isEmpty()) {
                            text = QString("#%1 · %2").arg(rowid).arg(parts.join(" · "));
                        }
                    }
                }
                cache->insert(rowid, text);
                return text;
            };
        }
    }

    // The explorer opens its own sqlite-vec connection to the same file.
    auto* dialog = new vec::VectorExplorerDialog(m_db.databaseName(), tableName, std::move(labelFn), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // Clicking a point in 3D shows that record in the results grid.
    // A vec0 table that declares its own primary key (e.g. "id") is addressed
    // by that column; a plain one by rowid.
    QString idColumn = "rowid";
    for (const auto& vecTable : m_schema.vectorTables()) {
        if (vecTable.name.compare(tableName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        for (const auto& col : vecTable.columns) {
            if (col.isPrimaryKey) {
                idColumn = col.name;
                break;
            }
        }
    }

    connect(dialog, &vec::VectorExplorerDialog::recordActivated, this, [this, tableName, idColumn](qint64 rowid) {
        const QString sql = (idColumn == "rowid")
            ? QString("SELECT rowid, * FROM \"%1\" WHERE rowid = %2;").arg(tableName).arg(rowid)
            : QString("SELECT * FROM \"%1\" WHERE \"%2\" = %3;").arg(tableName, idColumn).arg(rowid);
        if (!m_sqlModeActive) {
            m_sqlModeBtn->setChecked(true);
        }
        m_sqlEditor->setPlainText(sql);
        m_queryFromVecClick = true;  // see captureVectorResult()
        runQuery(sql);
        if (!m_queryRunning) {
            m_queryFromVecClick = false;  // query never started (busy / no database)
        }
    });
    dialog->show();

    m_vecDialog = dialog;
    m_vecDialogTable = tableName;

    // If the last SQL result came from this table (e.g. a MATCH ... AND k = 10
    // search), light those rows up straight away.
    if (!m_lastResultRowids.isEmpty() && m_lastResultVecTable.compare(tableName, Qt::CaseInsensitive) == 0) {
        dialog->highlightRowids(
            m_lastResultRowids,
            tr("SQL result: %1 vectors highlighted in %2.").arg(m_lastResultRowids.size()).arg(tableName));
    }
}

void MainWindow::onTreeItemClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);
    if (!item) {
        return;
    }

    // A vec0 table (or one of its columns). vec0 tables aren't in the query
    // builder's schema, so clicking one switches to SQL mode and runs a
    // preview query (the worker connection has sqlite-vec loaded). The user
    // can then edit the SQL freely, e.g. add a MATCH ... AND k = ... clause.
    if (item->data(0, Qt::UserRole + 1).toBool()) {
        if (item->parent() != nullptr) {
            return;  // a column row: nothing to do
        }
        item->setExpanded(!item->isExpanded());

        QString vecName = item->data(0, Qt::UserRole + 2).toString();
        if (vecName.isEmpty()) {
            return;
        }
        vecName.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        const QString previewSql = QString("SELECT * FROM \"%1\" LIMIT 100;").arg(vecName);

        if (!m_sqlModeActive) {
            m_sqlModeBtn->setChecked(true);  // fires onSqlModeToggled -> SQL page
        }
        m_sqlEditor->setPlainText(previewSql);
        runQuery(previewSql);
        return;
    }

    // Table rows and their column/index children all carry the owning
    // table name in UserRole, so clicking any of them also focuses that
    // table.
    const QString tableName = item->data(0, Qt::UserRole).toString();
    onTableActivated(tableName);

    // Clicking a table row toggles its expanded state, revealing columns.
    if (item->parent() == nullptr) {
        item->setExpanded(!item->isExpanded());
    }
}

void MainWindow::onTableActivated(const QString& tableName)
{
    if (tableName.isEmpty()) {
        return;
    }

    if (m_sqlModeActive) {
        // In SQL mode, clicking a table in the tree inserts its name into
        // the raw SQL at the cursor instead of touching the builder.
        m_sqlEditor->insertPlainText(tableName);
        m_sqlEditor->setFocus();
        return;
    }

    // Picking a table in the tree means "start a query from this table".
    // If it's already part of the query being built, leave the query alone.
    if (m_queryModel.involvedTables().contains(tableName)) {
        return;
    }
    startQueryFrom(tableName);
}

void MainWindow::startQueryFrom(const QString& tableName)
{
    if (tableName.isEmpty()) {
        return;
    }
    m_focusTable = tableName;

    m_queryModel = QueryModel();
    m_queryModel.setFromTable(tableName);

    refreshQueryPanel();
}

void MainWindow::onJoinCandidateClicked(const QString& tableName)
{
    // Every way this table can be joined to what's already in the query
    // (usually one; several when e.g. two columns both reference it).
    QVector<JoinCandidate> options;
    for (const auto& c : m_schema.joinCandidates(currentQueryTables())) {
        if (c.table == tableName) {
            options.push_back(c);
        }
    }
    if (options.isEmpty()) {
        return;
    }

    int chosen = 0;
    if (options.size() > 1) {
        QMenu menu(this);
        for (const auto& c : options) {
            const auto& fk = c.relation;
            menu.addAction(QString("%1.%2 = %3.%4").arg(fk.fromTable, fk.fromColumn, fk.toTable, fk.toColumn));
        }
        QAction* picked = menu.exec(QCursor::pos());
        if (!picked) {
            return;
        }
        chosen = menu.actions().indexOf(picked);
        if (chosen < 0) {
            return;
        }
    }

    addJoinToQuery(options[chosen]);
}

void MainWindow::addJoinToQuery(const JoinCandidate& candidate)
{
    if (m_sqlModeActive) {
        insertJoinIntoSql(candidate);   // textChanged -> refreshDiagram()
    } else {
        m_queryModel.addJoin(candidate.table, candidate.relation);
        refreshQueryPanel();            // also refreshes the diagram
    }
}

void MainWindow::refreshQueryPanel()
{
    clearLayout(m_stepsLayout);

    // Builds one row in the steps list. onRemove is empty for the FROM
    // row, since the base table can't be removed on its own.
    auto addStepRow = [this](const QString& keyword, const QString& value,
                              std::function<void()> onRemove) {
        auto* row = new QWidget();
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 4, 0, 4);

        auto* label = new QLabel(QString("<span style='color:#888780'>%1</span> "
                                          "<span style='color:#E6F1FB; font-weight:600'>%2</span>")
                                      .arg(keyword, value));
        label->setWordWrap(true);
        rowLayout->addWidget(label, 1);

        if (onRemove) {
            auto* removeBtn = new QPushButton("\u00D7");
            removeBtn->setObjectName("removeStepButton");
            removeBtn->setFixedSize(20, 20);
            removeBtn->setToolTip("Remove step");
            connect(removeBtn, &QPushButton::clicked, this, onRemove);
            rowLayout->addWidget(removeBtn);
        }

        m_stepsLayout->addWidget(row);
    };

    addStepRow("FROM", m_queryModel.fromTable(), nullptr);

    for (int i = 0; i < m_queryModel.joins().size(); ++i) {
        const auto& join = m_queryModel.joins()[i];
        addStepRow("JOIN", join.table, [this, i]() { onRemoveJoinClicked(i); });
    }

    for (int i = 0; i < m_queryModel.conditions().size(); ++i) {
        const auto& cond = m_queryModel.conditions()[i];
        addStepRow("WHERE", cond.column + " " + cond.op + " " + cond.value,
                   [this, i]() { onRemoveConditionClicked(i); });
    }

    m_sqlPreview->setPlainText(m_queryModel.toSql());
    refreshDiagram();
}

void MainWindow::onAddStepClicked()
{
    if (m_queryModel.isEmpty()) {
        return;
    }

    QMenu menu(this);
    QAction* joinAction = menu.addAction("Join related table (JOIN)…");
    QAction* conditionAction = menu.addAction("Add condition (WHERE)…");
    menu.addSeparator();
    QAction* resetAction = menu.addAction("Start over (keep only FROM)");
    QAction* chosen = menu.exec(m_addStepBtn->mapToGlobal(QPoint(0, m_addStepBtn->height())));

    if (chosen == joinAction) {
        addJoinStep();
    } else if (chosen == conditionAction) {
        addConditionStep();
    } else if (chosen == resetAction) {
        startQueryFrom(m_queryModel.fromTable());
    }
}

void MainWindow::addJoinStep()
{
    // Same candidate list the diagram draws as dashed nodes, so the
    // dialog and the diagram always offer exactly the same joins.
    const QVector<JoinCandidate> candidates = m_schema.joinCandidates(m_queryModel.involvedTables());
    if (candidates.isEmpty()) {
        QMessageBox::information(this, "No available relations",
                                  "All related tables are already included in the query.");
        return;
    }

    QStringList labels;
    for (const auto& c : candidates) {
        const auto& fk = c.relation;
        labels << QString("%1  (%2.%3 → %4.%5)")
                      .arg(c.table, fk.fromTable, fk.fromColumn, fk.toTable, fk.toColumn);
    }

    bool ok = false;
    const QString choice = QInputDialog::getItem(this, "Join table",
                                                   "Select a relation for JOIN:",
                                                   labels, 0, false, &ok);
    if (!ok) {
        return;
    }

    const int index = labels.indexOf(choice);
    if (index < 0) {
        return;
    }

    addJoinToQuery(candidates[index]);
}

void MainWindow::addConditionStep()
{
    const QVector<QString> involved = m_queryModel.involvedTables();
    QStringList columnLabels;

    // Vector columns (vec0 "float[N]" etc.) can't be compared with =, <, LIKE.
    // They get one special entry instead: a nearest-neighbour condition.
    struct VecChoice { QString table; QString column; QString pk; };
    QHash<QString, VecChoice> vecChoices;
    const auto isVectorType = [](const QString& type) {
        const QString t = type.trimmed().toLower();
        return t.startsWith("float[") || t.startsWith("int8[") || t.startsWith("bit[");
    };

    for (const auto& tableName : involved) {
        const Table* table = m_schema.findAnyTable(tableName);
        if (!table) {
            continue;
        }
        const bool isVecTable = m_schema.isVectorTable(tableName);
        QString pk = "rowid";
        for (const auto& col : table->columns) {
            if (col.isPrimaryKey) {
                pk = col.name;
                break;
            }
        }
        for (const auto& col : table->columns) {
            if (isVecTable && isVectorType(col.type)) {
                const QString label = tableName + "." + col.name + "  (nearest neighbours)";
                vecChoices.insert(label, {tableName, col.name, pk});
                columnLabels << label;
            } else {
                columnLabels << (tableName + "." + col.name);
            }
        }
    }

    if (columnLabels.isEmpty()) {
        QMessageBox::information(this, "No available columns",
                                  "The query doesn't have any tables with columns yet.");
        return;
    }

    bool ok = false;
    const QString column = QInputDialog::getItem(this, "New condition",
                                                   "Column:", columnLabels, 0, false, &ok);
    if (!ok) {
        return;
    }

    if (vecChoices.contains(column)) {
        const VecChoice vc = vecChoices.value(column);
        const int srcId = QInputDialog::getInt(this, "Nearest neighbours",
                                               QString("Find rows most similar to %1.%2 =").arg(vc.table, vc.pk),
                                               1, -2147483647, 2147483647, 1, &ok);
        if (!ok) {
            return;
        }
        const int k = QInputDialog::getInt(this, "Nearest neighbours", "How many neighbours (k):",
                                           5, 1, 10000, 1, &ok);
        if (!ok) {
            return;
        }
        Condition vecCondition;
        vecCondition.column = vc.table + "." + vc.column;
        vecCondition.op = "MATCH";
        vecCondition.value = QString("(SELECT %1 FROM %2 WHERE %3 = %4)\n  AND k = %5")
                                 .arg(vc.column, vc.table, vc.pk).arg(srcId).arg(k);
        m_queryModel.addCondition(vecCondition);
        refreshQueryPanel();
        return;
    }

    static const QStringList operators = {"=", "!=", ">", "<", ">=", "<=", "LIKE"};
    const QString op = QInputDialog::getItem(this, "New condition",
                                              "Operator:", operators, 0, false, &ok);
    if (!ok) {
        return;
    }

    const QString rawValue = QInputDialog::getText(this, "New condition",
                                                     "Value:", QLineEdit::Normal,
                                                     QString(), &ok);
    if (!ok || rawValue.isEmpty()) {
        return;
    }

    Condition condition;
    condition.column = column;
    condition.op = op;
    condition.value = sqlLiteral(rawValue);
    m_queryModel.addCondition(condition);

    refreshQueryPanel();
}

void MainWindow::onRemoveJoinClicked(int index)
{
    m_queryModel.removeJoin(index);
    refreshQueryPanel();
}

void MainWindow::onRemoveConditionClicked(int index)
{
    m_queryModel.removeCondition(index);
    refreshQueryPanel();
}

void MainWindow::onSqlModeToggled(bool checked)
{
    m_sqlModeActive = checked;

    if (checked) {
        // Seed the editable box with whatever the builder currently
        // represents, then hand control over to free-text SQL.
        m_sqlEditor->setPlainText(m_queryModel.toSql());
        m_rightStack->setCurrentIndex(1);
        m_addStepBtn->setEnabled(false);
    } else {
        // Leaving SQL mode discards manual edits and restores the
        // builder's view of the query, per the "escape hatch" design.
        m_rightStack->setCurrentIndex(0);
        m_addStepBtn->setEnabled(true);
        refreshQueryPanel();
    }
}

void MainWindow::onExecuteClicked()
{
    const QString sql = m_sqlModeActive ? m_sqlEditor->toPlainText().trimmed()
                                         : m_queryModel.toSql();
    if (sql.isEmpty()) {
        showError("No query to execute.");
        return;
    }
    runQuery(sql);
}

void MainWindow::onSqlTextChanged()
{
    if (m_sqlModeActive) {
        refreshDiagram();
    }
}

QVector<QString> MainWindow::sqlQueryTables() const
{
    const QString sql = m_sqlEditor->toPlainText();

    // Crude but effective: the table right after FROM and every table
    // right after JOIN, so the diagram tracks whatever is being typed
    // without needing a full SQL parser.
    static const QRegularExpression fromPattern(R"(\bFROM\s+([A-Za-z_][A-Za-z0-9_]*))",
                                                  QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression joinPattern(R"(\bJOIN\s+([A-Za-z_][A-Za-z0-9_]*))",
                                                  QRegularExpression::CaseInsensitiveOption);

    QVector<QString> tables;
    const auto fromMatch = fromPattern.match(sql);
    if (!fromMatch.hasMatch()) {
        return tables;
    }
    const QString fromTable = resolveTableName(fromMatch.captured(1));
    if (fromTable.isEmpty()) {
        return tables;
    }
    tables.push_back(fromTable);

    auto matches = joinPattern.globalMatch(sql);
    while (matches.hasNext()) {
        const QString joinTable = resolveTableName(matches.next().captured(1));
        if (!joinTable.isEmpty() && !tables.contains(joinTable)) {
            tables.push_back(joinTable);
        }
    }
    return tables;
}

QVector<QString> MainWindow::currentQueryTables() const
{
    return m_sqlModeActive ? sqlQueryTables() : m_queryModel.involvedTables();
}

void MainWindow::refreshDiagram()
{
    QVector<QString> tables;
    QVector<ForeignKey> joined;

    if (m_sqlModeActive) {
        tables = sqlQueryTables();
        // Typed SQL has no model behind it, so a solid line is drawn for
        // every foreign key that links two tables named in the query.
        for (const auto& fk : m_schema.foreignKeys()) {
            if (tables.contains(fk.fromTable) && tables.contains(fk.toTable)) {
                joined.push_back(fk);
            }
        }
    } else {
        tables = m_queryModel.involvedTables();
        for (const auto& join : m_queryModel.joins()) {
            joined.push_back(join.onRelation);
        }
    }

    if (tables.isEmpty()) {
        // SQL mode: keep showing the last diagram while the person is
        // mid-edit (e.g. FROM not typed yet) rather than flickering empty.
        if (!m_sqlModeActive) {
            m_diagramView->clear();
        }
        return;
    }

    QSet<QString> vecNames;
    for (const auto& vt : m_schema.vectorTables()) {
        vecNames.insert(vt.name);
    }
    m_diagramView->setQueryGraph(tables, joined, m_schema.joinCandidates(tables), vecNames);
}

void MainWindow::insertJoinIntoSql(const JoinCandidate& candidate)
{
    const QString text = m_sqlEditor->toPlainText();

    static const QRegularExpression fromPattern(R"(\bFROM\b)", QRegularExpression::CaseInsensitiveOption);
    // The FROM/JOIN section ends at the first of these (or the end of text).
    static const QRegularExpression sectionEnd(
        R"(\b(WHERE|GROUP\s+BY|HAVING|ORDER\s+BY|LIMIT|UNION|INTERSECT|EXCEPT)\b|;)",
        QRegularExpression::CaseInsensitiveOption);

    const auto fromMatch = fromPattern.match(text);
    if (!fromMatch.hasMatch()) {
        return;
    }
    const auto endMatch = sectionEnd.match(text, fromMatch.capturedEnd());
    const int sectionEndPos = endMatch.hasMatch() ? int(endMatch.capturedStart()) : int(text.size());

    // Land right after the last clause, not after the blank space before WHERE.
    int pos = sectionEndPos;
    while (pos > 0 && text.at(pos - 1).isSpace()) {
        --pos;
    }

    const auto& fk = candidate.relation;
    QString clause = "\nJOIN " + candidate.table + "\n  ON " + fk.fromTable + "." + fk.fromColumn
                   + " = " + fk.toTable + "." + fk.toColumn;

    // If another clause follows on the same line ("... FROM a WHERE ..."),
    // move it onto its own line so the result reads like generated SQL.
    // Otherwise leave the person's own whitespace exactly as it was.
    int replaceEnd = pos;
    if (endMatch.hasMatch() && sectionEndPos > pos && !text.mid(pos, sectionEndPos - pos).contains('\n')) {
        clause += "\n";
        replaceEnd = sectionEndPos;
    }

    // Through a cursor on the document, so Ctrl+Z undoes just this JOIN.
    QTextCursor cursor(m_sqlEditor->document());
    cursor.setPosition(pos);
    cursor.setPosition(replaceEnd, QTextCursor::KeepAnchor);
    cursor.insertText(clause);
}

QString MainWindow::resolveTableName(const QString& rawName) const
{
    for (const auto& table : m_schema.tables()) {
        if (table.name.compare(rawName, Qt::CaseInsensitive) == 0) {
            return table.name;
        }
    }
    for (const auto& table : m_schema.vectorTables()) {
        if (table.name.compare(rawName, Qt::CaseInsensitive) == 0) {
            return table.name;
        }
    }
    return QString();
}

void MainWindow::startQueryThread(const QString& dbPath)
{
    // Opening a database is not a one-time thing (Database menu lets the
    // user switch mid-session), so always tear down any previous worker first.
    stopQueryThread();

#ifdef Q_OS_WASM
    // WebAssembly build has no pthread support: the worker lives on the
    // GUI thread. Queued connection keeps "Running..." visible first.
    m_queryThread = nullptr;
    m_queryWorker = new QueryWorker(dbPath);
    connect(this, &MainWindow::requestQuery, m_queryWorker, &QueryWorker::execute, Qt::QueuedConnection);
    connect(m_queryWorker, &QueryWorker::succeeded, this, &MainWindow::onQuerySucceeded);
    connect(m_queryWorker, &QueryWorker::failed, this, &MainWindow::onQueryFailed);
#else
    m_queryThread = new QThread(this);
    m_queryWorker = new QueryWorker(dbPath);
    m_queryWorker->moveToThread(m_queryThread);

    // requestQuery() is emitted on the GUI thread and delivered to
    // execute() on m_queryThread; succeeded()/failed() make the return
    // trip the same way (automatically queued: different threads).
    connect(this, &MainWindow::requestQuery, m_queryWorker, &QueryWorker::execute);
    connect(m_queryWorker, &QueryWorker::succeeded, this, &MainWindow::onQuerySucceeded);
    connect(m_queryWorker, &QueryWorker::failed, this, &MainWindow::onQueryFailed);

    m_queryThread->start();
#endif
}

void MainWindow::stopQueryThread()
{
    if (m_queryThread) {
        m_queryThread->quit();
        m_queryThread->wait();
        // The worker was moved onto the thread, not parented to it, so it
        // must be deleted explicitly once the thread has fully stopped.
        delete m_queryWorker;
        delete m_queryThread;
    } else if (m_queryWorker) {
        delete m_queryWorker;  // WASM: worker lives on the GUI thread
    }
    m_queryWorker = nullptr;
    m_queryThread = nullptr;
    m_queryRunning = false;
}

void MainWindow::runQuery(const QString& sql)
{
#ifdef Q_OS_WASM
    if (!m_queryWorker) {
#else
    if (!m_queryThread || !m_queryWorker) {
#endif
        showError("Database is not open.");
        return;
    }

    if (m_queryRunning) {
        // A previous query is still executing; let it finish rather
        // than piling up requests on the worker.
        return;
    }

    // Direct in-place editing of the results grid is only safe for the
    // simplest possible query - "SELECT * FROM table", no WHERE/JOIN -
    // since only then does every displayed row map 1:1 onto a real row
    // we can find again by primary key. Anything else stays read-only.
    static const QRegularExpression simpleSelectPattern(
        R"(^\s*SELECT\s*\*\s*FROM\s+([A-Za-z_][A-Za-z0-9_]*)\s*;?\s*$)", QRegularExpression::CaseInsensitiveOption);
    const auto match = simpleSelectPattern.match(sql);
    const QString candidateTable = match.hasMatch() ? resolveTableName(match.captured(1)) : QString();
    const Table* candidateSchema = candidateTable.isEmpty() ? nullptr : m_schema.findTable(candidateTable);

    m_editableTable.clear();
    m_editablePkColumns.clear();
    if (candidateSchema) {
        for (const auto& col : candidateSchema->columns) {
            if (col.isPrimaryKey) {
                m_editablePkColumns << col.name;
            }
        }
        if (!m_editablePkColumns.isEmpty()) {
            m_editableTable = candidateTable;
        }
    }

    m_queryRunning = true;
    m_executeBtn->setEnabled(false);
    showRunning();

    m_lastSql = sql;
    emit requestQuery(sql);
}

void MainWindow::onQuerySucceeded(QStringList headers, ResultRows rows)
{
    persistDatabase();  // the query may have been an INSERT/UPDATE/DELETE/DDL
    m_queryRunning = false;
    m_executeBtn->setEnabled(true);
    // Queries fired by clicking a point in the 3D view are generated, not
    // typed by the person, so they don't belong in the history.
    if (!m_queryFromVecClick) {
        addToHistory(m_lastSql);
    }
    showResults(headers, rows);
    captureVectorResult(headers, rows);
}

void MainWindow::captureVectorResult(const QStringList& headers, const ResultRows& rows)
{
    m_lastResultRowids.clear();
    m_lastResultVecTable.clear();

    // The query that a click in the 3D view started returns just that one
    // record; the 3D view already shows its neighbours, so leave it alone.
    if (m_queryFromVecClick) {
        m_queryFromVecClick = false;
        return;
    }

    // Which vec0 table does this SQL read from?
    for (const auto& table : m_schema.vectorTables()) {
        const QRegularExpression nameRe("\\b" + QRegularExpression::escape(table.name) + "\\b",
                                        QRegularExpression::CaseInsensitiveOption);
        if (nameRe.match(m_lastSql).hasMatch()) {
            m_lastResultVecTable = table.name;
            break;
        }
    }
    if (m_lastResultVecTable.isEmpty()) {
        return;
    }

    // The result has to carry the vec0 record id, e.g.
    //   SELECT rowid, distance FROM t WHERE embedding MATCH ... AND k = 10
    // or, for a vec0 table with its own primary key, that column (e.g. "id").
    QStringList idNames = {"rowid"};
    for (const auto& vecTable : m_schema.vectorTables()) {
        if (vecTable.name.compare(m_lastResultVecTable, Qt::CaseInsensitive) != 0) {
            continue;
        }
        for (const auto& col : vecTable.columns) {
            if (col.isPrimaryKey) {
                idNames << col.name;
            }
        }
    }
    int rowidColumn = -1;
    for (int c = 0; c < headers.size() && rowidColumn < 0; ++c) {
        for (const QString& idName : idNames) {
            if (headers.at(c).compare(idName, Qt::CaseInsensitive) == 0) {
                rowidColumn = c;
                break;
            }
        }
    }
    if (rowidColumn < 0) {
        return;
    }
    for (const auto& row : rows) {
        bool ok = false;
        const qint64 rowid = row.value(rowidColumn).toLongLong(&ok);
        if (ok) {
            m_lastResultRowids.insert(rowid);
        }
    }

    if (m_vecDialog && !m_lastResultRowids.isEmpty()
        && m_vecDialogTable.compare(m_lastResultVecTable, Qt::CaseInsensitive) == 0) {
        m_vecDialog->highlightRowids(
            m_lastResultRowids,
            tr("SQL result: %1 vectors highlighted in %2.").arg(m_lastResultRowids.size()).arg(m_lastResultVecTable));
    }
}

void MainWindow::onQueryFailed(QString message)
{
    m_queryRunning = false;
    m_queryFromVecClick = false;
    m_executeBtn->setEnabled(true);
    m_editableTable.clear();
    m_editablePkColumns.clear();
    showError(message);
}

void MainWindow::showResults(const QStringList& headers, const ResultRows& rows)
{
    // Cells get populated programmatically below - this keeps those
    // changes from being mistaken for a user edit by onResultCellEdited.
    m_populatingResults = true;

    const int columnCount = headers.size();
    const bool editable = !m_editableTable.isEmpty();

    m_resultsTable->clear();
    m_resultsTable->setColumnCount(columnCount);
    m_resultsTable->setHorizontalHeaderLabels(headers);
    m_resultsTable->setEditTriggers(
        editable ? QAbstractItemView::EditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed)
                 : QAbstractItemView::NoEditTriggers);

    m_resultsTable->setRowCount(0);
    int rowCount = 0;
    for (const auto& row : rows) {
        m_resultsTable->insertRow(rowCount);

        // Stashed on column 0's item so onResultCellEdited() can look up
        // this row's original values (needed for the UPDATE's WHERE
        // clause) regardless of which cell the user actually edited.
        QVariantMap rowSnapshot;
        if (editable) {
            for (int c = 0; c < columnCount && c < row.size(); ++c) {
                rowSnapshot.insert(headers[c], row[c]);
            }
        }

        for (int c = 0; c < columnCount && c < row.size(); ++c) {
            auto* item = new QTableWidgetItem(row[c]);
            item->setToolTip(row[c]);  // full text for cells cut off by the column width cap
            if (!editable) {
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
            if (editable && c == 0) {
                item->setData(Qt::UserRole, rowSnapshot);
            }
            m_resultsTable->setItem(rowCount, c, item);
        }
        ++rowCount;
    }
    m_resultsTable->resizeColumnsToContents();
    // Cap very wide columns (long text, JSON, ...) so one column can't push
    // the rest off-screen; the user can still drag the header to widen it,
    // and the full value is in the cell's tooltip.
    constexpr int kMaxColWidth = 400;
    for (int c = 0; c < columnCount; ++c) {
        if (m_resultsTable->columnWidth(c) > kMaxColWidth) {
            m_resultsTable->setColumnWidth(c, kMaxColWidth);
        }
    }
    m_resultsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    m_resultsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_resultsTable->setTextElideMode(Qt::ElideRight);

    m_statusLabel->setStyleSheet("color: #888780;");
    m_statusLabel->setText(rowCount == 1 ? "1 row" : QString("%1 rows").arg(rowCount));
    m_exportResultsBtn->setEnabled(columnCount > 0);

    m_populatingResults = false;
}

void MainWindow::showRunning()
{
    m_statusLabel->setStyleSheet("color: #888780;");
    m_statusLabel->setText("Running…");
}

void MainWindow::showError(const QString& message)
{
    m_resultsTable->clear();
    m_resultsTable->setRowCount(0);
    m_resultsTable->setColumnCount(0);
    m_exportResultsBtn->setEnabled(false);

    m_statusLabel->setStyleSheet("color: #E5484D;");
    m_statusLabel->setText("Error: " + message);
}

void MainWindow::setSchemaActionsEnabled(bool enabled)
{
    for (auto* action : m_schemaActions) {
        action->setEnabled(enabled);
    }
}

void MainWindow::loadSettings()
{
    QSettings settings;
    m_uiFontSize = settings.value(kSettingsUiFontSize, kDefaultUiFontSize).toInt();
    m_editorFontSize = settings.value(kSettingsEditorFontSize, kDefaultEditorFontSize).toInt();
    m_confirmDestructiveActions = settings.value(kSettingsConfirmDestructive, kDefaultConfirmDestructive).toBool();
    m_defaultDialogDir = settings.value(kSettingsDefaultDir, QString()).toString();
}

void MainWindow::loadHistory()
{
    QSettings settings;
    m_queryHistory = settings.value(kSettingsQueryHistory).toStringList();
    while (m_queryHistory.size() > kMaxHistoryEntries) {
        m_queryHistory.removeLast();
    }
}

void MainWindow::saveHistory()
{
    QSettings settings;
    settings.setValue(kSettingsQueryHistory, m_queryHistory);
    flushPersistentStorage(settings);
}

void MainWindow::addToHistory(const QString& sql)
{
    const QString trimmed = sql.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    // Re-running a query moves it back to the top instead of duplicating it.
    m_queryHistory.removeAll(trimmed);
    m_queryHistory.prepend(trimmed);
    while (m_queryHistory.size() > kMaxHistoryEntries) {
        m_queryHistory.removeLast();
    }
    saveHistory();
}

void MainWindow::loadSqlIntoEditor(const QString& sql)
{
    if (!m_sqlModeActive) {
        m_sqlModeBtn->setChecked(true);  // fires onSqlModeToggled -> SQL page
    }
    m_sqlEditor->setPlainText(sql);
}

void MainWindow::onHistoryClicked()
{
    QMenu menu(this);
    if (m_queryHistory.isEmpty()) {
        menu.addAction("No queries yet")->setEnabled(false);
    } else {
        for (const QString& sql : m_queryHistory) {
            // One-line label for the menu; the full text is in the tooltip.
            QString label = sql.simplified();
            if (label.size() > 90) {
                label = label.left(87) + "...";
            }
            QAction* action = menu.addAction(label);
            action->setToolTip(sql);
            connect(action, &QAction::triggered, this, [this, sql] { loadSqlIntoEditor(sql); });
        }
        menu.setToolTipsVisible(true);
        menu.addSeparator();
        QAction* clear = menu.addAction("Clear history");
        connect(clear, &QAction::triggered, this, [this] {
            m_queryHistory.clear();
            saveHistory();
        });
    }
    menu.exec(m_historyBtn->mapToGlobal(QPoint(0, m_historyBtn->height())));
}

void MainWindow::saveSettings()
{
    QSettings settings;
    settings.setValue(kSettingsUiFontSize, m_uiFontSize);
    settings.setValue(kSettingsEditorFontSize, m_editorFontSize);
    settings.setValue(kSettingsConfirmDestructive, m_confirmDestructiveActions);
    settings.setValue(kSettingsDefaultDir, m_defaultDialogDir);
    flushPersistentStorage(settings);
}

void MainWindow::rememberDialogDir(const QString& filePath)
{
    const QString dir = QFileInfo(filePath).absolutePath();
    if (dir.isEmpty() || dir == m_defaultDialogDir) {
        return;
    }
    m_defaultDialogDir = dir;
    saveSettings();
}

void MainWindow::onSettingsTriggered()
{
    SettingsDialog dialog(m_uiFontSize, m_editorFontSize, m_confirmDestructiveActions, m_defaultDialogDir, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_uiFontSize = dialog.uiFontSize();
    m_editorFontSize = dialog.editorFontSize();
    m_confirmDestructiveActions = dialog.confirmDestructive();
    m_defaultDialogDir = dialog.defaultDir();

    saveSettings();
    applyDarkTheme();
}

void MainWindow::onNewDatabaseTriggered()
{
#ifdef Q_OS_WASM
    // Browser: no real disk. The database is kept in the browser's own
    // storage (/persist, see main.cpp), so it is still there after a reload;
    // Save Database downloads a copy as a file.
    bool nameOk = false;
    QString name = QInputDialog::getText(this, "New Database", "Database name:", QLineEdit::Normal, "new", &nameOk);
    name = QFileInfo(name.trimmed()).fileName();
    if (!nameOk || name.isEmpty()) {
        return;
    }
    if (QFileInfo(name).suffix().isEmpty()) {
        name += ".sqlite";
    }
    QDir().mkpath(kWasmDbDir);
    QString path = kWasmDbDir + "/" + name;
    if (QFile::exists(path)) {
        const auto confirm = QMessageBox::question(
            this, "New Database", "A saved database named \"" + name + "\" already exists in this browser. Replace it?");
        if (confirm != QMessageBox::Yes) {
            return;
        }
    }
#else
    QString path = QFileDialog::getSaveFileName(this, "Create New SQLite Database", m_defaultDialogDir,
                                                  "SQLite databases (*.sqlite *.db *.sqlite3);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += ".sqlite";
    }
    rememberDialogDir(path);
#endif

    // Remove any old file so the database really does start empty.
    if (QFile::exists(path)) {
        QFile::remove(path);
    }

    if (!openDatabase(path)) {
        QMessageBox::critical(this, "Error", "Unable to create the database.");
        return;
    }
    persistDatabase();
}

void MainWindow::onOpenDatabaseTriggered()
{
    auto openFromPath = [this](const QString& path) {
        rememberDialogDir(path);
        if (!openDatabase(path)) {
            QString message = "Unable to open or load the database.";
            if (!m_lastOpenError.isEmpty()) {
                message += "\n\n" + m_lastOpenError;
            }
            QMessageBox::critical(this, "Error", message);
        }
    };

#ifdef Q_OS_WASM
    // Browser: the picked file is not on a path SQLite can reach. Get its
    // bytes, write them into the virtual filesystem, open that copy.
    QFileDialog::getOpenFileContent(
        "SQLite databases (*.sqlite *.db *.sqlite3);;All files (*)",
        [this, openFromPath](const QString& name, const QByteArray& data) {
            if (name.isEmpty()) {
                return;
            }
            QDir().mkpath(kWasmDbDir);
            const QString path = kWasmDbDir + "/" + QFileInfo(name).fileName();

            // Opening a file whose name matches a copy that is already saved in
            // the browser: without this question the old file from the disk
            // would silently replace the edited copy.
            if (QFile::exists(path)) {
                QMessageBox box(QMessageBox::Question, "Open Database",
                                "\"" + QFileInfo(name).fileName() + "\" is already saved in this browser, "
                                "with the changes you made earlier.\n\n"
                                "Open that saved copy, or replace it with the file you just picked?",
                                QMessageBox::NoButton, this);
                QPushButton* keepBtn = box.addButton("Open saved copy", QMessageBox::AcceptRole);
                QPushButton* replaceBtn = box.addButton("Replace with picked file", QMessageBox::DestructiveRole);
                box.addButton(QMessageBox::Cancel);
                box.exec();
                if (box.clickedButton() == keepBtn) {
                    openFromPath(path);
                    m_statusLabel->setText("Opened the saved copy of " + QFileInfo(name).fileName());
                    return;
                }
                if (box.clickedButton() != replaceBtn) {
                    return;
                }
            }

            QFile::remove(path);
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QMessageBox::critical(this, "Error", "Cannot write " + path);
                return;
            }
            f.write(data);
            f.close();
            persistDatabase();

            // WASM SQLite is built without WAL support. If the file header says
            // WAL (bytes 18/19 == 2), switch it to rollback-journal mode. Data
            // still sitting in a separate .db-wal file is NOT part of this copy.
            bool wasWal = false;
            if (data.size() > 20 && static_cast<unsigned char>(data[18]) == 2
                && static_cast<unsigned char>(data[19]) == 2) {
                QFile patch(path);
                if (patch.open(QIODevice::ReadWrite)) {
                    patch.seek(18);
                    patch.write(QByteArray(2, '\x01'));
                    patch.close();
                    wasWal = true;
                }
            }

            openFromPath(path);

            m_statusLabel->setStyleSheet("color: #888780;");
            m_statusLabel->setText(QString("Loaded %1: %2 bytes, %3 table(s)%4")
                                       .arg(QFileInfo(name).fileName())
                                       .arg(data.size())
                                       .arg(m_schema.tables().size() + m_schema.vectorTables().size())
                                       .arg(wasWal ? ", was WAL mode (converted)" : ""));
        });
#else
    const QString path = QFileDialog::getOpenFileName(this, "Open SQLite Database", m_defaultDialogDir,
                                                        "SQLite databases (*.sqlite *.db *.sqlite3);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    openFromPath(path);
#endif
}

bool MainWindow::execSchemaChange(const QString& sql, const QString& context)
{
    if (!m_db.isOpen()) {
        QMessageBox::warning(this, context, "No database is open.");
        return false;
    }

    QSqlQuery query(m_db);
    if (!query.exec(sql)) {
        QMessageBox::critical(this, context, query.lastError().text());
        return false;
    }
    persistDatabase();
    return true;
}

void MainWindow::applySchemaChange(const QString& sql, const QString& context)
{
    if (!execSchemaChange(sql, context)) {
        return;
    }
    reloadSchemaAndFocus();
}

void MainWindow::reloadSchemaAndFocus()
{
    persistDatabase();  // also covers SQL import, which runs its statements directly
    if (!m_schema.load(m_db)) {
        showError(m_schema.errorMessage());
        return;
    }

    syncSchemaUi();

    // Keep the query being built if everything it refers to still exists
    // (tables, and the foreign keys its JOINs rely on). Otherwise fall
    // back to the schema's default table.
    bool queryStillValid = !m_queryModel.isEmpty() && m_schema.findAnyTable(m_queryModel.fromTable());
    if (queryStillValid) {
        for (const auto& join : m_queryModel.joins()) {
            if (!m_schema.findAnyTable(join.table) || !m_schema.foreignKeys().contains(join.onRelation)) {
                queryStillValid = false;
                break;
            }
        }
    }

    if (queryStillValid) {
        refreshQueryPanel();
        return;
    }

    m_focusTable.clear();
    const QString target = defaultFocusTable();
    if (target.isEmpty()) {
        m_diagramView->clear();
        m_queryModel = QueryModel();
        refreshQueryPanel();
    } else {
        startQueryFrom(target);
    }
}

void MainWindow::onNewTableTriggered()
{
    CreateTableDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString tableName = dialog.tableName();
    if (m_schema.findTable(tableName)) {
        QMessageBox::warning(this, "New Table", "A table named \"" + tableName + "\" already exists.");
        return;
    }

    QStringList columnDefs;
    QStringList primaryKeyColumns;
    for (const auto& col : dialog.columns()) {
        QString def = col.name + " " + col.type;
        if (col.notNull) {
            def += " NOT NULL";
        }
        columnDefs << def;
        if (col.primaryKey) {
            primaryKeyColumns << col.name;
        }
    }
    if (!primaryKeyColumns.isEmpty()) {
        columnDefs << "PRIMARY KEY (" + primaryKeyColumns.join(", ") + ")";
    }

    const QString sql = "CREATE TABLE " + tableName + " (\n  " + columnDefs.join(",\n  ") + "\n);";
    applySchemaChange(sql, "New Table");
}

void MainWindow::onNewIndexTriggered()
{
    if (m_schema.tables().isEmpty()) {
        QMessageBox::information(this, "New Index", "Create a table first.");
        return;
    }

    CreateIndexDialog dialog(m_schema, m_focusTable, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString sql = QString("CREATE %1INDEX %2 ON %3 (%4);")
                             .arg(dialog.isUnique() ? "UNIQUE " : "", dialog.indexName(), dialog.tableName(),
                                  dialog.selectedColumns().join(", "));
    applySchemaChange(sql, "New Index");
}

namespace {

// Quotes an identifier for SQL ("" escapes a literal quote). The Edit
// Table/Index code uses it everywhere, so names created outside this app
// (spaces, keywords, ...) still work.
QString quoteIdent(const QString& name)
{
    QString escaped = name;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

}  // namespace

bool MainWindow::execSchemaBatch(const QStringList& statements, const QString& context)
{
    if (!m_db.isOpen()) {
        QMessageBox::warning(this, context, "No database is open.");
        return false;
    }
    if (!m_db.transaction()) {
        QMessageBox::critical(this, context, m_db.lastError().text());
        return false;
    }

    QSqlQuery query(m_db);
    for (const QString& sql : statements) {
        if (!query.exec(sql)) {
            const QString error = query.lastError().text();
            query.finish();
            m_db.rollback();
            QMessageBox::critical(this, context, error + "\n\nStatement:\n" + sql + "\n\nNothing was changed.");
            return false;
        }
        query.finish();
    }

    if (!m_db.commit()) {
        const QString error = m_db.lastError().text();
        m_db.rollback();
        QMessageBox::critical(this, context, error);
        return false;
    }

    persistDatabase();
    return true;
}

QString MainWindow::indexWhereClause(const QString& indexName) const
{
    QSqlQuery query(m_db);
    query.prepare("SELECT sql FROM sqlite_master WHERE type = 'index' AND name = ?");
    query.addBindValue(indexName);
    if (!query.exec() || !query.next()) {
        return QString();
    }

    // "CREATE INDEX name ON tbl (cols) WHERE condition"
    static const QRegularExpression whereRe(
        QStringLiteral(R"(\)\s*WHERE\s+(.+?)\s*;?\s*$)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = whereRe.match(query.value(0).toString());
    return match.hasMatch() ? " WHERE " + match.captured(1) : QString();
}

void MainWindow::onSaveDatabaseTriggered()
{
    if (!m_db.isOpen()) {
        QMessageBox::warning(this, "Save Database", "No database is open.");
        return;
    }

    const QString source = m_db.databaseName();
    if (!QFileInfo::exists(source)) {
        QMessageBox::critical(this, "Save Database", "The database file \"" + source + "\" was not found.");
        return;
    }

#ifdef Q_OS_WASM
    // Browser: the open database is a file in the in-memory filesystem, and
    // every edit has already been committed into it. Saving = handing that
    // file to the browser as a download (a real .sqlite file, unlike
    // Export Database, which writes an SQL script).
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::critical(this, "Save Database", "Could not read the database.");
        return;
    }
    QFileDialog::saveFileContent(file.readAll(), QFileInfo(source).fileName());
#else
    // Desktop: edits are written to the open file as they are made, so there
    // is nothing pending. "Save" here means saving a copy under a new name.
    const QString suggested = QDir(m_defaultDialogDir).filePath(QFileInfo(source).fileName());
    const QString path = QFileDialog::getSaveFileName(this, "Save Database As", suggested,
                                                       "SQLite databases (*.sqlite *.db *.sqlite3);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    rememberDialogDir(path);

    if (QFileInfo(path) == QFileInfo(source)) {
        QMessageBox::information(this, "Save Database",
                                  "This is the file that is already open - all changes are saved in it.");
        return;
    }
    if (QFile::exists(path) && !QFile::remove(path)) {
        QMessageBox::critical(this, "Save Database", "Could not replace \"" + path + "\".");
        return;
    }
    if (!QFile::copy(source, path)) {
        QMessageBox::critical(this, "Save Database", "Could not write to \"" + path + "\".");
        return;
    }
    m_statusLabel->setText("Saved a copy to " + QFileInfo(path).fileName());
#endif
}

void MainWindow::onEditIndexTriggered()
{
    if (m_schema.indexes().isEmpty()) {
        QMessageBox::information(this, "Edit Index", "There are no indexes to edit.");
        return;
    }

    QStringList labels;
    for (const auto& index : m_schema.indexes()) {
        labels << index.name + "  (" + index.table + ")";
    }

    bool ok = false;
    const QString choice = QInputDialog::getItem(this, "Edit Index", "Index:", labels, 0, false, &ok);
    if (!ok) {
        return;
    }

    const int chosenIndex = labels.indexOf(choice);
    if (chosenIndex < 0) {
        return;
    }
    editIndex(m_schema.indexes()[chosenIndex]);
}

void MainWindow::editIndex(const Index& indexRef)
{
    // Copy: the schema (and with it indexRef) is reloaded after the change.
    const Index index = indexRef;

    // Expression indexes (CREATE INDEX i ON t (lower(name))) report an empty
    // column name; re-creating one from column names alone would corrupt it.
    for (const QString& col : index.columns) {
        if (col.isEmpty()) {
            QMessageBox::information(this, "Edit Index",
                                      "Index \"" + index.name + "\" is based on an expression, which this "
                                      "dialog can't edit. Use the SQL editor (DROP INDEX / CREATE INDEX).");
            return;
        }
    }

    // Per-column DESC / COLLATE settings are not part of the dialog; carry
    // them over for the columns that stay in the index.
    QHash<QString, QString> columnSuffix;  // lower-cased column name -> " COLLATE x DESC"
    QSqlQuery xinfo(m_db);
    if (xinfo.exec("PRAGMA index_xinfo(" + quoteIdent(index.name) + ");")) {
        while (xinfo.next()) {
            if (xinfo.value("key").toInt() == 0) {
                continue;  // trailing rowid column, not part of the user's definition
            }
            QString suffix;
            const QString collation = xinfo.value("coll").toString();
            if (!collation.isEmpty() && collation.compare("BINARY", Qt::CaseInsensitive) != 0) {
                suffix += " COLLATE " + collation;
            }
            if (xinfo.value("desc").toInt() != 0) {
                suffix += " DESC";
            }
            columnSuffix.insert(xinfo.value("name").toString().toLower(), suffix);
        }
    }
    xinfo.finish();

    CreateIndexDialog dialog(m_schema, index, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList columnSql;
    for (const QString& col : dialog.selectedColumns()) {
        columnSql << quoteIdent(col) + columnSuffix.value(col.toLower());
    }

    const QString createSql = QString("CREATE %1INDEX %2 ON %3 (%4)%5;")
                                  .arg(dialog.isUnique() ? "UNIQUE " : "", quoteIdent(dialog.indexName()),
                                       quoteIdent(index.table), columnSql.join(", "),
                                       indexWhereClause(index.name));

    // SQLite has no ALTER INDEX: an edit is DROP + CREATE. One transaction,
    // so if the new definition is rejected (say UNIQUE over duplicate data)
    // the old index is still there.
    if (execSchemaBatch({"DROP INDEX " + quoteIdent(index.name) + ";", createSql}, "Edit Index")) {
        reloadSchemaAndFocus();
    }
}

void MainWindow::onEditTableTriggered()
{
    if (m_schema.tables().isEmpty()) {
        QMessageBox::information(this, "Edit Table", "There are no tables to edit.");
        return;
    }

    QStringList names;
    for (const auto& table : m_schema.tables()) {
        names << table.name;
    }

    bool ok = false;
    const QString table = QInputDialog::getItem(this, "Edit Table", "Table:", names,
                                                 qMax(0, names.indexOf(m_focusTable)), false, &ok);
    if (!ok) {
        return;
    }
    editTable(table);
}

void MainWindow::editTable(const QString& tableName)
{
    const Table* found = m_schema.findTable(tableName);
    if (!found) {
        return;
    }
    // Copy: the schema is reloaded after the change.
    const Table table = *found;

    CreateTableDialog dialog(table, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newName = dialog.tableName();
    const QVector<CreateTableDialog::ColumnDef> columns = dialog.columns();

    if (newName.compare(table.name, Qt::CaseInsensitive) != 0) {
        bool taken = false;
        for (const auto& t : m_schema.tables()) {
            taken = taken || t.name.compare(newName, Qt::CaseInsensitive) == 0;
        }
        for (const auto& t : m_schema.vectorTables()) {
            taken = taken || t.name.compare(newName, Qt::CaseInsensitive) == 0;
        }
        if (taken) {
            QMessageBox::warning(this, "Edit Table", "A table named \"" + newName + "\" already exists.");
            return;
        }
    }

    // Work out what changed. Existing columns are matched by their original
    // name (kept in the dialog), so a rename is not mistaken for drop + add.
    QSet<QString> keptColumns;
    for (const auto& col : columns) {
        if (!col.originalName.isEmpty()) {
            keptColumns.insert(col.originalName);
        }
    }
    QStringList droppedColumns;
    for (const auto& col : table.columns) {
        if (!keptColumns.contains(col.name)) {
            droppedColumns << col.name;
        }
    }
    QVector<std::pair<QString, QString>> renames;  // from, to
    for (const auto& col : columns) {
        if (!col.originalName.isEmpty() && col.name != col.originalName) {
            renames.push_back({col.originalName, col.name});
        }
    }

    // ALTER TABLE ... RENAME COLUMN needs SQLite 3.25, DROP COLUMN 3.35.
    QSqlQuery versionQuery(m_db);
    QVersionNumber sqliteVersion;
    if (versionQuery.exec("SELECT sqlite_version();") && versionQuery.next()) {
        sqliteVersion = QVersionNumber::fromString(versionQuery.value(0).toString());
    }
    versionQuery.finish();
    if (!sqliteVersion.isNull()) {
        if (!renames.isEmpty() && sqliteVersion < QVersionNumber(3, 25)) {
            QMessageBox::warning(this, "Edit Table",
                                  "Renaming columns needs SQLite 3.25 or newer (this one is " +
                                      sqliteVersion.toString() + ").");
            return;
        }
        if (!droppedColumns.isEmpty() && sqliteVersion < QVersionNumber(3, 35)) {
            QMessageBox::warning(this, "Edit Table",
                                  "Removing columns needs SQLite 3.35 or newer (this one is " +
                                      sqliteVersion.toString() + ").");
            return;
        }
    }

    if (!droppedColumns.isEmpty() && m_confirmDestructiveActions) {
        const auto confirm = QMessageBox::question(
            this, "Edit Table",
            "Delete column(s) " + droppedColumns.join(", ") + " from \"" + table.name +
                "\" together with all of their data? This cannot be undone.");
        if (confirm != QMessageBox::Yes) {
            return;
        }
    }

    // Statement order matters: drops first (frees names), then renames, then
    // adds, and the table rename last so every earlier statement can still
    // use the old table name. SQLite itself updates indexes, triggers, views
    // and foreign keys of other tables that mention a renamed table/column.
    const QString tableRef = quoteIdent(table.name);
    QStringList statements;

    for (const QString& col : droppedColumns) {
        statements << "ALTER TABLE " + tableRef + " DROP COLUMN " + quoteIdent(col) + ";";
    }

    // A rename can only run once its target name is free, which matters for
    // chains (a -> b, b -> c). A swap (a <-> b) can't be done in one step.
    QSet<QString> presentNames;  // lower-cased column names at this point of the script
    for (const auto& col : table.columns) {
        if (!droppedColumns.contains(col.name)) {
            presentNames.insert(col.name.toLower());
        }
    }
    auto pending = renames;
    while (!pending.isEmpty()) {
        bool progressed = false;
        for (int i = 0; i < pending.size(); ++i) {
            const QString from = pending[i].first;
            const QString to = pending[i].second;
            const bool onlyCaseChange = from.compare(to, Qt::CaseInsensitive) == 0;
            if (!onlyCaseChange && presentNames.contains(to.toLower())) {
                continue;  // target still used by another column: try the others first
            }
            statements << "ALTER TABLE " + tableRef + " RENAME COLUMN " + quoteIdent(from) + " TO " +
                              quoteIdent(to) + ";";
            presentNames.remove(from.toLower());
            presentNames.insert(to.toLower());
            pending.removeAt(i);
            progressed = true;
            break;
        }
        if (!progressed) {
            QMessageBox::warning(this, "Edit Table",
                                  "These column renames swap names with each other, which can't be done in "
                                  "one step. Rename one of the columns to a temporary name first, save, "
                                  "then edit again.");
            return;
        }
    }

    for (const auto& col : columns) {
        if (!col.originalName.isEmpty()) {
            continue;
        }
        QString def = quoteIdent(col.name);
        if (!col.type.isEmpty()) {
            def += " " + col.type;
        }
        statements << "ALTER TABLE " + tableRef + " ADD COLUMN " + def + ";";
    }

    if (newName != table.name) {
        statements << "ALTER TABLE " + tableRef + " RENAME TO " + quoteIdent(newName) + ";";
    }

    if (statements.isEmpty()) {
        return;  // nothing changed
    }

    // Remember before the schema reload whether the query being built reads
    // this table, so it can be pointed at the table's new name afterwards.
    const bool queryUsedTable = currentQueryTables().contains(table.name);

    if (!execSchemaBatch(statements, "Edit Table")) {
        return;
    }
    reloadSchemaAndFocus();
    if (queryUsedTable && m_schema.findTable(newName)) {
        startQueryFrom(newName);
    }
}

void MainWindow::onDropTableTriggered()
{
    if (m_schema.tables().isEmpty()) {
        QMessageBox::information(this, "Drop Table", "There are no tables to drop.");
        return;
    }

    QStringList names;
    for (const auto& table : m_schema.tables()) {
        names << table.name;
    }

    bool ok = false;
    const QString table = QInputDialog::getItem(this, "Drop Table", "Table:", names, 0, false, &ok);
    if (!ok) {
        return;
    }

    if (m_confirmDestructiveActions) {
        const auto confirm = QMessageBox::question(
            this, "Drop Table", "Delete table \"" + table + "\" and all of its data? This cannot be undone.");
        if (confirm != QMessageBox::Yes) {
            return;
        }
    }

    applySchemaChange("DROP TABLE " + table + ";", "Drop Table");
}

void MainWindow::onDropIndexTriggered()
{
    if (m_schema.indexes().isEmpty()) {
        QMessageBox::information(this, "Drop Index", "There are no indexes to drop.");
        return;
    }

    QStringList labels;
    for (const auto& index : m_schema.indexes()) {
        labels << index.name + "  (" + index.table + ")";
    }

    bool ok = false;
    const QString choice = QInputDialog::getItem(this, "Drop Index", "Index:", labels, 0, false, &ok);
    if (!ok) {
        return;
    }

    const int chosenIndex = labels.indexOf(choice);
    if (chosenIndex < 0) {
        return;
    }
    const Index& index = m_schema.indexes()[chosenIndex];

    if (m_confirmDestructiveActions) {
        const auto confirm =
            QMessageBox::question(this, "Drop Index", "Delete index \"" + index.name + "\"? This cannot be undone.");
        if (confirm != QMessageBox::Yes) {
            return;
        }
    }

    applySchemaChange("DROP INDEX " + index.name + ";", "Drop Index");
}

void MainWindow::onInsertRowTriggered()
{
    if (m_schema.tables().isEmpty()) {
        QMessageBox::information(this, "Insert Row", "Create a table first.");
        return;
    }

    InsertRowDialog dialog(m_schema, m_focusTable, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList columns;
    QStringList literals;
    for (const auto& entry : dialog.values()) {
        columns << entry.first;
        literals << (entry.second.isEmpty() ? "NULL" : sqlLiteral(entry.second));
    }

    const QString sql = "INSERT INTO " + dialog.tableName() + " (" + columns.join(", ") + ")\nVALUES ("
                       + literals.join(", ") + ");";

    if (!execSchemaChange(sql, "Insert Row")) {
        return;
    }

    m_statusLabel->setStyleSheet("color: #888780;");
    m_statusLabel->setText("Row inserted.");

    // Show the table's data right away so the new row is visible, and so
    // it lands in the results grid in its now-editable form.
    runQuery("SELECT * FROM " + dialog.tableName() + ";");
}

void MainWindow::onResultCellEdited(QTableWidgetItem* item)
{
    if (m_populatingResults || m_editableTable.isEmpty() || !item) {
        return;
    }

    const int row = item->row();
    const QTableWidgetItem* anchorItem = m_resultsTable->item(row, 0);
    const QVariantMap snapshot = anchorItem ? anchorItem->data(Qt::UserRole).toMap() : QVariantMap();
    const QTableWidgetItem* headerItem = m_resultsTable->horizontalHeaderItem(item->column());
    const QString columnName = headerItem ? headerItem->text() : QString();

    auto revertCell = [this, item](const QString& originalValue) {
        m_populatingResults = true;
        item->setText(originalValue);
        m_populatingResults = false;
    };

    if (columnName.isEmpty() || snapshot.isEmpty()) {
        return;
    }

    QStringList whereClauses;
    for (const auto& pk : m_editablePkColumns) {
        if (!snapshot.contains(pk)) {
            // Shouldn't happen for a plain "SELECT * FROM table" result,
            // but bail out rather than risk an UPDATE with a wrong WHERE.
            QMessageBox::warning(this, "Update Failed",
                                  "Can't identify this row (missing key column \"" + pk + "\").");
            revertCell(snapshot.value(columnName).toString());
            return;
        }
        whereClauses << pk + " = " + sqlLiteral(snapshot.value(pk).toString());
    }

    const QString newValue = item->text();
    const QString sql = "UPDATE " + m_editableTable + "\nSET " + columnName + " = "
                       + (newValue.isEmpty() ? "NULL" : sqlLiteral(newValue)) + "\nWHERE " + whereClauses.join(" AND ")
                       + ";";

    if (!execSchemaChange(sql, "Update Row")) {
        revertCell(snapshot.value(columnName).toString());
        return;
    }

    // Keep the cached snapshot in sync so later edits to the same row -
    // including editing a key column itself - still find it correctly.
    QVariantMap updatedSnapshot = snapshot;
    updatedSnapshot.insert(columnName, newValue);
    if (auto* mutableAnchor = m_resultsTable->item(row, 0)) {
        mutableAnchor->setData(Qt::UserRole, updatedSnapshot);
    }

    m_statusLabel->setStyleSheet("color: #888780;");
    m_statusLabel->setText("Saved.");
}

QString MainWindow::quoteForExport(const QVariant& value) const
{
    if (value.isNull()) {
        return "NULL";
    }

    // Numbers go out unquoted; everything else is treated as text and
    // quoted, with embedded single quotes doubled per SQL's escaping
    // rule - unlike the looser sqlLiteral() helper used for WHERE/INSERT
    // values typed directly by the user, correctness here matters more
    // since a round trip through export/import needs to reproduce the
    // data exactly.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = value.typeId();
#else
    const int typeId = value.type();
#endif
    switch (typeId) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
        return value.toString();
    default:
        break;
    }

    QString text = value.toString();
    text.replace("'", "''");
    return "'" + text + "'";
}

void MainWindow::onToggleMaximizeResults()
{
    m_resultsMaximized = !m_resultsMaximized;

    // Hiding the other panes hands all the space to the results panel.
    m_bodySplitter->setVisible(!m_resultsMaximized);
    m_leftPanel->setVisible(!m_resultsMaximized);

    m_maximizeResultsBtn->setText(m_resultsMaximized ? "Restore" : "Maximize");
    m_maximizeResultsBtn->setToolTip(m_resultsMaximized ? "Restore the normal layout (F11)"
                                                         : "Show results over the whole window (F11)");
}

void MainWindow::onExportResultsTriggered()
{
    const int columnCount = m_resultsTable->columnCount();
    const int rowCount = m_resultsTable->rowCount();
    if (columnCount == 0) {
        QMessageBox::information(this, "Export Results", "Run a query first - there are no results to export.");
        return;
    }

    QStringList headers;
    headers.reserve(columnCount);
    for (int c = 0; c < columnCount; ++c) {
        const auto* headerItem = m_resultsTable->horizontalHeaderItem(c);
        headers << (headerItem ? headerItem->text() : QString("column%1").arg(c + 1));
    }

    ResultRows rows;
    rows.reserve(rowCount);
    for (int r = 0; r < rowCount; ++r) {
        QStringList row;
        row.reserve(columnCount);
        for (int c = 0; c < columnCount; ++c) {
            const auto* item = m_resultsTable->item(r, c);
            row << (item ? item->text() : QString());
        }
        rows.push_back(row);
    }

#ifdef Q_OS_WASM
    bool fmtOk = false;
    const QString fmt = QInputDialog::getItem(this, "Export Results", "Format:",
                                              QStringList{"csv", "tsv", "json", "sql"}, 0, false, &fmtOk);
    if (!fmtOk) {
        return;
    }
    QDir().mkpath("/tmp");
    const QString path = "/tmp/results." + fmt;
#else
    const QString path = QFileDialog::getSaveFileName(
        this, "Export Results", m_defaultDialogDir,
        "CSV (*.csv);;Tab-separated values (*.tsv);;JSON (*.json);;SQL INSERT statements (*.sql);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    rememberDialogDir(path);
#endif

    // Pick the writer from whatever extension actually ended up in path
    // (typed by hand or added by the chosen filter); anything unrecognized
    // - including a bare "All files" pick with no suffix - falls back to
    // CSV, the most broadly-openable format.
    const QString suffix = QFileInfo(path).suffix().toLower();
    bool ok = false;
    if (suffix == "json") {
        ok = writeResultsAsJson(path, headers, rows);
    } else if (suffix == "tsv") {
        ok = writeResultsAsDelimited(path, headers, rows, '\t');
    } else if (suffix == "sql") {
        ok = writeResultsAsSql(path, headers, rows);
    } else {
        ok = writeResultsAsDelimited(path, headers, rows, ',');
    }

    if (!ok) {
        QMessageBox::critical(this, "Export Results", "Could not write to \"" + path + "\".");
        return;
    }
#ifdef Q_OS_WASM
    wasmDownload(path);
#endif

    m_statusLabel->setStyleSheet("color: #888780;");
    m_statusLabel->setText("Exported " + QString::number(rowCount) + " row(s) to " + QFileInfo(path).fileName() + ".");
}

bool MainWindow::writeResultsAsDelimited(const QString& path, const QStringList& headers, const ResultRows& rows,
                                          QChar delimiter)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#endif

    const QString sep(delimiter);
    QStringList headerFields;
    for (const auto& h : headers) {
        headerFields << delimitedFieldEscape(h, delimiter);
    }
    out << headerFields.join(sep) << "\n";

    for (const auto& row : rows) {
        QStringList fields;
        for (const auto& value : row) {
            fields << delimitedFieldEscape(value, delimiter);
        }
        out << fields.join(sep) << "\n";
    }
    return true;
}

bool MainWindow::writeResultsAsJson(const QString& path, const QStringList& headers, const ResultRows& rows)
{
    QJsonArray array;
    for (const auto& row : rows) {
        QJsonObject obj;
        for (int c = 0; c < headers.size() && c < row.size(); ++c) {
            obj.insert(headers[c], row[c]);
        }
        array.append(obj);
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    return true;
}

bool MainWindow::writeResultsAsSql(const QString& path, const QStringList& headers, const ResultRows& rows)
{
    // Reuses the currently-editable table's name when these results came
    // from a plain "SELECT * FROM table" (see runQuery()'s m_editableTable
    // logic) - the export is then a literal round trip. Otherwise the
    // query might join or alias several tables, so ask.
    QString tableName = m_editableTable;
    if (tableName.isEmpty()) {
        bool ok = false;
        tableName = QInputDialog::getText(this, "Export Results", "Table name for the INSERT statements:",
                                           QLineEdit::Normal, "results", &ok);
        if (!ok || tableName.trimmed().isEmpty()) {
            return false;
        }
        tableName = tableName.trimmed();
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#endif

    out << "-- Exported " << rows.size() << " row(s) from query results on "
        << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";

    for (const auto& row : rows) {
        QStringList values;
        for (int c = 0; c < headers.size() && c < row.size(); ++c) {
            values << sqlLiteralForResultsExport(row[c]);
        }
        out << "INSERT INTO " << tableName << " (" << headers.join(", ") << ") VALUES (" << values.join(", ")
            << ");\n";
    }
    return true;
}

void MainWindow::onExportDatabaseTriggered()
{
    if (!m_db.isOpen()) {
        QMessageBox::warning(this, "Export Database", "No database is open.");
        return;
    }
    if (m_schema.tables().isEmpty()) {
        QMessageBox::information(this, "Export Database", "This database has no tables to export.");
        return;
    }

    ExportDialog dialog(m_schema, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

#ifdef Q_OS_WASM
    QDir().mkpath("/tmp");
    const QString path = "/tmp/export.sql";
#else
    const QString path =
        QFileDialog::getSaveFileName(this, "Export Database", m_defaultDialogDir, "SQL script (*.sql);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    rememberDialogDir(path);
#endif

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Export Database", "Could not write to \"" + path + "\".");
        return;
    }

    QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#endif

    out << "-- Exported from " << QFileInfo(m_db.databaseName()).fileName() << " on "
        << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";

    int exportedRows = 0;
    for (const auto& tableName : dialog.selectedTables()) {
        out << "-- Table: " << tableName << "\n";

        if (dialog.includeStructure()) {
            // Pull the exact original DDL text out of sqlite_master
            // rather than reconstructing it from Column/Index - that
            // preserves anything our Schema model doesn't itself model
            // (CHECK constraints, DEFAULT values, table-level FKs, etc).
            QSqlQuery ddlQuery(m_db);
            ddlQuery.prepare("SELECT sql FROM sqlite_master WHERE type='table' AND name = ?;");
            ddlQuery.addBindValue(tableName);
            if (ddlQuery.exec() && ddlQuery.next()) {
                out << ddlQuery.value(0).toString() << ";\n";
            }

            QSqlQuery indexDdlQuery(m_db);
            indexDdlQuery.prepare(
                "SELECT sql FROM sqlite_master WHERE type='index' AND tbl_name = ? AND sql IS NOT NULL;");
            indexDdlQuery.addBindValue(tableName);
            if (indexDdlQuery.exec()) {
                while (indexDdlQuery.next()) {
                    out << indexDdlQuery.value(0).toString() << ";\n";
                }
            }
        }

        if (dialog.includeData()) {
            QSqlQuery dataQuery(m_db);
            if (dataQuery.exec("SELECT * FROM " + tableName + ";")) {
                const QSqlRecord record = dataQuery.record();
                const int columnCount = record.count();
                QStringList columnNames;
                for (int c = 0; c < columnCount; ++c) {
                    columnNames << record.fieldName(c);
                }

                while (dataQuery.next()) {
                    QStringList values;
                    for (int c = 0; c < columnCount; ++c) {
                        values << quoteForExport(dataQuery.value(c));
                    }
                    out << "INSERT INTO " << tableName << " (" << columnNames.join(", ") << ") VALUES ("
                        << values.join(", ") << ");\n";
                    ++exportedRows;
                }
            }
        }

        out << "\n";
    }

    file.close();
#ifdef Q_OS_WASM
    wasmDownload(path);
#endif

    m_statusLabel->setStyleSheet("color: #888780;");
    m_statusLabel->setText("Exported " + QString::number(dialog.selectedTables().size()) + " table(s), "
                            + QString::number(exportedRows) + " row(s) to " + QFileInfo(path).fileName() + ".");
}

void MainWindow::onImportDatabaseTriggered()
{
    if (!m_db.isOpen()) {
        QMessageBox::warning(this, "Import Database", "Open or create a database first.");
        return;
    }

#ifdef Q_OS_WASM
    QFileDialog::getOpenFileContent("SQL script (*.sql);;All files (*)",
        [this](const QString& name, const QByteArray& data) {
            if (name.isEmpty()) {
                return;
            }
            importSqlText(QString::fromUtf8(data));
        });
#else
    const QString path =
        QFileDialog::getOpenFileName(this, "Import SQL", m_defaultDialogDir, "SQL script (*.sql);;All files (*)");
    if (path.isEmpty()) {
        return;
    }
    rememberDialogDir(path);

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Import Database", "Could not read \"" + path + "\".");
        return;
    }

    QTextStream in(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    in.setCodec("UTF-8");
#endif
    const QString contents = in.readAll();
    file.close();
    importSqlText(contents);
#endif
}

void MainWindow::importSqlText(const QString& contents)
{
    const QStringList statements = splitSqlStatements(contents);
    if (statements.isEmpty()) {
        QMessageBox::information(this, "Import Database", "That file has no SQL statements to run.");
        return;
    }

    // Best-effort import: run every statement, skip and record any that
    // fail (e.g. "table already exists" when re-importing structure into
    // a database that already has it), and reload once at the end rather
    // than after every single statement.
    int succeeded = 0;
    QStringList failures;
    for (const auto& statement : statements) {
        QSqlQuery query(m_db);
        if (query.exec(statement)) {
            ++succeeded;
        } else {
            QString preview = statement.left(60);
            if (statement.size() > 60) {
                preview += "…";
            }
            failures << preview + "\n    → " + query.lastError().text();
        }
    }

    reloadSchemaAndFocus();

    const QString summary = QString("%1 of %2 statements ran successfully.").arg(succeeded).arg(statements.size());
    if (failures.isEmpty()) {
        m_statusLabel->setStyleSheet("color: #888780;");
        m_statusLabel->setText(summary);
        return;
    }

    // Keep the dialog readable: show at most the first several failures,
    // plus a count of anything beyond that.
    const int shown = qMin(failures.size(), 8);
    QString detail = failures.mid(0, shown).join("\n\n");
    if (failures.size() > shown) {
        detail += QString("\n\n…and %1 more.").arg(failures.size() - shown);
    }
    QMessageBox::warning(this, "Import finished with errors", summary + "\n\n" + detail);
}