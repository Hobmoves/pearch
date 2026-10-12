add_executable(pearch_tests
    test_main.cpp
    test_text.cpp
    test_query.cpp
    test_search.cpp
    test_store.cpp
    test_scanner.cpp
    test_render.cpp
    test_matcher.cpp
)
target_link_libraries(pearch_tests PRIVATE pearch_core)
pearch_configure_target(pearch_tests)
add_test(NAME pearch_tests COMMAND pearch_tests)
