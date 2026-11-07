# nxshader: Vulkan SPIR-V and reflection for the game's shaders (Python; docs/m5-record-shaders.md). Tests only: the tool runs from the source folder.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME nxshader.unit COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nxshader.py)
  add_test(NAME nxshader.spirv_val_sample COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus_sample.py)
  set_tests_properties(nxshader.unit nxshader.spirv_val_sample PROPERTIES SKIP_RETURN_CODE 77)
endif()
