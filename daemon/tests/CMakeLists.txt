# Unit tests are C++ (GoogleTest) calling the daemon's C headers. The C++
# language and standard are enabled at the top level (only when
# TAZ_BUILD_TESTS is on) so Conan builds gtest with the same settings.
find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

add_executable(
    taz_tests
    test_proto.cpp
    test_frame.cpp
    test_reassembly.cpp
    test_payload.cpp
    test_dispatch.cpp
    test_connection.cpp
    test_server.cpp
    test_config.cpp
    test_error.cpp)
# taz::proto comes in transitively via taz_core; taz_cxx_warnings carries C++
# warning flags without the C-only -Wstrict-prototypes/-Wmissing-prototypes.
target_link_libraries(taz_tests PRIVATE taz_core taz_cxx_warnings
                                        GTest::gtest_main)

gtest_discover_tests(taz_tests)
