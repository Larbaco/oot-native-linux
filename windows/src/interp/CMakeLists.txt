# interp: the fallback interpreter (DESIGN D4), the same semantics as nxlift compiled natively. Needs the runtime's rt_* symbols.
add_library(nxw_interp STATIC src/interp.cpp)
target_include_directories(nxw_interp PUBLIC include PRIVATE ${NXW_SEM_OPS_DIR})
target_link_libraries(nxw_interp PUBLIC nxw_sem nxw_a64)
add_dependencies(nxw_interp nxw_sem_ops)

# Known answers for the semantics (no game data)
add_executable(interp-kat tests/kat.cpp)
target_link_libraries(interp-kat PRIVATE nxw_interp)
add_test(NAME interp.kat COMMAND interp-kat)
