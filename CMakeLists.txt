cmake_minimum_required(VERSION 3.20)
project(Renewed120 CXX)

# GTA SA is a 32-bit game: configure with -A Win32.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Renewed120 must be built as 32-bit (use: cmake -A Win32)")
endif()

add_library(Renewed120 SHARED src/main.cpp src/telemetry.cpp src/interp.cpp)
set_target_properties(Renewed120 PROPERTIES SUFFIX ".asi" PREFIX "" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(Renewed120 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(Renewed120 PRIVATE /W4 /MT$<$<CONFIG:Debug>:d>)
endif()
