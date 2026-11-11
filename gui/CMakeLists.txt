# WindowsEQ.exe: the control app (Win32 + Direct2D/DirectWrite, no third-party
# dependencies). It hosts the router in-process.
add_executable(WindowsEQ WIN32
    src/main.cpp
    src/Ui.cpp
    src/App.cpp
    src/Analyzer.cpp
    src/Storage.cpp
    src/Presets.cpp
    res/WindowsEQ.rc
    res/WindowsEQ.manifest
)
target_include_directories(WindowsEQ PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(WindowsEQ PRIVATE weqrouter weqinstall d2d1 dwrite dwmapi comdlg32 comctl32 shell32 shlwapi
    windowscodecs)
