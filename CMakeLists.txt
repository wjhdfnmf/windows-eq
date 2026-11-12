cmake_minimum_required(VERSION 3.20)
project(WindowsEQ VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: the APO DLL is loaded into audiodg.exe, so it must not depend on
# a VC++ redistributable being installed. Use it everywhere for consistency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    # /fp:precise (the default) is intentional: no fast-math, so filter math is
    # deterministic and the null test can be bit-exact.
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    # Debug info in Release too (doesn't change code generation), so crash
    # dumps can be symbolized with the .pdb.
    add_compile_options($<$<CONFIG:Release>:/Zi>)
endif()

add_subdirectory(dsp)
add_subdirectory(apo)
add_subdirectory(setup)
add_subdirectory(router)
add_subdirectory(install)
add_subdirectory(gui)
add_subdirectory(installer)

option(WINDOWSEQ_BUILD_TESTS "Build DSP unit tests and benchmark" ON)
if(WINDOWSEQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
