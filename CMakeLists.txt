cmake_minimum_required(VERSION 3.20)

project(pearch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(pearch
    src/main.cpp
    src/cli/cli.cpp
    src/ui/terminal.cpp
    src/ui/dashboard.cpp
    src/filesystem/scanner.cpp
    src/index/database.cpp
    src/index/index.cpp
)

enable_testing()

add_subdirectory(tests)

target_include_directories(pearch PRIVATE src)
# Prefer SQLite from Miniconda if available
if(NOT DEFINED ENV{CONDA_PREFIX})
    set(_conda_prefix "C:/Users/Hotmoves/miniconda3")
else()
    set(_conda_prefix $ENV{CONDA_PREFIX})
endif()
if(EXISTS "${_conda_prefix}/Library/include/sqlite3.h" AND EXISTS "${_conda_prefix}/Library/lib/sqlite3.lib")
    message(STATUS "Using SQLite from Miniconda: ${_conda_prefix}")
    set(SQLite3_INCLUDE_DIR "${_conda_prefix}/Library/include" CACHE PATH "SQLite include dir" FORCE)
    set(SQLite3_LIBRARY "${_conda_prefix}/Library/lib/sqlite3.lib" CACHE FILEPATH "SQLite lib" FORCE)
endif()

# Find SQLite3 (optional; we'll fallback to fetching if not found)
find_package(SQLite3)
if (TARGET SQLite::SQLite3)
    target_link_libraries(pearch PRIVATE SQLite::SQLite3)
elseif (TARGET SQLite3::SQLite3)
    target_link_libraries(pearch PRIVATE SQLite3::SQLite3)
else()
    target_link_libraries(pearch PRIVATE sqlite3)
endif()

if (NOT SQLite3_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        sqlite_src
        GIT_REPOSITORY https://github.com/sqlite/sqlite.git
    )
    FetchContent_MakeAvailable(sqlite_src)
    add_library(sqlite3_static STATIC ${sqlite_src_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3_static PUBLIC ${sqlite_src_SOURCE_DIR})
    target_compile_definitions(sqlite3_static PRIVATE SQLITE_THREADSAFE=1)
    target_link_libraries(pearch PRIVATE sqlite3_static)
endif()

target_link_libraries(pearch PRIVATE Shell32 Ole32)
