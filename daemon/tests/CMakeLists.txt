# Unit tests are C++ (GoogleTest) calling the daemon's C headers. The C++
# language and standard are enabled at the top level (only when
# TAZER_BUILD_TESTS is on) so Conan builds gtest with the same settings.
find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

add_executable(tazer_tests test_proto.cpp)
target_link_libraries(tazer_tests PRIVATE tazer::proto GTest::gtest_main)
target_include_directories(tazer_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

gtest_discover_tests(tazer_tests)
