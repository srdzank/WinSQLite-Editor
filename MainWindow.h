#pragma once

#include <QAction>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSqlDatabase>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThread>
#include <QTreeWidget>
#include <QVariant>
#include <QVBoxLayout>
#include <QVector>

#include "DiagramView.h"
#include "QueryModel.h"
#include "QueryWorker.h"
#include "Schema.h"
#include "SqlEditor.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Loads a database file, populates the table tree, and focuses the
    // first table that has at least one relation (falls back to the
    // first table if none do).
    bool openDatabase(const QString& path);

signals:
    // Crosses onto the query thread via a queued connection to kick off
    // execute() there - see startQueryThread().
    void requestQuery(const QString& sql);

private slots:
    void onTableActivated(const QString& tableName);
    void onTreeItemClicked(QTreeWidgetItem* item, int column);

    // A dashed "+ table" node in the diagram was clicked: extend the
    // current query with a JOIN to it (never rebuilds the query).
    void onJoinCandidateClicked(const QString& tableName);

    // Query builder step management.
    void onAddStepClicked();
    void onRemoveJoinClicked(int index);
    void onRemoveConditionClicked(int index);

    // SQL mode / execution.
    void onSqlModeToggled(bool checked);
    void onExecuteClicked();
    void onSqlTextChanged();

    // Delivered from QueryWorker, running on m_queryThread.
    void onQuerySucceeded(QStringList headers, ResultRows rows);
    void onQueryFailed(QString message);

    // Database menu.
    void onNewDatabaseTriggered();
    void onOpenDatabaseTriggered();
    void onExportDatabaseTriggered();
    void onImportDatabaseTriggered();

    // Settings menu.
    void onSettingsTriggered();

    // Schema menu - each of these edits the live database directly (DDL
    // runs synchronously on the GUI thread's m_db, since these are quick
    // one-off statements, not the potentially-slow queries the worker
    // thread is for) and then reloads the schema.
    void onNewTableTriggered();
    void onNewIndexTriggered();
    void onDropTableTriggered();
    void onDropIndexTriggered();

    // Data menu / results grid editing.
    void onInsertRowTriggered();
    void onResultCellEdited(QTableWidgetItem* item);
    void onExportResultsTriggered();

private:
    void buildUi();
    void buildMenus();
    void applyDarkTheme();
    void refreshQueryPanel();
    void populateTableTree();

    // Preferences (Settings dialog), persisted via QSettings.
    // loadSettings() runs before buildUi()/applyDarkTheme() so the very
    // first paint already reflects saved values.
    void loadSettings();
    void saveSettings();
    // Updates m_defaultDialogDir to filePath's containing folder (and
    // persists it) if it changed - called after every file dialog that
    // returns a real path, so later dialogs default to wherever the
    // person last saved/opened something.
    void rememberDialogDir(const QString& filePath);

    // Query builder "+ Add step" flows.
    void addJoinStep();
    void addConditionStep();

    // Execution / results.
    void startQueryThread(const QString& dbPath);
    void stopQueryThread();
    void runQuery(const QString& sql);
    void showResults(const QStringList& headers, const ResultRows& rows);
    void showError(const QString& message);
    void showRunning();

    // Schema editing (New/Drop Table/Index). Runs sql directly against
    // m_db and reports failures via a message box; returns whether it
    // succeeded so callers can decide whether to reload the schema.
    bool execSchemaChange(const QString& sql, const QString& context);
    // Convenience wrapper: execSchemaChange() + reload schema + refresh
    // every schema-dependent part of the UI (tree, autocomplete, diagram).
    void applySchemaChange(const QString& sql, const QString& context);
    // The reload+refresh half of applySchemaChange(), factored out so
    // onImportDatabaseTriggered() (which runs many statements at once,
    // not a single one) can reuse it too.
    void reloadSchemaAndFocus();
    void setSchemaActionsEnabled(bool enabled);

    // Refreshes everything that depends on m_schema but doesn't depend on
    // which table is focused: the table tree and raw-SQL autocomplete.
    void syncSchemaUi();
    // The table a fresh view of the schema should default to: the first
    // table with at least one relation, else just the first table, else
    // empty if the schema has no tables at all.
    QString defaultFocusTable() const;
    // Starts a NEW query: FROM = tableName, nothing else. Joins are then
    // added one at a time (diagram click or "+ Add step"). Only called for
    // explicit "start from this table" actions - clicking in the diagram
    // never goes through here, so it can't wipe a query in progress.
    void startQueryFrom(const QString& tableName);

    // Redraws the diagram from whatever the query currently is: the
    // builder model, or - in SQL mode - the tables found in the typed SQL.
    // Everything that changes the query ends up here, so the diagram can't
    // show something the query doesn't contain.
    void refreshDiagram();

    // Tables in the query right now (FROM first), in whichever mode is active.
    QVector<QString> currentQueryTables() const;
    // Tables found in the raw SQL text: the one after FROM, then each JOIN.
    QVector<QString> sqlQueryTables() const;
    // Adds candidate to the query: a model change in builder mode, a text
    // insertion (before WHERE/GROUP BY/...) in SQL mode - either way the
    // existing query is extended, not replaced.
    void addJoinToQuery(const JoinCandidate& candidate);
    void insertJoinIntoSql(const JoinCandidate& candidate);
    void resetUiForNewDatabase();

    // Case-insensitively resolves a table name as typed in raw SQL to its
    // canonical name in the schema, or an empty string if it doesn't exist.
    QString resolveTableName(const QString& rawName) const;

    // Renders a single cell value for an exported INSERT statement:
    // NULL, an unquoted number, or a quoted/escaped string.
    QString quoteForExport(const QVariant& value) const;

    // Writers behind "Data > Export Results…" / the results panel's
    // Export button - each writes the full grid (headers + rows, as
    // currently displayed) to path in one format. Return false (and
    // leave any partial file behind) only if the file couldn't be
    // opened for writing.
    bool writeResultsAsDelimited(const QString& path, const QStringList& headers, const ResultRows& rows,
                                  QChar delimiter);
    bool writeResultsAsJson(const QString& path, const QStringList& headers, const ResultRows& rows);
    // May prompt for a table name (see m_editableTable) - not const.
    bool writeResultsAsSql(const QString& path, const QStringList& headers, const ResultRows& rows);

    Schema m_schema;
    QueryModel m_queryModel;
    QString m_focusTable;
    QSqlDatabase m_db;

    // Preferences edited via the Settings dialog, loaded from/saved to
    // QSettings. m_uiFontSize/m_editorFontSize feed applyDarkTheme()'s
    // stylesheet; m_confirmDestructiveActions gates the Drop Table/Index
    // confirmation prompts; m_defaultDialogDir seeds every Open/New/
    // Export/Import file dialog and is kept up to date automatically.
    int m_uiFontSize = 12;
    int m_editorFontSize = 11;
    bool m_confirmDestructiveActions = true;
    QString m_defaultDialogDir;

    // Query execution happens on this dedicated thread/worker so a slow
    // query never blocks the UI. The worker has its own SQLite
    // connection, independent of m_db above (which stays on the GUI
    // thread for schema loading and autocomplete).
    QThread* m_queryThread = nullptr;
    QueryWorker* m_queryWorker = nullptr;
    bool m_queryRunning = false;

    // True while the right-hand panel shows the raw, user-editable SQL
    // text box instead of the step-based builder view.
    bool m_sqlModeActive = false;

    QTreeWidget* m_tableTree = nullptr;
    DiagramView* m_diagramView = nullptr;

    // Top bar title, showing the currently open database's file name.
    QLabel* m_titleLabel = nullptr;

    // Actions that only make sense with a database open: New/Drop
    // Table/Index, Insert Row, Export, Import. All disabled until then.
    QVector<QAction*> m_schemaActions;

    // Right panel: builder page (steps + read-only preview) vs raw SQL page.
    QStackedWidget* m_rightStack = nullptr;
    QVBoxLayout* m_stepsLayout = nullptr;
    QPlainTextEdit* m_sqlPreview = nullptr;  // read-only, mirrors the builder
    SqlEditor* m_sqlEditor = nullptr;        // editable, used in SQL mode, with autocomplete

    QPushButton* m_sqlModeBtn = nullptr;
    QPushButton* m_executeBtn = nullptr;
    QPushButton* m_addStepBtn = nullptr;

    // Bottom panel: query results.
    QTableWidget* m_resultsTable = nullptr;
    QLabel* m_statusLabel = nullptr;
    // Exports the currently-displayed grid to CSV/TSV/JSON/SQL. Enabled
    // only while the grid actually has columns (see showResults()).
    QPushButton* m_exportResultsBtn = nullptr;

    // Direct in-place editing of the results grid is only enabled when
    // the last successful query was exactly "SELECT * FROM table" (no
    // WHERE/JOIN) for a table that has a primary key - only then is it
    // safe to map an edited cell back to a specific database row.
    // Empty m_editableTable means the grid is read-only.
    QString m_editableTable;
    QStringList m_editablePkColumns;
    // True while showResults() is populating cells, so those
    // programmatic changes don't get mistaken for user edits.
    bool m_populatingResults = false;
};
