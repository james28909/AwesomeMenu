cmake_minimum_required(VERSION 3.20)
project(comtext_flyout LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Build must target x64 (64-bit) because Explorer is 64-bit.")
endif()

add_library(comtext_flyout SHARED
  src/dllmain.cpp
  src/ClassFactory.h
  src/ClassFactory.cpp
  src/AwesomeMenuHost.h
  src/AwesomeMenuHost.cpp
  src/Guids.h
  src/Guids.cpp
  src/ExplorerCommands.h
  src/ExplorerCommands.cpp
  src/AwesomeMenuHost.def
)

target_compile_definitions(comtext_flyout PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_include_directories(comtext_flyout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(comtext_flyout PRIVATE
  shlwapi
  shell32
  ole32
  uuid
)

# Use a nice output name
set_target_properties(comtext_flyout PROPERTIES
  OUTPUT_NAME "AwesomeMenuHost"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

install(TARGETS comtext_flyout RUNTIME DESTINATION bin)