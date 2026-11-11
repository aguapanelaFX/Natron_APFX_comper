# SOLVFX USD bridge: solvusd shared library (C interface) built against OpenUSD.
#   cmake -S tools/usdbridge -B build -Dpxr_DIR=<usd install>
cmake_minimum_required(VERSION 3.20)
project(solvusd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pxr REQUIRED CONFIG)

add_library(solvusd SHARED solvusd.cpp solvusd.h)
target_compile_definitions(solvusd PRIVATE SOLVUSD_BUILD NOMINMAX)
target_include_directories(solvusd PRIVATE ${PXR_INCLUDE_DIRS})
# monolithic build exports "usd_ms"; split builds export the separate libraries
if(TARGET usd_ms)
  target_link_libraries(solvusd PRIVATE usd_ms)
else()
  target_link_libraries(solvusd PRIVATE usd usdGeom usdShade usdLux usdUtils sdf tf vt gf)
endif()
if(MSVC)
  target_compile_options(solvusd PRIVATE /W3 /bigobj /Zc:inline- )
endif()
install(TARGETS solvusd RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
