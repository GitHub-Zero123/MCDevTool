cmake_minimum_required(VERSION 3.15)

project(MC_DEV_TOOL_TEST LANGUAGES CXX C)

add_executable(test1 test1.cpp)
target_compile_features(test1 PRIVATE cxx_std_20)