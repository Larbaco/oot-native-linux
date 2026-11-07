cmake_minimum_required(VERSION 3.28)
project(oot_native_linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# The static C runtime (/MT) everywhere, as LLVM's libraries already require of nxlift: the tools the installer ships run on a PC without the
# Visual C++ Redistributable (docs/M7-REPORT.md). -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL gives the old /MD tree.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  add_compile_options(/W4 /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
  # The build folder's paths stay out of what is compiled (__FILE__, the semantics bitcode nxlift embeds): the shipped tools name no build PC's folders
  set(NXW_PATH_MAP "-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=" "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=" "-fdebug-compilation-dir=.")
  add_compile_options(${NXW_PATH_MAP})
endif()
if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()

# The game profile header (DM_GAME_* macros) is generated per game from games/<NXRT_GAME>/{game.tsv,profile.tsv}; one nxrt build folder per game
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NXRT_GAME "0400486024DEC000" CACHE STRING "the game profile (games/<id>) the runtime is built for")
set(NXRT_GAME_DIR "${CMAKE_SOURCE_DIR}/../games/${NXRT_GAME}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NXRT_GAME_DIR}/game.tsv" "${NXRT_GAME_DIR}/profile.tsv" "${CMAKE_SOURCE_DIR}/../tools/gen_game_profile.py")
execute_process(COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/../tools/gen_game_profile.py --game ${NXRT_GAME} --out ${CMAKE_BINARY_DIR}/gen/game_profile.h
                RESULT_VARIABLE _gp_rc ERROR_VARIABLE _gp_err)
if(NOT _gp_rc EQUAL 0)
  message(FATAL_ERROR "game_profile.h for ${NXRT_GAME}: ${_gp_err}")
endif()
include_directories(${CMAKE_BINARY_DIR}/gen)

# One folder per component; each owns its CMakeLists.txt
add_subdirectory(src/common)
add_subdirectory(src/abi)
add_subdirectory(src/nxload)
add_subdirectory(src/a64)
add_subdirectory(src/nxanalyze)
add_subdirectory(src/sem)
add_subdirectory(src/nxlift)
add_subdirectory(src/interp)
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(src/nxrt)
endif()
add_subdirectory(src/s2tex)
add_subdirectory(src/nxshader)   # the shader tool's tests (docs/m5-record-shaders.md)
add_subdirectory(src/coop)       # Link Play between our builds (docs/coop-step1.md)
