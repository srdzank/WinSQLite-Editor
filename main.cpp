#include <QApplication>
#include <QIcon>
#include <QSettings>

#ifdef Q_OS_WASM
#include <emscripten.h>
#include <emscripten/em_js.h>

// Browser: the virtual filesystem is in-memory and is wiped on every page
// refresh. Mount an IndexedDB-backed folder and load whatever was saved
// there last time, so QSettings (preferences + query history) survives.
// Needs -sASYNCIFY (already set in CMakeLists.txt) to wait for the load.
//
// Before that load, any database that an earlier build stored under
// /persist/databases is deleted from IndexedDB. FS.syncfs(true) reads EVERY
// stored file into memory, so a leftover 300 MB database would otherwise be
// loaded again on each page refresh and keep the tab's memory high.
// (IDBFS names its IndexedDB database after the mount point, '/persist', and
// keeps one record per file/folder in the 'FILE_DATA' store, keyed by path.)
EM_ASYNC_JS(void, mountPersistentStorage, (), {
    await new Promise((resolve) => {
        let request;
        try { request = indexedDB.open('/persist'); } catch (e) { resolve(); return; }
        // Not created yet: nothing to clean. Abort so we don't create it half-way.
        request.onupgradeneeded = () => { try { request.transaction.abort(); } catch (e) {} };
        request.onerror = () => resolve();
        request.onblocked = () => resolve();
        request.onsuccess = () => {
            const db = request.result;
            try {
                if (!db.objectStoreNames.contains('FILE_DATA')) { db.close(); resolve(); return; }
                const tx = db.transaction('FILE_DATA', 'readwrite');
                const prefix = '/persist/databases';
                tx.objectStore('FILE_DATA').delete(IDBKeyRange.bound(prefix, prefix + String.fromCharCode(0xffff)));
                tx.oncomplete = () => { db.close(); resolve(); };
                tx.onerror = () => { db.close(); resolve(); };
                tx.onabort = () => { db.close(); resolve(); };
            } catch (e) {
                try { db.close(); } catch (e2) {}
                resolve();
            }
        };
    });

    try { FS.mkdir('/persist'); } catch (e) {}
    try { FS.mount(IDBFS, {}, '/persist'); } catch (e) {}
    await new Promise((resolve) => FS.syncfs(true, (err) => resolve()));
});
#endif

#include "MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

#ifdef Q_OS_WASM
    mountPersistentStorage();
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, "/persist");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, "/persist");
#endif

    // QSettings (used to persist the Settings dialog's preferences)
    // needs an organization/application name to know where to store
    // its data (registry key on Windows, .conf file on Linux/macOS).
    QApplication::setOrganizationName("WinSQLite");
    QApplication::setApplicationName("WinSQLite");

    // Sets the icon used for the title bar / taskbar / alt-tab switcher.
    // Comes from app.qrc, which embeds resources/app_icon.png - on
    // Windows, resources/winsqlite.rc separately embeds an .ico into the
    // .exe itself, so Explorer shows the right icon before the app even
    // runs.
    app.setWindowIcon(QIcon(":/resources/app_icon.png"));

    MainWindow window;

    // Optional convenience: still allow launching with a database path
    // as an argument (e.g. a file association or a shell script), but
    // it's no longer required - Database > Open/New in the menu bar
    // covers the interactive case.
    if (argc > 1) {
        window.openDatabase(QString::fromLocal8Bit(argv[1]));
    }

    window.show();
    return app.exec();
}
