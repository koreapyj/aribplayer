add_subdirectory(Core.LogicTests)

# CTest. `ctest -L unit` is the fast set without GPU playback: PlayerCore.LogicTests, PlayerUi.Tests and the app's
# --logictest. Plain `ctest` also runs the libffplay integration tests of PlayerCore.Tests and --headless-check on the
# local fixtures (skipped when missing).
add_test(NAME PlayerCore.LogicTests COMMAND PlayerCore.LogicTests)
set_tests_properties(PlayerCore.LogicTests PROPERTIES LABELS "unit")

if(WVP_PORTABLE_CORE)
    return()
endif()

add_subdirectory(Core.Tests)
add_subdirectory(Presenter.Tests)
add_subdirectory(Ui.Tests)

set(WVP_TEST_OUT "${CMAKE_BINARY_DIR}/test-output")

add_test(NAME PlayerCore.Tests COMMAND PlayerCore.Tests)
set_tests_properties(PlayerCore.Tests PROPERTIES LABELS "integration" TIMEOUT 3600)

add_test(NAME PlayerPresenter.Tests COMMAND PlayerPresenter.Tests)
set_tests_properties(PlayerPresenter.Tests PROPERTIES LABELS "integration" TIMEOUT 1800)

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
