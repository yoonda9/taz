# Unit tests are C++ (GoogleTest) calling the daemon's C headers. C++ is
# enabled only here so the daemon itself stays a pure C build.
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

add_executable(tazer_tests test_proto.cpp)
target_link_libraries(tazer_tests PRIVATE tazer::proto GTest::gtest_main)
target_include_directories(tazer_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

gtest_discover_tests(tazer_tests)
