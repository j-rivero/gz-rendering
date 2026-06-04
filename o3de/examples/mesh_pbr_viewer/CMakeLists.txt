cmake_minimum_required(VERSION 3.22.1 FATAL_ERROR)
project(mesh-pbr-viewer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(gz-rendering REQUIRED)

add_executable(mesh_pbr_viewer Main.cc)

target_link_libraries(mesh_pbr_viewer
  ${GZ-RENDERING_LIBRARIES}
)
