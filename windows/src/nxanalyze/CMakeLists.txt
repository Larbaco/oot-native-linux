# nxanalyze: functions, CFGs, jump tables, indirect branches, landing pads, special functions, spin loops and HLE binding → program.db
add_library(nxanalyze STATIC src/insn.cpp src/discover.cpp src/cfg.cpp src/lsda.cpp src/hle.cpp src/traces.cpp src/analyze.cpp
            src/db.cpp src/report.cpp src/takeover.cpp)
target_include_directories(nxanalyze PUBLIC include PRIVATE src ${CMAKE_SOURCE_DIR}/src/nxload/tools)
target_link_libraries(nxanalyze PUBLIC nxload nxw_a64)

add_executable(nxanalyze-cli tools/nxanalyze.cpp)
set_target_properties(nxanalyze-cli PROPERTIES OUTPUT_NAME nxanalyze)
target_link_libraries(nxanalyze-cli PRIVATE nxanalyze)

add_executable(nxanalyze-tests tests/synthetic.cpp)
target_include_directories(nxanalyze-tests PRIVATE src ${CMAKE_SOURCE_DIR}/src/nxload/tests)
target_link_libraries(nxanalyze-tests PRIVATE nxanalyze)
add_test(NAME nxanalyze.synthetic COMMAND nxanalyze-tests)

# Reads the ExeFS folder named by NXW_DUMP at run time; skipped when it is not set
add_executable(nxanalyze-dump-tests tests/dump.cpp)
target_include_directories(nxanalyze-dump-tests PRIVATE ${CMAKE_SOURCE_DIR}/src/nxload/tests)
target_link_libraries(nxanalyze-dump-tests PRIVATE nxanalyze)
add_test(NAME nxanalyze.dump COMMAND nxanalyze-dump-tests)
set_tests_properties(nxanalyze.dump PROPERTIES SKIP_RETURN_CODE 77)

# The takeover check: program.db's host-bound set equals the runtime registry's (src/nxrt/gen/out/hle_inventory.tsv); synthetic cases always,
# the game's program.db files under _local\analyze when they exist
add_executable(nxanalyze-takeover-tests tests/takeover.cpp)
target_include_directories(nxanalyze-takeover-tests PRIVATE ${CMAKE_SOURCE_DIR}/src/nxload/tests)
target_link_libraries(nxanalyze-takeover-tests PRIVATE nxanalyze)
add_test(NAME nxanalyze.takeover COMMAND nxanalyze-takeover-tests ${CMAKE_SOURCE_DIR})
