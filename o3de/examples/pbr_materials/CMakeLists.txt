cmake_minimum_required(VERSION 3.22.1 FATAL_ERROR)
project(gz-rendering-pbr-materials)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(gz-rendering REQUIRED)

add_executable(pbr_materials Main.cc)

target_link_libraries(pbr_materials
  ${GZ-RENDERING_LIBRARIES}
)
