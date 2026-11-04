cmake_minimum_required(VERSION 3.24)
project(gw2_agent_chat VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
include(FetchContent)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_Declare(httplib_source
  URL https://codeload.github.com/yhirose/cpp-httplib/tar.gz/a7bc00e3307fecdb4d67545e93be7b88cfb1e186
  URL_HASH SHA256=ec95cbfb4dac7cd423d1f1056028341f836fa866b0d5a3d171635e74f13efd7a)
FetchContent_MakeAvailable(nlohmann_json httplib_source)

if(BUILD_TESTING)
  add_executable(opencode_protocol_test addon/opencode_protocol_test.cpp addon/opencode_protocol.cpp)
  target_include_directories(opencode_protocol_test PRIVATE addon)
  target_link_libraries(opencode_protocol_test PRIVATE nlohmann_json::nlohmann_json)
  add_test(NAME opencode_protocol COMMAND opencode_protocol_test)
  find_program(NODE_EXECUTABLE node)
  if(NODE_EXECUTABLE)
    add_executable(opencode_client_test addon/opencode_client_test.cpp addon/opencode_client.cpp addon/opencode_protocol.cpp)
    target_include_directories(opencode_client_test PRIVATE addon)
    target_link_libraries(opencode_client_test PRIVATE nlohmann_json::nlohmann_json httplib::httplib)
    add_test(NAME opencode_client
      COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/opencode_fixture.mjs $<TARGET_FILE:opencode_client_test>)
    set_tests_properties(opencode_client PROPERTIES TIMEOUT 20)
  endif()
endif()

if(WIN32)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Nexus requires a Windows x64 build")
  endif()
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
  add_library(gw2_agent_chat SHARED addon/addon.cpp addon/opencode_client.cpp addon/opencode_protocol.cpp)
  target_include_directories(gw2_agent_chat PRIVATE addon ${nexus_sdk_SOURCE_DIR})
  target_compile_definitions(gw2_agent_chat PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(gw2_agent_chat PRIVATE nexus_imgui nlohmann_json::nlohmann_json httplib::httplib user32 imm32 shell32 ole32)
  set_target_properties(gw2_agent_chat PROPERTIES PREFIX "" OUTPUT_NAME "gw2_agent_chat")
  if(MSVC)
    target_compile_options(gw2_agent_chat PRIVATE /W4 /permissive-)
  else()
    target_compile_options(gw2_agent_chat PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  install(TARGETS gw2_agent_chat RUNTIME DESTINATION .)
  install(FILES ${nexus_sdk_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME Nexus-API.txt)
  install(FILES ${imgui_source_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME ImGui.txt)
  install(FILES ${nlohmann_json_SOURCE_DIR}/LICENSE.MIT DESTINATION licenses RENAME nlohmann-json.txt)
  install(FILES ${httplib_source_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME cpp-httplib.txt)
endif()
