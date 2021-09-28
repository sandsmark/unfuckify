cmake_minimum_required(VERSION 3.13.4)

option(ENABLE_SANITIZERS "Enable runtime sanitizing (for development)")
if (ENABLE_SANITIZERS)
    message("Enabling asan and ubsan")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project(unfuckify)
find_package(Clang REQUIRED)
include_directories(${CLANG_INCLUDE_DIRS})
add_executable(unfuckify main.cpp test.cpp fuckery1.cpp fuckery2.cpp)
target_link_libraries(unfuckify ${LIBCLANG_LIBRARIES} libclang)
install(TARGETS unfuckify DESTINATION bin)
