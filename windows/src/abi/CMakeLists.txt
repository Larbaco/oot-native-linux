add_library(nxw_abi INTERFACE)
target_include_directories(nxw_abi INTERFACE include)

add_executable(abi-check abi_check.c)
target_link_libraries(abi-check PRIVATE nxw_abi)
add_test(NAME abi.layout COMMAND abi-check)
