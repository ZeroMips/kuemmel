cmake_minimum_required(VERSION 3.5.1)

find_package(PkgConfig REQUIRED)

set (CMAKE_CXX_STANDARD 11)

pkg_check_modules(SDL2 REQUIRED SDL2)

project(dupl C CXX)

ADD_DEFINITIONS(-DUNICODE)

find_program(FXC fxc DOC "fx compiler" PATHS "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.19041.0\\x64")
if(NOT FXC)
message(SEND_ERROR "Cannot find fxc.")
endif(NOT FXC)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/PixelShader.h
  COMMAND ${FXC} /nologo /EPS /Tps_4_0_level_9_1 $<IF:$<CONFIG:DEBUG>,/Od,/O1> /Zi /Fh ${CMAKE_BINARY_DIR}/PixelShader.h /Fd ${CMAKE_BINARY_DIR}/$PixelShader.pdb PixelShader.hlsl
  MAIN_DEPENDENCY PixelShader.hlsl
  COMMENT "HLSL ${FILE}"
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  VERBATIM)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/VertexShader.h
  COMMAND ${FXC} /nologo /EVS /Tvs_4_0_level_9_1 $<IF:$<CONFIG:DEBUG>,/Od,/O1> /Zi /Fh ${CMAKE_BINARY_DIR}/VertexShader.h /Fd ${CMAKE_BINARY_DIR}/$VertexShader.pdb VertexShader.hlsl
  MAIN_DEPENDENCY VertexShader.hlsl
  COMMENT "HLSL ${FILE}"
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  VERBATIM)

add_executable(dupl
  ${CMAKE_CURRENT_BINARY_DIR}/PixelShader.h
  ${CMAKE_CURRENT_BINARY_DIR}/VertexShader.h
  DuplicationManager.cpp
  main.cpp)

target_link_libraries(dupl 
  ${SDL2_LIBRARIES}
  dxgi
  d3d11)

target_include_directories(dupl PUBLIC
  ${SDL2_INCLUDE_DIRS})

target_compile_options(dupl PUBLIC
  ${SDL2_CFLAGS_OTHER})
