# Router core (shared by the weq-router CLI and the WindowsEQ GUI).
add_library(weqrouter STATIC
    src/Router.cpp
    src/AudioDevices.cpp
)
target_include_directories(weqrouter PUBLIC src ${CMAKE_SOURCE_DIR}/common/include)
target_compile_definitions(weqrouter PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(weqrouter PUBLIC eqdsp ole32 avrt propsys winmm)

add_executable(weq-router src/main.cpp)
target_link_libraries(weq-router PRIVATE weqrouter)
