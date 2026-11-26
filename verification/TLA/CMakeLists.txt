add_test(NAME tla_loadgen_protocol
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/loadgen_test.py
            $<TARGET_FILE:mex_loadgen>)
set_tests_properties(tla_loadgen_protocol PROPERTIES TIMEOUT 45)

add_test(NAME tla_fault_game
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/fault_game_test.py
            $<TARGET_FILE:mex_exchange>)
set_tests_properties(tla_fault_game PROPERTIES TIMEOUT 240)

add_executable(tla_bridge_replay bridge/replay.cpp)
target_link_libraries(tla_bridge_replay PRIVATE mex)
