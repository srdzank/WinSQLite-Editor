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
EM_ASYNC_JS(void, mountPersistentStorage, (), {
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