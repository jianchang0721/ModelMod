cmake_minimum_required(VERSION 3.18)
project(modelmod CXX)

add_library(modelmod SHARED mod.cpp)

target_compile_options(modelmod PRIVATE -O2 -fvisibility=hidden)
target_link_libraries(modelmod log dl android)
