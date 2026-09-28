# Unit tests are C++ (GoogleTest) calling the daemon's C headers. The C++
# language and standard are enabled at the top level (only when
# TAZ_BUILD_TESTS is on) so Conan builds gtest with the same settings.
find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

add_executable(taz_tests test_proto.cpp test_frame.cpp)
target_link_libraries(taz_tests PRIVATE taz_core taz::proto GTest::gtest_main)

gtest_discover_tests(taz_tests)
