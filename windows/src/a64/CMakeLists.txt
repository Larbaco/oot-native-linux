# a64: the A64 decoder and printer, generated from a64.tbl by gen_a64.py at build time
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(A64_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
  OUTPUT ${A64_GEN}/nxw/a64_gen.hpp ${A64_GEN}/a64_gen.cpp
  COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/gen_a64.py ${CMAKE_CURRENT_SOURCE_DIR}/a64.tbl
          ${CMAKE_CURRENT_SOURCE_DIR}/names.tbl ${A64_GEN}
  DEPENDS gen_a64.py a64.tbl names.tbl
  COMMENT "Generating the A64 decoder from a64.tbl")

add_library(nxw_a64 STATIC print.cpp ${A64_GEN}/a64_gen.cpp)
target_include_directories(nxw_a64 PUBLIC include ${A64_GEN} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nxw_a64 PUBLIC nxw_common)

add_executable(a64-xcheck tools/a64_xcheck.cpp)
target_link_libraries(a64-xcheck PRIVATE nxw_a64)
add_executable(a64-dis tools/a64_dis.cpp)
target_link_libraries(a64-dis PRIVATE nxw_a64)

add_executable(a64-tests tests/a64_tests.cpp)
target_link_libraries(a64-tests PRIVATE nxw_a64)
add_test(NAME a64-tests COMMAND a64-tests)
# The table's own encodings against LLVM (no game data). Pass the discovered
# executable explicitly; this test is disabled only when LLVM's tool is absent.
find_program(NXW_LLVM_OBJDUMP NAMES llvm-objdump llvm-objdump-23 llvm-objdump.exe)
if(NXW_LLVM_OBJDUMP)
  add_test(NAME a64-xcheck-patterns COMMAND a64-xcheck --patterns 200 --objdump "${NXW_LLVM_OBJDUMP}" --scratch ${CMAKE_CURRENT_BINARY_DIR})
else()
  add_test(NAME a64-xcheck-patterns COMMAND ${CMAKE_COMMAND} -E echo "SKIP: llvm-objdump not installed")
  set_tests_properties(a64-xcheck-patterns PROPERTIES DISABLED TRUE)
  message(STATUS "a64-xcheck-patterns disabled: llvm-objdump not installed")
endif()
