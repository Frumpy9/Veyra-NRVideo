find_package(Vulkan 1.1 REQUIRED COMPONENTS glslangValidator)
include(CTest)

set(veyra_linux_shaders)
foreach(stage motion interpolate)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${stage}.spv")
  add_custom_command(OUTPUT "${output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND Vulkan::glslangValidator -V --target-env vulkan1.1
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${stage}.comp" -o "${output}"
    DEPENDS "shaders/${stage}.comp" "shaders/common.glsl" VERBATIM)
  list(APPEND veyra_linux_shaders "${output}")
endforeach()
add_custom_target(veyra_linux_shaders DEPENDS ${veyra_linux_shaders})
add_library(veyra_linux_gpu STATIC VulkanInterpolator.cpp)
target_compile_features(veyra_linux_gpu PUBLIC cxx_std_20)
target_compile_options(veyra_linux_gpu PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
target_include_directories(veyra_linux_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(veyra_linux_gpu PUBLIC Vulkan::Vulkan)
add_dependencies(veyra_linux_gpu veyra_linux_shaders)

# This readback tool is a correctness harness, not the live playback pipeline.
add_executable(veyra_linux_interpolate InterpolateTool.cpp)
target_link_libraries(veyra_linux_interpolate PRIVATE veyra_linux_gpu)
target_compile_definitions(veyra_linux_interpolate PRIVATE
  VEYRA_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
if(BUILD_TESTING)
  add_executable(veyra_linux_gpu_tests tests/InterpolationTests.cpp)
  target_link_libraries(veyra_linux_gpu_tests PRIVATE veyra_linux_gpu)
  target_compile_definitions(veyra_linux_gpu_tests PRIVATE
    VEYRA_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
  add_test(NAME linux_vulkan_interpolation COMMAND veyra_linux_gpu_tests)
  set_tests_properties(linux_vulkan_interpolation PROPERTIES TIMEOUT 120)
endif()
