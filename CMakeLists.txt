cmake_minimum_required(VERSION 3.18)

set(CMAKE_CXX_STANDARD 20)
set(CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(unfuckify)
find_package(Clang REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Core)
add_executable(unfuckify main.cpp)
target_link_libraries(unfuckify libclang Qt5::Core)
