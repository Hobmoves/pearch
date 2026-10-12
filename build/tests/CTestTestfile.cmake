# CMake generated Testfile for 
# Source directory: C:/projects/pearch/tests
# Build directory: C:/projects/pearch/build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test([=[index_tests]=] "C:/projects/pearch/build/tests/Debug/test_index.exe")
  set_tests_properties([=[index_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/projects/pearch/tests/CMakeLists.txt;28;add_test;C:/projects/pearch/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test([=[index_tests]=] "C:/projects/pearch/build/tests/Release/test_index.exe")
  set_tests_properties([=[index_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/projects/pearch/tests/CMakeLists.txt;28;add_test;C:/projects/pearch/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test([=[index_tests]=] "C:/projects/pearch/build/tests/MinSizeRel/test_index.exe")
  set_tests_properties([=[index_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/projects/pearch/tests/CMakeLists.txt;28;add_test;C:/projects/pearch/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test([=[index_tests]=] "C:/projects/pearch/build/tests/RelWithDebInfo/test_index.exe")
  set_tests_properties([=[index_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/projects/pearch/tests/CMakeLists.txt;28;add_test;C:/projects/pearch/tests/CMakeLists.txt;0;")
else()
  add_test([=[index_tests]=] NOT_AVAILABLE)
endif()
