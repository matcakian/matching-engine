cmake_minimum_required(VERSION 3.20)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(matching_engine src/order_book.cpp)
target_include_directories(matching_engine PUBLIC include)
target_compile_options(matching_engine PRIVATE -Wall -Wextra -Wpedantic)

add_executable(matching_engine_demo src/main.cpp)
target_link_libraries(matching_engine_demo PRIVATE matching_engine)

add_executable(matching_engine_server src/http_server.cpp)
target_link_libraries(matching_engine_server PRIVATE matching_engine)
target_compile_options(matching_engine_server PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(matching_engine_tests tests/order_book_tests.cpp)
target_link_libraries(matching_engine_tests PRIVATE matching_engine)
add_test(NAME matching_engine_tests COMMAND matching_engine_tests)
