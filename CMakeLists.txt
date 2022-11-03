cmake_minimum_required(VERSION 3.5.1)

find_package(PkgConfig REQUIRED)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(SPICE REQUIRED spice-server)

project(kuemmel C CXX)

add_executable(kuemmel
  main.c
  IDXGIOutputDuplication/DuplicationManager.cpp
  display.cpp)

target_link_libraries(kuemmel 
  ${SPICE_LIBRARIES}
  ${GLIB2_LIBRARIES}
  Ws2_32
  d3d11)

target_include_directories(kuemmel PUBLIC
  ${SPICE_INCLUDE_DIRS}
  ${GLIB2_INCLUDE_DIRS})

target_compile_options(kuemmel PUBLIC
  ${SPICE_CFLAGS_OTHER}
  ${GLIB2_CFLAGS_OTHER})
