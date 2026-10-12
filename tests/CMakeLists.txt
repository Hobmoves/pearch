cmake_minimum_required(VERSION 3.20)

add_executable(test_index
    tests/test_index.cpp
    ${CMAKE_SOURCE_DIR}/src/index/database.cpp
    ${CMAKE_SOURCE_DIR}/src/index/index.cpp
    ${CMAKE_SOURCE_DIR}/src/filesystem/scanner.cpp
)

target_include_directories(test_index PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Link SQLite the same way as the main target
if (TARGET SQLite::SQLite3)
    target_link_libraries(test_index PRIVATE SQLite::SQLite3)
elseif (TARGET SQLite3::SQLite3)
    target_link_libraries(test_index PRIVATE SQLite3::SQLite3)
elseif(TARGET sqlite3_static)
    target_link_libraries(test_index PRIVATE sqlite3_static)
elseif (DEFINED SQLite3_LIBRARY)
    target_include_directories(test_index PRIVATE ${SQLite3_INCLUDE_DIR})
    target_link_libraries(test_index PRIVATE ${SQLite3_LIBRARY})
else()
    target_link_libraries(test_index PRIVATE sqlite3)
endif()

target_link_libraries(test_index PRIVATE Shell32 Ole32)

add_test(NAME index_tests COMMAND test_index)
