# nxload: NSO/MOD0/dynamic/symbols/relocations, .eh_frame, npdm, and rtld-style program linking
add_library(nxload STATIC src/lz4.cpp src/sha256.cpp src/module.cpp src/ehframe.cpp src/npdm.cpp src/program.cpp)
target_include_directories(nxload PUBLIC include)
target_link_libraries(nxload PUBLIC nxw_common PRIVATE $<$<PLATFORM_ID:Windows>:bcrypt>)

add_executable(nxload-info tools/nxload_info.cpp)
target_link_libraries(nxload-info PRIVATE nxload)

# GPL-2.0 ZBIC decompression is deliberately isolated in its own process.
add_executable(zbic-unpack tools/zbic_unpack.cpp src/zbic.c)
target_include_directories(zbic-unpack PRIVATE include)

add_executable(nxload-tests tests/synthetic.cpp)
target_link_libraries(nxload-tests PRIVATE nxload)
add_test(NAME nxload.synthetic COMMAND nxload-tests)

# External-dump census tests used a non-release game profile and are not part of
# the public source or test set. `nxload.synthetic` covers the format behavior.
