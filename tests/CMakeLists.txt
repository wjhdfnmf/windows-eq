add_executable(eqdsp_tests eqdsp_tests.cpp)
target_link_libraries(eqdsp_tests PRIVATE eqdsp)
add_test(NAME eqdsp_tests COMMAND eqdsp_tests)

add_executable(eqdsp_bench eqdsp_bench.cpp)
target_link_libraries(eqdsp_bench PRIVATE eqdsp)

add_executable(apo_smoke apo_smoke.cpp)
target_include_directories(apo_smoke PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
target_compile_definitions(apo_smoke PRIVATE UNICODE _UNICODE NOMINMAX
    WEQ_APO_DLL=L"$<TARGET_FILE:WindowsEqApo>")
target_link_libraries(apo_smoke PRIVATE ole32 audiomediatypecrt)
add_dependencies(apo_smoke WindowsEqApo)
add_test(NAME apo_smoke COMMAND apo_smoke)

add_executable(analyzer_test analyzer_test.cpp ${CMAKE_SOURCE_DIR}/gui/src/Analyzer.cpp)
target_include_directories(analyzer_test PRIVATE ${CMAKE_SOURCE_DIR}/gui/src)
add_test(NAME analyzer_test COMMAND analyzer_test)
