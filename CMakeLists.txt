cmake_minimum_required(VERSION 3.16)

project(sqlite_visual_editor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Sql Widgets)

# ---- Static SQLite + sqlite-vec (own copy, independent of Qt's QSQLITE plugin) ----
# Place these files (from the official AMALGAMATION zips) here:
#   third_party/sqlite/sqlite3.c, sqlite3.h
#   third_party/sqlite-vec/sqlite-vec.c, sqlite-vec.h
add_library(sqlite_core STATIC
    third_party/sqlite/sqlite3.c
    third_party/sqlite-vec/sqlite-vec.c
)
target_include_directories(sqlite_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-vec
)
set_source_files_properties(third_party/sqlite-vec/sqlite-vec.c PROPERTIES
    COMPILE_DEFINITIONS "SQLITE_CORE;SQLITE_VEC_STATIC"
)
target_compile_definitions(sqlite_core PRIVATE SQLITE_THREADSAFE=1)

# On Windows, embed resources/app_icon.ico directly into the .exe
if(WIN32)
    set(APP_ICON_RESOURCE_WINDOWS "${CMAKE_CURRENT_SOURCE_DIR}/resources/winsqlite.rc")
endif()

add_executable(sqlite_visual_editor WIN32
    main.cpp
    Schema.cpp
    QueryModel.cpp
    TableNode.cpp
    DiagramView.cpp
    MainWindow.cpp
    SqlEditor.cpp
    QueryWorker.cpp
    CreateTableDialog.cpp
    CreateIndexDialog.cpp
    InsertRowDialog.cpp
    ExportDialog.cpp
    SettingsDialog.cpp
    VecDatabase.cpp
    Projection.cpp
    ScatterView3D.cpp
    VectorExplorerDialog.cpp
    VectorSupport.cpp
    app.qrc
    ${APP_ICON_RESOURCE_WINDOWS}
)

set_target_properties(sqlite_visual_editor PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_include_directories(sqlite_visual_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(sqlite_visual_editor PRIVATE
    sqlite_core
    Qt6::Core Qt6::Sql Qt6::Widgets
)
