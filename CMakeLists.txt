cmake_minimum_required(VERSION 3.20)
project(FrameSmooth C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no redistributable needed
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/out)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/out)

include(FetchContent)
# Headers only (SOURCE_SUBDIR points nowhere so nothing is built from these repos)
FetchContent_Declare(openxr
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.41.zip
  SOURCE_SUBDIR _headers_only)
FetchContent_Declare(openvr
  URL https://github.com/ValveSoftware/openvr/archive/refs/tags/v2.5.1.zip
  SOURCE_SUBDIR _headers_only)
FetchContent_Declare(vulkanheaders
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.290.zip
  SOURCE_SUBDIR _headers_only)
FetchContent_Declare(minhook
  URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.zip
  SOURCE_SUBDIR _headers_only)
FetchContent_MakeAvailable(openxr openvr vulkanheaders minhook)

add_library(FrameSmoothLayer SHARED layer/layer.cpp)
target_include_directories(FrameSmoothLayer PRIVATE ${openxr_SOURCE_DIR}/include common)
target_link_libraries(FrameSmoothLayer PRIVATE d3d11 d3d12 dxgi d3dcompiler)
target_compile_definitions(FrameSmoothLayer PRIVATE UNICODE _UNICODE)
target_compile_options(FrameSmoothLayer PRIVATE /W3 /permissive-)

# OpenVR hook (loaded into opted-in OpenVR games by the dashboard). Headers only: it uses the game's openvr_api.dll.
add_library(FrameSmoothOVR SHARED ovr/ovr.cpp
  ${minhook_SOURCE_DIR}/src/buffer.c ${minhook_SOURCE_DIR}/src/hook.c ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(FrameSmoothOVR PRIVATE ${openvr_SOURCE_DIR}/headers ${vulkanheaders_SOURCE_DIR}/include
  ${minhook_SOURCE_DIR}/include common)
target_link_libraries(FrameSmoothOVR PRIVATE d3d11 d3d12 dxgi d3dcompiler)
target_compile_definitions(FrameSmoothOVR PRIVATE UNICODE _UNICODE)
target_compile_options(FrameSmoothOVR PRIVATE /W3 /permissive-)

# Developer experiment (not installed): checks which texture types SteamVR accepts after Vulkan submits.
add_executable(fs_submit_test tools/submit_test.cpp)
target_include_directories(fs_submit_test PRIVATE ${openvr_SOURCE_DIR}/headers ${vulkanheaders_SOURCE_DIR}/include)
target_link_libraries(fs_submit_test PRIVATE ${openvr_SOURCE_DIR}/lib/win64/openvr_api.lib d3d11 dxgi)
set_target_properties(fs_submit_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/tools)

add_executable(FrameSmoothDashboard WIN32 dashboard/dashboard.cpp)
target_include_directories(FrameSmoothDashboard PRIVATE ${openvr_SOURCE_DIR}/headers common)
target_link_libraries(FrameSmoothDashboard PRIVATE ${openvr_SOURCE_DIR}/lib/win64/openvr_api.lib gdi32 user32 ole32 shell32 uuid)
target_compile_definitions(FrameSmoothDashboard PRIVATE UNICODE _UNICODE)
add_custom_command(TARGET FrameSmoothDashboard POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${openvr_SOURCE_DIR}/bin/win64/openvr_api.dll $<TARGET_FILE_DIR:FrameSmoothDashboard>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/scripts/install.ps1 ${CMAKE_SOURCE_DIR}/scripts/uninstall.ps1
          ${CMAKE_SOURCE_DIR}/scripts/Install.cmd ${CMAKE_SOURCE_DIR}/scripts/Uninstall.cmd ${CMAKE_SOURCE_DIR}/README.md
          $<TARGET_FILE_DIR:FrameSmoothDashboard>)
