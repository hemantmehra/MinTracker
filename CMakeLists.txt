cmake_minimum_required(VERSION 3.20)
project(MinTracker LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "MinTracker requires Windows.")
endif()

add_executable(MinTracker WIN32 src\\main.cpp)
target_compile_features(MinTracker PRIVATE cxx_std_17)
target_compile_definitions(MinTracker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(MinTracker PRIVATE comctl32 shell32 ole32 winsqlite3)

if(MSVC)
  target_compile_options(MinTracker PRIVATE /W4 /permissive- /utf-8)
endif()
