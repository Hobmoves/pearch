cmake_minimum_required(VERSION 3.20)

project(pearch
    VERSION 1.0.0
    DESCRIPTION "Instant file search for your terminal"
    LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PEARCH_BUILD_TESTS "Build the test suite" ON)
option(PEARCH_BUILD_BENCH "Build the search benchmark" ON)
option(PEARCH_SANITIZE "Build with AddressSanitizer + UBSan (GCC/Clang)" OFF)

# Ship a self-contained exe: no VC++ redistributable needed on the target PC.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT PEARCH_HAS_IPO OUTPUT _ipo_output LANGUAGES CXX)

find_package(Threads REQUIRED)

configure_file(src/core/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.h @ONLY)

function(pearch_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc
            $<$<CONFIG:Release>:/O2 /Oi /GS->)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
            $<$<CONFIG:Release>:-O3>)
        if(PEARCH_SANITIZE)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
    if(PEARCH_HAS_IPO AND NOT PEARCH_SANITIZE)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# pearch_core: everything except main(), shared by the app, tests and bench
# ---------------------------------------------------------------------------
add_library(pearch_core STATIC
    src/core/text.cpp
    src/core/format.cpp
    src/core/fileio.cpp
    src/core/hardware.cpp
    src/engine/index_store.cpp
    src/engine/query.cpp
    src/engine/matcher.cpp
    src/engine/searcher.cpp
    src/engine/history.cpp
    src/fs/scanner.cpp
    src/tui/screen.cpp
    src/tui/widgets.cpp
    src/tui/pear3d.cpp
    src/tui/index_view.cpp
    src/tui/search_view.cpp
    src/app/cli.cpp
    src/app/config.cpp
    src/app/commands.cpp
)

if(WIN32)
    target_sources(pearch_core PRIVATE
        src/fs/scanner_win.cpp
        src/platform/console_win.cpp
        src/platform/shell_win.cpp)
    target_compile_definitions(pearch_core PUBLIC
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(pearch_core PUBLIC shell32 ole32 uuid advapi32 user32)
else()
    target_sources(pearch_core PRIVATE
        src/fs/scanner_posix.cpp
        src/platform/console_posix.cpp
        src/platform/shell_posix.cpp)
endif()

target_include_directories(pearch_core PUBLIC
    src
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(pearch_core PUBLIC Threads::Threads)
pearch_configure_target(pearch_core)

# ---------------------------------------------------------------------------
# pearch executable
# ---------------------------------------------------------------------------
add_executable(pearch src/main.cpp)
target_link_libraries(pearch PRIVATE pearch_core)
pearch_configure_target(pearch)

if(WIN32)
    # Icon, version info and manifest (UTF-8 code page, long paths).
    set(PEARCH_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/pearch.ico")
    set(PEARCH_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/pearch.manifest")
    configure_file(resources/pearch.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/pearch.rc @ONLY)
    target_sources(pearch PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/pearch.rc)
    if(MSVC)
        # The manifest is embedded by the .rc file instead of the linker.
        target_link_options(pearch PRIVATE /MANIFEST:NO)
    endif()
    if(MINGW)
        target_link_options(pearch PRIVATE -static)
    endif()
endif()

install(TARGETS pearch RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests and benchmark
# ---------------------------------------------------------------------------
if(PEARCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PEARCH_BUILD_BENCH)
    add_executable(pearch_bench bench/bench.cpp)
    target_link_libraries(pearch_bench PRIVATE pearch_core)
    pearch_configure_target(pearch_bench)
endif()
