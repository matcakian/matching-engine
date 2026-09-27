cmake_minimum_required(VERSION 3.20)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)

add_library(matching_engine src/order_book.cpp)
target_include_directories(matching_engine PUBLIC include)
target_compile_options(matching_engine PRIVATE -Wall -Wextra -Wpedantic)

add_library(matching_engine_user_store src/user_store.cpp)
target_link_libraries(matching_engine_user_store PUBLIC matching_engine SQLite3::SQLite3)
target_compile_options(matching_engine_user_store PRIVATE -Wall -Wextra -Wpedantic)

add_executable(matching_engine_demo src/main.cpp)
target_link_libraries(matching_engine_demo PRIVATE matching_engine)

add_executable(matching_engine_server src/http_server.cpp)
target_link_libraries(matching_engine_server PRIVATE matching_engine_user_store)
target_compile_options(matching_engine_server PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(matching_engine_tests tests/order_book_tests.cpp)
target_link_libraries(matching_engine_tests PRIVATE matching_engine)
add_test(NAME matching_engine_tests COMMAND matching_engine_tests)

add_executable(user_store_tests tests/user_store_tests.cpp)
target_link_libraries(user_store_tests PRIVATE matching_engine_user_store)
add_test(NAME user_store_tests COMMAND user_store_tests)

add_executable(http_server_tests tests/http_server_tests.cpp)
target_link_libraries(http_server_tests PRIVATE matching_engine_user_store)
target_compile_options(http_server_tests PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME http_server_tests COMMAND http_server_tests)
set_tests_properties(http_server_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
