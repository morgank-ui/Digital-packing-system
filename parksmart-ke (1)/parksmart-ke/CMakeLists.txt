# ParkSmart KE - CMake build (alternative to the Makefile)
#   cmake -B build && cmake --build build
#   ./build/parksmart --demo
cmake_minimum_required(VERSION 3.14)
project(ParkSmartKE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(parksmart src/main.cpp)
target_link_libraries(parksmart PRIVATE SQLite::SQLite3 Threads::Threads)
if(WIN32)
  target_link_libraries(parksmart PRIVATE ws2_32)   # sockets on Windows
endif()

# Self-checks:  ctest --test-dir build
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE SQLite::SQLite3 Threads::Threads)
add_test(NAME core COMMAND test_core)
