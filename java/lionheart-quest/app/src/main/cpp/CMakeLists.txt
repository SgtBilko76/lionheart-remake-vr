cmake_minimum_required(VERSION 3.22.1)
project(lionheartxr C)

find_package(OpenXR REQUIRED CONFIG)

add_library(lionheartxr SHARED lionheart_xr.c)
target_compile_options(lionheartxr PRIVATE -Wall -Wno-unused-function -O2)
target_link_libraries(lionheartxr OpenXR::openxr_loader android log EGL GLESv3 m)
