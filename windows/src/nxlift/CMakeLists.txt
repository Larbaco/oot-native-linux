# nxlift: program.db functions -> LLVM IR (through the semantics bitcode) -> x86-64 COFF objects; and the JIT test harness.
# Needs LLVM's development build (NXW_LLVM_DIR, set in src/sem) and the semantics bitcode.
if(NOT TARGET nxw_sem_bc)
  message(STATUS "nxlift: skipped (no LLVM development build)")
  return()
endif()

# The official build links zlib and zstd into LLVMSupport without shipping them, and CMake would otherwise find MSYS2's MinGW
# copies: stand-ins instead (llvm_stubs.c), defined before LLVM's config looks for the real ones
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_zstd TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_LibXml2 TRUE)
if(WIN32)
  add_library(nxw_llvm_stubs STATIC src/llvm_stubs.c src/msvc_stl_shims.cpp)
else()
  add_library(nxw_llvm_stubs STATIC src/llvm_stubs.c)
endif()
set_target_properties(nxw_llvm_stubs PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
set_target_properties(ZLIB::ZLIB PROPERTIES INTERFACE_LINK_LIBRARIES nxw_llvm_stubs)
add_library(zstd::libzstd_static INTERFACE IMPORTED GLOBAL)
set_target_properties(zstd::libzstd_static PROPERTIES INTERFACE_LINK_LIBRARIES nxw_llvm_stubs)
find_package(LLVM 23.1 REQUIRED CONFIG PATHS "${NXW_LLVM_DIR}/lib/cmake/llvm" NO_DEFAULT_PATH)
message(STATUS "nxlift: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

# LLVM's release build uses the static C runtime (/MT, CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded in LLVMConfig.cmake) and no
# RTTI. Everything linked with it must use /MT too, so the libraries nxlift needs are compiled a second time here with /MT;
# the rest of the tree keeps the default /MD.
function(nxw_mt_twin target)
  get_target_property(srcs ${target} SOURCES)
  get_target_property(dir ${target} SOURCE_DIR)
  set(abs)
  foreach(s ${srcs})
    if(IS_ABSOLUTE "${s}")
      list(APPEND abs "${s}")
    else()
      list(APPEND abs "${dir}/${s}")
    endif()
  endforeach()
  add_library(${target}_mt STATIC ${abs})
  set_target_properties(${target}_mt PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
  target_include_directories(${target}_mt PUBLIC $<TARGET_PROPERTY:${target},INTERFACE_INCLUDE_DIRECTORIES>
                             PRIVATE $<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>)
  target_compile_options(${target}_mt PRIVATE $<TARGET_PROPERTY:${target},COMPILE_OPTIONS>)
  add_dependencies(${target}_mt ${target})
endfunction()
nxw_mt_twin(nxw_common)
nxw_mt_twin(nxw_a64)
target_link_libraries(nxw_a64_mt PUBLIC nxw_common_mt)
nxw_mt_twin(nxload)
target_link_libraries(nxload_mt PUBLIC nxw_common_mt PRIVATE $<$<PLATFORM_ID:Windows>:bcrypt>)
nxw_mt_twin(nxw_sem)
target_link_libraries(nxw_sem_mt PUBLIC nxw_abi nxw_a64_mt)
add_library(nxw_lift_db STATIC ${CMAKE_SOURCE_DIR}/src/nxanalyze/src/db.cpp)   # only program.db's reader from nxanalyze
set_target_properties(nxw_lift_db PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
target_include_directories(nxw_lift_db PUBLIC ${CMAKE_SOURCE_DIR}/src/nxanalyze/include)
target_link_libraries(nxw_lift_db PUBLIC nxw_common_mt)

# The target is the semantics bitcode's (src/sem NXW_SEM_TARGET): x86-64 for the game DLL, arm64 for the Mac swap-in harness
if(NXW_SEM_TARGET MATCHES "^(arm64|aarch64)")
  set(NXW_LIFT_TARGET_LIBS aarch64codegen aarch64asmparser aarch64desc aarch64info)
  set(NXW_LIFT_TARGET_DEF NXW_LIFT_AARCH64=1)
else()
  set(NXW_LIFT_TARGET_LIBS x86codegen x86asmparser x86desc x86info)
  set(NXW_LIFT_TARGET_DEF NXW_LIFT_X86=1)
endif()
llvm_map_components_to_libnames(NXW_LLVM_LIBS core bitreader bitwriter irreader linker passes ipo instcombine scalaropts
                                transformutils analysis target ${NXW_LIFT_TARGET_LIBS} support)
separate_arguments(NXW_LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")

add_library(nxlift STATIC src/inputs.cpp src/lift.cpp src/sem_blob.cpp)
set_target_properties(nxlift PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
target_include_directories(nxlift PUBLIC include)
target_include_directories(nxlift SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(nxlift PUBLIC ${NXW_LLVM_DEFS} ${NXW_LIFT_TARGET_DEF})
target_link_libraries(nxlift PUBLIC nxload_mt nxw_lift_db nxw_a64_mt nxw_sem_mt ${NXW_LLVM_LIBS} nxw_llvm_stubs)
# the bitcode is embedded (#embed, a C23 feature clang accepts in C++)
get_filename_component(SEM_BC_DIR_FOR_EMBED "${NXW_SEM_BC}" DIRECTORY)
target_compile_options(nxlift PRIVATE "--embed-dir=${SEM_BC_DIR_FOR_EMBED}" -Wno-c23-extensions)
set_source_files_properties(src/sem_blob.cpp PROPERTIES OBJECT_DEPENDS "${NXW_SEM_BC}")
add_dependencies(nxlift nxw_sem_bc)

add_executable(nxlift-cli tools/nxlift.cpp)
set_target_properties(nxlift-cli PROPERTIES OUTPUT_NAME nxlift MSVC_RUNTIME_LIBRARY MultiThreaded)
target_link_libraries(nxlift-cli PRIVATE nxlift)

# The JIT harness (real game functions: lifted vs interpreter vs host). The interpreter is compiled here with /MT as well.
# Windows only (it maps the modules with VirtualAlloc); the Mac checks lifted code inside the real game instead (src/swapin).
if(NOT WIN32)
  return()
endif()
add_library(nxw_interp_mt STATIC ${CMAKE_SOURCE_DIR}/src/interp/src/interp.cpp)
set_target_properties(nxw_interp_mt PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
target_include_directories(nxw_interp_mt PUBLIC ${CMAKE_SOURCE_DIR}/src/interp/include PRIVATE ${NXW_SEM_OPS_DIR})
target_link_libraries(nxw_interp_mt PUBLIC nxw_sem_mt nxw_a64_mt)
add_dependencies(nxw_interp_mt nxw_sem_ops)
llvm_map_components_to_libnames(NXW_LLVM_JIT_LIBS orcjit)
add_executable(nxlift-tests tests/lift_tests.cpp)
set_target_properties(nxlift-tests PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
target_compile_definitions(nxlift-tests PRIVATE "NXW_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
target_link_libraries(nxlift-tests PRIVATE nxlift nxw_interp_mt ${NXW_LLVM_JIT_LIBS})
target_link_options(nxlift-tests PRIVATE -Xlinker /STACK:0x4000000)   # deep guest call chains run on the host stack
add_test(NAME nxlift.dump COMMAND nxlift-tests)
set_tests_properties(nxlift.dump PROPERTIES SKIP_RETURN_CODE 77)
