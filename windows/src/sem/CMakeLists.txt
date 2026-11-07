# sem: the instruction semantics, compiled natively (nxw_sem: the interpreter, the runtime, the Mac oracle) and by clang to one
# LLVM bitcode module (nxw_sem.bc) that nxlift inlines. The generated sem_ops.inc lists the ops that have semantics.
set(NXW_SEM_SOURCES src/int.cpp src/mem.cpp src/fp.cpp src/simd.cpp src/state.cpp src/sysreg.cpp)
# x86-64-v3 (AVX2 + FMA3) is the declared minimum CPU (DESIGN D6); FP contraction off so separate FMUL + FADD never fuse.
# On the Mac the same sources target arm64 (the swap-in harness, docs/m4-swapin.md); the bitcode's triple picks nxlift's target.
if(APPLE)
  set(NXW_SEM_ARCH "-mcpu=apple-m1" CACHE STRING "CPU flags for the semantics (native and bitcode)")
  set(NXW_SEM_TARGET "arm64-apple-macos13.0" CACHE STRING "Target triple of the semantics bitcode (and so of lifted code)")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(NXW_SEM_ARCH "-march=x86-64-v3" CACHE STRING "CPU flags for the semantics (native and bitcode)")
  set(NXW_SEM_TARGET "x86_64-pc-linux-gnu" CACHE STRING "Target triple of the semantics bitcode (and so of lifted code)")
else()
  set(NXW_SEM_ARCH "-march=x86-64-v3" CACHE STRING "CPU flags for the semantics (native and bitcode)")
  set(NXW_SEM_TARGET "x86_64-pc-windows-msvc" CACHE STRING "Target triple of the semantics bitcode (and so of lifted code)")
endif()
set(NXW_SEM_FLAGS ${NXW_SEM_ARCH} -ffp-contract=off -fno-math-errno)

add_library(nxw_sem STATIC ${NXW_SEM_SOURCES})
target_include_directories(nxw_sem PUBLIC include PRIVATE src)
target_link_libraries(nxw_sem PUBLIC nxw_abi nxw_a64)
target_compile_options(nxw_sem PRIVATE ${NXW_SEM_FLAGS})

# The ops with semantics, from the library's own symbols (NXW_SEM_OP(<Op>) lines, for the interpreter's dispatch)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
get_filename_component(NXW_CLANG_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(NXW_LLVM_NM llvm-nm HINTS "${NXW_CLANG_BIN}" REQUIRED)
set(NXW_SEM_OPS_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen CACHE INTERNAL "")
add_custom_command(
  OUTPUT ${NXW_SEM_OPS_DIR}/sem_ops.inc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${NXW_SEM_OPS_DIR}
  COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/gen_ops.py ${NXW_LLVM_NM} $<TARGET_FILE:nxw_sem>
          "$<TARGET_PROPERTY:nxw_a64,BINARY_DIR>/gen/nxw/a64_gen.hpp" ${NXW_SEM_OPS_DIR}/sem_ops.inc
  DEPENDS nxw_sem gen_ops.py
  COMMENT "Listing the ops with semantics")
add_custom_target(nxw_sem_ops DEPENDS ${NXW_SEM_OPS_DIR}/sem_ops.inc)

# The bitcode needs llvm-link from LLVM's development build (official windows-msvc release), which nxlift links against
set(NXW_LLVM_DIR "${CMAKE_SOURCE_DIR}/../toolchains/llvm-23.1.4" CACHE PATH "LLVM 23.1 development build (headers, static libraries)")
find_program(NXW_LLVM_LINK llvm-link HINTS "${NXW_LLVM_DIR}/bin" NO_DEFAULT_PATH)
if(NOT NXW_LLVM_LINK)
  message(STATUS "sem: no llvm-link in ${NXW_LLVM_DIR}/bin; no bitcode, no nxlift")
  return()
endif()

set(SEM_BC_DIR ${CMAKE_CURRENT_BINARY_DIR}/bc)
set(SEM_INCLUDES -I${CMAKE_CURRENT_SOURCE_DIR}/include -I${CMAKE_CURRENT_SOURCE_DIR}/src -I${CMAKE_SOURCE_DIR}/src/abi/include
    -I${CMAKE_SOURCE_DIR}/src/a64/include "-I$<TARGET_PROPERTY:nxw_a64,BINARY_DIR>/gen")
set(SEM_SYSROOT)
if(CMAKE_OSX_SYSROOT)
  set(SEM_SYSROOT -isysroot ${CMAKE_OSX_SYSROOT})
endif()
set(SEM_BCS)
foreach(src ${NXW_SEM_SOURCES})
  get_filename_component(stem ${src} NAME_WE)
  set(bc ${SEM_BC_DIR}/${stem}.bc)
  add_custom_command(
    OUTPUT ${bc}
    COMMAND ${CMAKE_CXX_COMPILER} --target=${NXW_SEM_TARGET} ${SEM_SYSROOT} -std=c++20 -O2 -DNDEBUG -fno-exceptions -fno-rtti
            -fno-stack-protector ${NXW_SEM_FLAGS} ${SEM_INCLUDES} ${NXW_PATH_MAP} -c -emit-llvm ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${bc}
            -MD -MF ${bc}.d
    DEPFILE ${bc}.d
    DEPENDS ${src} nxw_a64
    COMMAND_EXPAND_LISTS
    COMMENT "Compiling the semantics to bitcode: ${src}")
  list(APPEND SEM_BCS ${bc})
endforeach()
set(NXW_SEM_BC ${SEM_BC_DIR}/nxw_sem.bc CACHE INTERNAL "")
add_custom_command(
  OUTPUT ${NXW_SEM_BC}
  COMMAND ${NXW_LLVM_LINK} ${SEM_BCS} -o ${NXW_SEM_BC}
  DEPENDS ${SEM_BCS}
  COMMENT "Linking the semantics bitcode")
add_custom_target(nxw_sem_bc DEPENDS ${NXW_SEM_BC})
