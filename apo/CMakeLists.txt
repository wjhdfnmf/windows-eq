add_library(WindowsEqApo SHARED
    src/Dll.cpp
    src/WindowsEqApo.cpp
    src/ApoLog.cpp
    src/StatusShm.cpp
    WindowsEqApo.def
)

target_include_directories(WindowsEqApo PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_compile_definitions(WindowsEqApo PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# The SDK's V140 base-APO library was compiled against the DLL CRT; we link the
# static CRT on purpose (audiodg must not need a VC++ redistributable). The
# linker resolves those imports locally, which is what LNK4217 reports.
target_link_options(WindowsEqApo PRIVATE /IGNORE:4217)

target_link_libraries(WindowsEqApo PRIVATE
    eqdsp
    AudioBaseProcessingObjectV140
    audiomediatypecrt
    ole32
    advapi32
)
