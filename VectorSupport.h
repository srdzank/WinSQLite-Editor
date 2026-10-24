#pragma once
#include <QString>
#include <QtGlobal>
#include <functional>

class QMainWindow;

namespace vec {

// Adds "Tools > Vector space..." to the window's menu bar (reuses a menu titled
// "Tools" if one exists). MainWindow needs only one call to this function.
//
//   dbPath   : returns the path of the currently opened database file ("" if none)
//   label    : optional tooltip text for a point (called on the GUI thread)
//   onRecord : optional, called when the user clicks a point (rowid of the record)
void installVectorExplorer(QMainWindow* window,
                           std::function<QString()> dbPath,
                           std::function<QString(qint64 rowid)> label = {},
                           std::function<void(qint64 rowid)> onRecord = {});

} // namespace vec
