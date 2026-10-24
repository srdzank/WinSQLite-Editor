#include "VectorSupport.h"

#include "VecDatabase.h"
#include "VectorExplorerDialog.h"

#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <random>

namespace {

QMenu* toolsMenu(QMainWindow* w)
{
    for (QAction* a : w->menuBar()->actions())
        if (a->menu() && a->text().remove(QLatin1Char('&')).compare(QStringLiteral("Tools"), Qt::CaseInsensitive) == 0)
            return a->menu();
    return w->menuBar()->addMenu(QObject::tr("&Tools"));
}

// Creates a standalone SQLite file with a vec0 table: 4 clusters x 80 points, 16 dimensions.
bool createDemoDatabase(const QString& path, QString& error)
{
    QFile::remove(path);                       // the save dialog already asked about overwriting
    vec::Database db;
    if (!db.open(path, /*readOnly=*/false, /*create=*/true)) { error = db.lastError(); return false; }

    constexpr int kDims = 16, kClusters = 4, kPerCluster = 80;
    if (!db.createVecTable(QStringLiteral("vec_items"), kDims, vec::Metric::Cosine)) { error = db.lastError(); return false; }

    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.f, 0.15f);
    std::vector<vec::Embedding> items;
    items.reserve(kClusters * kPerCluster);
    for (int c = 0; c < kClusters; ++c)
        for (int i = 0; i < kPerCluster; ++i) {
            vec::Embedding e;
            e.rowid = c * kPerCluster + i + 1;
            e.values.assign(kDims, 0.f);
            e.values[std::size_t(c * 3)] = 1.f;
            for (float& x : e.values) x += noise(rng);
            items.push_back(std::move(e));
        }
    if (!db.insertMany(QStringLiteral("vec_items"), items)) { error = db.lastError(); return false; }
    return true;
}

} // namespace

namespace vec {

void installVectorExplorer(QMainWindow* window,
                           std::function<QString()> dbPath,
                           std::function<QString(qint64)> label,
                           std::function<void(qint64)> onRecord)
{
    QMenu* menu = toolsMenu(window);

    // 1) Open a vector database (a separate file that contains a vec0 table).
    QAction* open = menu->addAction(QObject::tr("Vector space (3D)..."));
    QObject::connect(open, &QAction::triggered, window,
                     [window, dbPath, label = std::move(label), onRecord = std::move(onRecord)] {
        const QString startDir = (dbPath && !dbPath().isEmpty()) ? QFileInfo(dbPath()).absolutePath() : QString();
        const QString file = QFileDialog::getOpenFileName(
            window, QObject::tr("Open vector database"), startDir,
            QObject::tr("SQLite (*.db *.sqlite *.sqlite3 *.vec);;All files (*)"));
        if (file.isEmpty()) return;

        bool ok = false;
        const QString table = QInputDialog::getText(
            window, QObject::tr("Vector space"), QObject::tr("Name of the vec0 table:"),
            QLineEdit::Normal, QStringLiteral("vec_items"), &ok);
        if (!ok || table.isEmpty()) return;
        if (!Database::isValidIdentifier(table)) {
            QMessageBox::warning(window, QObject::tr("Vector space"), QObject::tr("Invalid table name."));
            return;
        }

        auto* dlg = new VectorExplorerDialog(file, table, label, window);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        if (onRecord)
            QObject::connect(dlg, &VectorExplorerDialog::recordActivated, window, onRecord);
        dlg->show();
    });

    // 2) Generate a demo vector database to try the feature.
    QAction* demo = menu->addAction(QObject::tr("Create demo vector database..."));
    QObject::connect(demo, &QAction::triggered, window, [window, dbPath] {
        const QString startDir = (dbPath && !dbPath().isEmpty()) ? QFileInfo(dbPath()).absolutePath() : QString();
        const QString file = QFileDialog::getSaveFileName(
            window, QObject::tr("Create demo vector database"),
            startDir.isEmpty() ? QStringLiteral("demo_vectors.db") : startDir + QStringLiteral("/demo_vectors.db"),
            QObject::tr("SQLite (*.db)"));
        if (file.isEmpty()) return;

        QString error;
        if (!createDemoDatabase(file, error)) {
            QMessageBox::critical(window, QObject::tr("Vector space"), QObject::tr("Could not create demo: %1").arg(error));
            return;
        }
        QMessageBox::information(window, QObject::tr("Vector space"),
            QObject::tr("Created %1\nTable: vec_items (320 vectors, 16 dimensions).\n"
                        "Open it with Tools > Vector space (3D)...").arg(file));
    });
}

} // namespace vec
