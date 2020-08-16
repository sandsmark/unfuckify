cmake_minimum_required(VERSION 3.18)

project(unfuckify)
find_package(Clang REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Core)
add_executable(unfuckify main.cpp)
add_executable(test_foo test_foo.cpp)
target_link_libraries(unfuckify libclang Qt5::Core)
target_link_libraries(test_foo libclang)
