#pragma once
// Edits ONE vector element by element: a table with one row per dimension
// (index | value). The number of dimensions is fixed - it is defined by the
// vec0 column (float[N]) - so only the values change.
//
// Convenience for long vectors (hundreds of dimensions): filter by index/value,
// "go to index", paste a whole list of numbers, set a range of elements at
// once, reset to the stored values. Changed elements are highlighted.

#include <QDialog>
#include <QString>
#include <QtGlobal>
#include <vector>

class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QTableWidgetItem;

namespace vec {

class VectorEditDialog : public QDialog {
    Q_OBJECT
public:
    VectorEditDialog(const QString& title, std::vector<float> values, QWidget* parent = nullptr);

    // The edited vector (valid after exec() == Accepted).
    std::vector<float> values() const;

    // Whole flow used by both the results grid and the 3D view: opens its own
    // read-write sqlite-vec connection to dbPath, loads the embedding of
    // `rowid` from vec0 table `table`, shows this dialog and, on OK, saves.
    // Returns true only if the vector was actually changed in the database.
    static bool editRecord(QWidget* parent, const QString& dbPath, const QString& table, qint64 rowid);

private slots:
    void applyFilter(const QString& text);
    void goToIndex();
    void pasteList();
    void setRange();
    void resetAll();
    void onItemChanged(QTableWidgetItem* item);

private:
    void accept() override;
    bool parseRow(int row, float& out) const;
    void markRow(int row);
    void updateInfo();

    std::vector<float> m_original;
    QTableWidget* m_table = nullptr;
    QLineEdit*    m_filter = nullptr;
    QSpinBox*     m_gotoSpin = nullptr;
    QLabel*       m_info = nullptr;
    bool          m_populating = false;
};

} // namespace vec
