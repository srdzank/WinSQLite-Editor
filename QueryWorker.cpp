#include "QueryWorker.h"

namespace {
// Register the cross-thread signal payload type once, the first time a
// worker is constructed. Needed because QVector<QStringList> isn't one
// of Qt's built-in meta types, and queued connections (which is what a
// cross-thread signal/slot always uses) require every argument type to
// be registered with the meta-object system.
void registerResultRowsMetaType()
{
    static const int registered = qRegisterMetaType<ResultRows>("ResultRows");
    Q_UNUSED(registered);
}
} // namespace

QueryWorker::QueryWorker(QString dbPath, QObject* parent)
    : QObject(parent)
    , m_dbPath(std::move(dbPath))
{
    registerResultRowsMetaType();
}

QueryWorker::~QueryWorker()
{
    m_db.close();
}

bool QueryWorker::ensureConnected()
{
    if (m_db.isOpen()) {
        return true;
    }

    if (!m_db.open(m_dbPath, /*readOnly=*/false, /*create=*/false)) {
        emit failed(m_db.lastError());
        return false;
    }
    return true;
}

void QueryWorker::execute(const QString& sql)
{
    if (!ensureConnected()) {
        return;
    }

    QStringList headers;
    ResultRows rows;
    if (!m_db.runSql(sql, headers, rows)) {
        emit failed(m_db.lastError());
        return;
    }

    emit succeeded(headers, rows);
}
