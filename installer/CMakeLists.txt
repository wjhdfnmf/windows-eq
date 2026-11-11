# WindowsEQ-Setup.exe: per-user installer with WindowsEQ.exe embedded.

# Copy the freshly built app next to the resource script so the RCDATA path
# is fixed; the resource compiler tracks it and re-embeds on change.
set(PAYLOAD_PATH ${CMAKE_CURRENT_BINARY_DIR}/payload/WindowsEQ.exe)
set(ICON_PATH ${CMAKE_SOURCE_DIR}/gui/res/app.ico)
add_custom_command(
    OUTPUT ${PAYLOAD_PATH}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:WindowsEQ> ${PAYLOAD_PATH}
    DEPENDS WindowsEQ
    COMMENT "Embedding WindowsEQ.exe into the installer"
    VERBATIM)
add_custom_target(installer_payload DEPENDS ${PAYLOAD_PATH})

configure_file(res/Setup.rc.in ${CMAKE_CURRENT_BINARY_DIR}/Setup.rc @ONLY)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/Setup.rc PROPERTIES OBJECT_DEPENDS "${PAYLOAD_PATH};${ICON_PATH}")

add_executable(WindowsEQ-Setup WIN32
    src/Setup.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/Setup.rc
    res/Setup.manifest
)
add_dependencies(WindowsEQ-Setup installer_payload)
target_include_directories(WindowsEQ-Setup PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(WindowsEQ-Setup PRIVATE weqinstall weqrouter comctl32 shell32)

# Release builds also drop the installer into <repo>/dist for easy access.
add_custom_command(TARGET WindowsEQ-Setup POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:WindowsEQ-Setup> ${CMAKE_SOURCE_DIR}/dist/
    VERBATIM)
