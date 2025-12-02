cmake_minimum_required(VERSION 3.23)
cmake_policy(SET CMP0091 NEW)

project(mongoose
    LANGUAGES C CXX
)

add_library( mongoose STATIC mongoose/mongoose.c)
target_sources(mongoose
    PUBLIC
        FILE_SET public_headers
        TYPE HEADERS
        FILES
            mongoose/mongoose.h
)

install(
    TARGETS mongoose
    FILE_SET public_headers
)
