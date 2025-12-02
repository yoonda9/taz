cmake_minimum_required(VERSION 3.23)

project(tazer
    LANGUAGES C
)

add_executable(tazer)
target_sources(tazer PRIVATE src/main.c)

find_package(mongoose REQUIRED)
target_link_libraries(tazer mongoose::mongoose)

install(TARGETS tazer)
