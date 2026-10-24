#pragma once
#include "ScatterView3D.h"
#include "VecDatabase.h"

#include <QDialog>

class QLabel;
class QSpinBox;

namespace vec {

// Loads a vec0 table, projects it to 3D (PCA), shows it in a ScatterView3D.
// Clicking a point selects it, runs a KNN query and highlights the neighbours.
class VectorExplorerDialog : public QDialog {
    Q_OBJECT
public:
    VectorExplorerDialog(const QString& dbPath, QString vecTable,
                         ScatterView3D::LabelFn labelFn, QWidget* parent = nullptr);

signals:
    // Emitted on every click: the vec0 rowid equals the source table's rowid/id.
    void recordActivated(qint64 rowid);

private:
    bool loadData(const QString& dbPath);
    void onPointClicked(qint64 rowid);

    Database       m_db;
    QString        m_table;
    ScatterView3D* m_view   = nullptr;
    QLabel*        m_status = nullptr;
    QSpinBox*      m_k      = nullptr;
};

} // namespace vec
