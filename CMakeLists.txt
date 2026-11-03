cmake_minimum_required(VERSION 3.24)
project(gw2_agent_chat VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)

if(BUILD_TESTING)
  add_executable(sse_parser_test addon/sse_parser_test.cpp addon/sse_parser.cpp)
  target_include_directories(sse_parser_test PRIVATE addon)
  add_test(NAME sse_parser COMMAND sse_parser_test)
endif()

if(WIN32)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Nexus requires a Windows x64 build")
  endif()
  include(FetchContent)
  FetchContent_Declare(nexus_sdk
    URL https://codeload.github.com/RaidcoreGG/Nexus-API/tar.gz/e585220d489ed27feeb2dd208845f15472e0cf04
    URL_HASH SHA256=a2c263d81b509b444dabeaf8ff85fb4e11b65895a1e7ff7b25e4f572254dbbd2)
  FetchContent_Declare(imgui_source
    URL https://codeload.github.com/RaidcoreGG/imgui/tar.gz/58075c4414b985b352d10718b02a8c43f25efd7c
    URL_HASH SHA256=841321a17d00fd73256af6691dccd868ffb50dcff039d07e749c6f721365f53c)
  FetchContent_MakeAvailable(nexus_sdk imgui_source)
  add_library(nexus_imgui STATIC
    ${imgui_source_SOURCE_DIR}/imgui.cpp
    ${imgui_source_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_source_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_source_SOURCE_DIR}/imgui_widgets.cpp)
  target_include_directories(nexus_imgui PUBLIC ${imgui_source_SOURCE_DIR})
  add_library(gw2_agent_chat SHARED addon/addon.cpp addon/sse_parser.cpp)
  target_include_directories(gw2_agent_chat PRIVATE addon ${nexus_sdk_SOURCE_DIR})
  target_compile_definitions(gw2_agent_chat PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(gw2_agent_chat PRIVATE nexus_imgui winhttp user32 imm32 shell32 ole32)
  set_target_properties(gw2_agent_chat PROPERTIES PREFIX "" OUTPUT_NAME "gw2_agent_chat")
  if(MSVC)
    target_compile_options(gw2_agent_chat PRIVATE /W4 /permissive-)
  else()
    target_compile_options(gw2_agent_chat PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  install(TARGETS gw2_agent_chat RUNTIME DESTINATION .)
  install(FILES ${nexus_sdk_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME Nexus-API.txt)
  install(FILES ${imgui_source_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME ImGui.txt)
  install(FILES licenses/golang-x-sys.txt DESTINATION licenses)
endif()
