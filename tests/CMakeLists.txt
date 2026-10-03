add_subdirectory(Core.Tests)
add_subdirectory(Ui.Tests)

# CTest. `ctest -L unit` is the fast set without GPU playback: PlayerCore.Tests without integration tests,
# PlayerUi.Tests and the app's --logictest. Plain `ctest` also runs the full PlayerCore.Tests (libffplay integration,
# so its non-integration tests run twice) and --headless-check on the local fixtures (skipped when missing).
set(WVP_TEST_OUT "${CMAKE_BINARY_DIR}/test-output")

add_test(NAME PlayerCore.Tests COMMAND PlayerCore.Tests)
set_tests_properties(PlayerCore.Tests PROPERTIES LABELS "integration" TIMEOUT 3600)

add_test(NAME PlayerCore.Tests.unit COMMAND PlayerCore.Tests --no-integration)
set_tests_properties(PlayerCore.Tests.unit PROPERTIES LABELS "unit")

add_test(NAME PlayerUi.Tests COMMAND PlayerUi.Tests)
set_tests_properties(PlayerUi.Tests PROPERTIES LABELS "unit")

add_test(NAME PlayerApp.logictest COMMAND PlayerApp --logictest)
set_tests_properties(PlayerApp.logictest PROPERTIES LABELS "unit")

foreach(fixture IN ITEMS "hw/mpeg2-telecine-1080i.ts" "arib/tbs-captions.ts")
    get_filename_component(stem "${fixture}" NAME_WE)
    add_test(NAME PlayerApp.headless.${stem}
        COMMAND "${CMAKE_COMMAND}"
            "-DEXE=$<TARGET_FILE:PlayerApp>"
            "-DMEDIA=${WVP_ROOT}/testmedia/${fixture}"
            "-DRESULT=${WVP_TEST_OUT}/headless-${stem}.json"
            -P "${WVP_ROOT}/cmake/run-headless.cmake")
    set_tests_properties(PlayerApp.headless.${stem} PROPERTIES
        LABELS "integration;headless" TIMEOUT 600 SKIP_REGULAR_EXPRESSION "WVP_SKIP:")
endforeach()
