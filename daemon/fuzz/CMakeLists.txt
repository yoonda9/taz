# libFuzzer harnesses for the frame reassembly and nanopb decode paths.
# Built only with the linux-fuzz preset (Clang + -fsanitize=fuzzer,address,undefined).
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(
        FATAL_ERROR
            "Fuzz targets require Clang. Use the linux-fuzz preset (CC=clang).")
endif()

foreach(_harness fuzz_reassembly fuzz_decode)
    add_executable(${_harness} ${_harness}.c)
    target_link_libraries(${_harness} PRIVATE taz_core taz_warnings)
    target_compile_options(${_harness} PRIVATE -fsanitize=fuzzer)
    target_link_options(${_harness} PRIVATE -fsanitize=fuzzer)
endforeach()
