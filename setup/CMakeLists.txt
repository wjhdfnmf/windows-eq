add_executable(weq-setup
    src/main.cpp
    src/Devices.cpp
    src/Install.cpp
    src/Util.cpp
)

target_include_directories(weq-setup PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_compile_definitions(weq-setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(weq-setup PRIVATE ole32 advapi32 propsys)

# Ship the APO DLL next to the setup tool so "install" can find it.
add_dependencies(weq-setup WindowsEqApo)
add_custom_command(TARGET weq-setup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:WindowsEqApo> $<TARGET_FILE_DIR:weq-setup>
)
