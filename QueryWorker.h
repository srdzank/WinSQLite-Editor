#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "VecDatabase.h"

// One row of query results, already converted to display strings.
using ResultRow = QStringList;
using ResultRows = QVector<ResultRow>;

// Executes SQL on a background thread so a slow or accidentally
// expensive query (large JOIN, missing index, etc.) never freezes the
// UI. Lives in its own QThread (see MainWindow), and owns its own
// SQLite connection - never the one MainWindow uses on the GUI thread.
//
// The connection is a vec::Database (the project's own static SQLite with
// sqlite-vec registered), NOT Qt's QSQLITE plugin, so vec0 virtual tables
// and the vec_* functions work in user queries.
//
// The connection is opened lazily, on first execute(), because it must
// be created on the thread that will use it - not in the constructor,
// which still runs on the GUI thread before moveToThread() takes effect.
class QueryWorker : public QObject
{
    Q_OBJECT

public:
    explicit QueryWorker(QString dbPath, QObject* parent = nullptr);
    ~QueryWorker() override;

public slots:
    void execute(const QString& sql);

signals:
    // Headers + rows are plain value types so they can safely cross the
    // thread boundary via a queued connection.
    void succeeded(QStringList headers, ResultRows rows);
    void failed(QString message);

private:
    bool ensureConnected();

    QString m_dbPath;
    vec::Database m_db;
};
