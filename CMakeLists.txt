cmake_minimum_required(VERSION 3.16)

project(sqlite_visual_editor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Sql Widgets)

# ---- Static SQLite + sqlite-vec ----
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

if(EMSCRIPTEN)
    # Browser: no threads, no WAL, temp data in RAM
    target_compile_definitions(sqlite_core PRIVATE
        SQLITE_THREADSAFE=0
        SQLITE_TEMP_STORE=3
        SQLITE_OMIT_WAL=1
        SQLITE_DEFAULT_MEMSTATUS=0
        # Qt's qsqlite driver calls sqlite3_column_table_name16() etc. Without
        # this define our SQLite lacks them, so the linker also pulls Qt's own
        # SQLite object and the two copies corrupt each other (memory OOB).
        SQLITE_ENABLE_COLUMN_METADATA=1
    )
else()
    target_compile_definitions(sqlite_core PRIVATE SQLITE_THREADSAFE=1)
endif()

# On Windows, embed resources/app_icon.ico directly into the .exe
if(WIN32)
    set(APP_ICON_RESOURCE_WINDOWS "${CMAKE_CURRENT_SOURCE_DIR}/resources/winsqlite.rc")
endif()

qt_add_executable(sqlite_visual_editor WIN32
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

set_target_properties(sqlite_visual_editor PROPERTIES WIN32_EXECUTABLE TRUE)

target_include_directories(sqlite_visual_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(sqlite_visual_editor PRIVATE
    sqlite_core
    Qt6::Core Qt6::Sql Qt6::Widgets
)

# ---- WebAssembly (Emscripten) ----
if(EMSCRIPTEN)
    target_link_options(sqlite_visual_editor PRIVATE
        -sNO_DISABLE_EXCEPTION_CATCHING
        -Wl,--allow-multiple-definition
        --bind
        -sASYNCIFY
        -sASYNCIFY_STACK_SIZE=65536
        -sFORCE_FILESYSTEM=1
        -lidbfs.js
        "-sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap"
        -sALLOW_MEMORY_GROWTH=1
    )

    if(TARGET Qt6::qsqlite)
        set_target_properties(Qt6::qsqlite PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()