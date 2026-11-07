# s2tex: block-linear <-> linear for the Tegra X1 and the Switch 2 (docs/s2tex.md). Builds on its own too: cmake -S src/s2tex -B <dir>
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(s2tex LANGUAGES C)
  enable_testing()
endif()

add_library(s2tex STATIC s2tex.c)
target_include_directories(s2tex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(s2tex PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

# The real-data corpus stays outside the repository (game data); the test is skipped when it is absent
set(S2TEX_CORPUS_DIR "${CMAKE_SOURCE_DIR}/_local/s2tex/corpus" CACHE PATH "s2tex real-data corpus (manifest.tsv, <id>.bl, <id>.raw)")

add_executable(s2tex_tests tests/t_s2tex.c)
target_link_libraries(s2tex_tests PRIVATE s2tex)
add_test(NAME s2tex.synthetic COMMAND s2tex_tests)

# The reference: upstream nxnative's unpatched deswizzling at commit c1df4dd (MIT, tests/ref/LICENSE), copied unchanged
add_executable(s2tex_ref tests/t_ref.c tests/ref/tex/nvn_deswizzle.c tests/ref/nvn/nvn_tex.c)
target_include_directories(s2tex_ref PRIVATE tests/ref)
target_link_libraries(s2tex_ref PRIVATE s2tex)
if(MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set_source_files_properties(tests/ref/tex/nvn_deswizzle.c tests/ref/nvn/nvn_tex.c PROPERTIES COMPILE_OPTIONS "/w")
else()
  set_source_files_properties(tests/ref/tex/nvn_deswizzle.c tests/ref/nvn/nvn_tex.c PROPERTIES COMPILE_OPTIONS "-w")
endif()
add_test(NAME s2tex.tx1_vs_nxnative COMMAND s2tex_ref)

add_executable(s2tex_corpus tests/t_corpus.c)
target_link_libraries(s2tex_corpus PRIVATE s2tex)
if(WIN32)
  target_compile_definitions(s2tex_corpus PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
add_test(NAME s2tex.corpus COMMAND s2tex_corpus ${S2TEX_CORPUS_DIR})
set_tests_properties(s2tex.corpus PROPERTIES SKIP_RETURN_CODE 77)

if(APPLE)
  enable_language(OBJC)
  add_executable(s2tex_metal_check tests/metal_check.m)
  target_link_libraries(s2tex_metal_check PRIVATE s2tex "-framework Metal" "-framework Foundation")
  target_compile_options(s2tex_metal_check PRIVATE -fobjc-arc)
  add_test(NAME s2tex.metal COMMAND s2tex_metal_check ${CMAKE_CURRENT_SOURCE_DIR}/s2tex.metal ${S2TEX_CORPUS_DIR})
  set_tests_properties(s2tex.metal PROPERTIES SKIP_RETURN_CODE 77)
endif()
