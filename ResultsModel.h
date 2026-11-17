#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QStringList>

#include <functional>
#include <type_traits>

// Table model behind the RESULTS grid. It just points at the rows the query
// returned instead of copying every cell into a widget item, so showing a
// result is instant whatever its size (a QTableWidget needs one heap object
// per cell, which for a few hundred thousand rows takes seconds and a lot of
// memory, especially in the browser). The view only asks for the cells that
// are on screen.
//
// Header-only on purpose: nothing to add to CMakeLists.txt.
class ResultsModel : public QAbstractTableModel
{
public:
    // Called when the person edits a cell. Return true once the database has
    // been updated; false makes the view keep the old value.
    using EditHandler = std::function<bool(int row, int column, const QString& newValue)>;

    explicit ResultsModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    void setEditHandler(EditHandler handler) { m_editHandler = std::move(handler); }

    // rows is shared, not copied, when it already is a QList<QStringList>.
    template <class Rows>
    void setResult(const QStringList& headers, const Rows& rows, bool editable)
    {
        beginResetModel();
        m_headers = headers;
        if constexpr (std::is_same_v<Rows, QList<QStringList>>) {
            m_rows = rows;
        } else {
            m_rows.clear();
            m_rows.reserve(rows.size());
            for (const auto& row : rows) {
                m_rows.push_back(QStringList(row.begin(), row.end()));
            }
        }
        m_editable = editable;
        endResetModel();
    }

    void clear()
    {
        beginResetModel();
        m_headers.clear();
        m_rows.clear();
        m_editable = false;
        endResetModel();
    }

    const QStringList& headers() const { return m_headers; }
    const QList<QStringList>& rows() const { return m_rows; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int columnCount(const QModelIndex& parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_headers.size());
    }

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            return QVariant();
        }
        // DisplayRole/EditRole show the value; the tooltip carries the full
        // text for cells cut off by the column width cap.
        if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::ToolTipRole) {
            const QStringList& row = m_rows[index.row()];
            return index.column() < row.size() ? row[index.column()] : QString();
        }
        return QVariant();
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override
    {
        if (role == Qt::DisplayRole && orientation == Qt::Horizontal && section >= 0 && section < m_headers.size()) {
            return m_headers[section];
        }
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        if (m_editable) {
            f |= Qt::ItemIsEditable;
        } else {
            f &= ~Qt::ItemIsEditable;
        }
        return f;
    }

    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override
    {
        if (role != Qt::EditRole || !m_editable || !m_editHandler || !index.isValid()
            || index.row() >= m_rows.size() || index.column() >= m_headers.size()) {
            return false;
        }
        const QString newValue = value.toString();
        if (!m_editHandler(index.row(), index.column(), newValue)) {
            return false;
        }
        QStringList row = m_rows[index.row()];
        while (row.size() <= index.column()) {
            row << QString();
        }
        row[index.column()] = newValue;
        m_rows[index.row()] = row;
        emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole, Qt::ToolTipRole});
        return true;
    }

private:
    QStringList m_headers;
    QList<QStringList> m_rows;
    bool m_editable = false;
    EditHandler m_editHandler;
};
