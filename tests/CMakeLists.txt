cmake_minimum_required(VERSION 3.15)

project(MC_DEV_TOOL_TEST LANGUAGES CXX C)

add_executable(test1 test1.cpp)
target_compile_features(test1 PRIVATE cxx_std_20)


add_executable(test2 test2.cpp)
target_compile_features(test2 PRIVATE cxx_std_23)
target_link_libraries(test2 PRIVATE mcdevtool)

add_executable(test3 test3.cpp)
target_compile_features(test3 PRIVATE cxx_std_23)