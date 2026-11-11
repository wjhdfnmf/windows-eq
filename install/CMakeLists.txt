# Per-user install helpers shared by WindowsEQ.exe (uninstall, sign-in
# launch) and WindowsEQ-Setup.exe.
add_library(weqinstall STATIC src/Install.cpp)
target_include_directories(weqinstall PUBLIC include ${CMAKE_SOURCE_DIR}/common/include)
target_compile_definitions(weqinstall PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(weqinstall PUBLIC ole32 shell32 advapi32 uuid)
