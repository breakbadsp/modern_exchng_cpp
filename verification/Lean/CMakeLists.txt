option(MEX_VERIFY_LEAN "Check the Lean model and replay its traces against MEX" OFF)
if(MEX_VERIFY_LEAN)
    find_program(MEX_LEAN_EXECUTABLE NAMES lean REQUIRED)
    add_executable(lean_matching_replay ../TLA/matching/replay.cpp)
    target_link_libraries(lean_matching_replay PRIVATE mex)
    add_executable(lean_depth_replay depth_replay.cpp)
    target_link_libraries(lean_depth_replay PRIVATE mex)
    add_test(NAME lean_verification
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py
            --lean ${MEX_LEAN_EXECUTABLE}
            --matching $<TARGET_FILE:lean_matching_replay>
            --depth $<TARGET_FILE:lean_depth_replay>)
    set_tests_properties(lean_verification PROPERTIES TIMEOUT 120)
endif()
