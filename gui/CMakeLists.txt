# WindowsEQ.exe: the control app (Win32 + Direct2D/DirectWrite, no third-party
# dependencies). It hosts the router in-process.
add_executable(WindowsEQ WIN32
    src/main.cpp
    src/Ui.cpp
    src/App.cpp
    src/Analyzer.cpp
    src/Storage.cpp
    src/Presets.cpp
    src/CrashHandler.cpp
    res/WindowsEQ.rc
    res/WindowsEQ.manifest
)
target_include_directories(WindowsEQ PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_link_libraries(WindowsEQ PRIVATE weqrouter weqinstall d2d1 dwrite dwmapi comdlg32 comctl32 shell32 shlwapi
    windowscodecs dbghelp)
# Ship a symbol file next to the exe (not in the installer) so crash dumps
# from release builds can be read. /OPT:REF,ICF keep the code identical to a
# build without symbols.
target_link_options(WindowsEQ PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
